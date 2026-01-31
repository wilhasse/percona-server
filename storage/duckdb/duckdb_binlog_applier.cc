/* Copyright (c) 2026, Percona LLC and/or its affiliates.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is designed to work with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have either included with
  the program or referenced in the documentation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "storage/duckdb/duckdb_binlog_applier.h"

#include <utility>

namespace duckdb_se {
namespace {

size_t EstimateRowBytes(const Row &row) {
  size_t bytes = 0;
  for (const auto &cell : row) {
    bytes += cell.value.size();
  }
  return bytes;
}

}  // namespace

DuckDBBinlogApplier::DuckDBBinlogApplier(DuckDBAdapter *adapter,
                                         Options options)
    : adapter_(adapter), options_(options) {}

Status DuckDBBinlogApplier::BeginTransaction(Gtid gtid) {
  if (!adapter_) {
    return Status::Error(StatusCode::kInvalid, "DuckDBAdapter is null");
  }
  if (in_txn_) {
    return Status::Error(StatusCode::kInvalid,
                         "Binlog transaction already active");
  }
  if (apply_txn_.active) {
    return Status::Error(StatusCode::kInvalid,
                         "Apply transaction still active");
  }

  current_gtid_ = std::move(gtid);
  in_txn_ = true;
  ResetBuffers();
  apply_txn_ = ApplyTxn{};
  return Status::Ok();
}

Status DuckDBBinlogApplier::AppendInsert(TableId table, Row row) {
  RowBatch batch;
  batch.table = std::move(table);
  batch.rows.emplace_back(std::move(row));
  return AppendInsertBatch(batch.table, std::move(batch));
}

Status DuckDBBinlogApplier::AppendInsertBatch(TableId table, RowBatch batch) {
  if (!in_txn_) {
    return Status::Error(StatusCode::kInvalid, "No active binlog transaction");
  }
  if (batch.rows.empty()) {
    return Status::Ok();
  }

  TableKey key{table.schema, table.table};
  auto &buffer = buffers_[key];
  if (buffer.inserts.table.table.empty()) {
    buffer.inserts.table = table;
  }
  if (!have_buffered_) {
    have_buffered_ = true;
    first_event_time_ = std::chrono::steady_clock::now();
  }

  size_t rows_added = 0;
  size_t bytes_added = 0;
  for (const auto &row : batch.rows) {
    ++rows_added;
    bytes_added += EstimateRowBytes(row);
  }

  buffer.inserts.rows.reserve(buffer.inserts.rows.size() + batch.rows.size());
  for (auto &row : batch.rows) {
    buffer.inserts.rows.push_back(std::move(row));
  }

  buffer.row_count += rows_added;
  buffer.byte_count += bytes_added;
  buffered_rows_ += rows_added;
  buffered_bytes_ += bytes_added;

  return FlushBuffered(false);
}

Status DuckDBBinlogApplier::AddUpdateStatement(TableId table, std::string sql) {
  if (!in_txn_) {
    return Status::Error(StatusCode::kInvalid, "No active binlog transaction");
  }
  if (sql.empty()) {
    return Status::Error(StatusCode::kInvalid, "Empty UPDATE statement");
  }

  TableKey key{table.schema, table.table};
  auto &buffer = buffers_[key];
  if (buffer.updates.table.table.empty()) {
    buffer.updates.table = table;
  }
  if (!have_buffered_) {
    have_buffered_ = true;
    first_event_time_ = std::chrono::steady_clock::now();
  }

  const size_t bytes_added = sql.size();
  buffer.updates.statements.push_back(std::move(sql));
  buffer.row_count += 1;
  buffer.byte_count += bytes_added;
  buffered_rows_ += 1;
  buffered_bytes_ += bytes_added;

  return FlushBuffered(false);
}

Status DuckDBBinlogApplier::AddDeleteStatement(TableId table, std::string sql) {
  if (!in_txn_) {
    return Status::Error(StatusCode::kInvalid, "No active binlog transaction");
  }
  if (sql.empty()) {
    return Status::Error(StatusCode::kInvalid, "Empty DELETE statement");
  }

  TableKey key{table.schema, table.table};
  auto &buffer = buffers_[key];
  if (buffer.deletes.table.table.empty()) {
    buffer.deletes.table = table;
  }
  if (!have_buffered_) {
    have_buffered_ = true;
    first_event_time_ = std::chrono::steady_clock::now();
  }

  const size_t bytes_added = sql.size();
  buffer.deletes.statements.push_back(std::move(sql));
  buffer.row_count += 1;
  buffer.byte_count += bytes_added;
  buffered_rows_ += 1;
  buffered_bytes_ += bytes_added;

  return FlushBuffered(false);
}

Status DuckDBBinlogApplier::CommitTransaction() {
  if (!in_txn_) {
    return Status::Error(StatusCode::kInvalid, "No active binlog transaction");
  }

  Status st = FlushBuffered(true);
  if (!st.ok()) {
    RollbackTransaction();
    return st;
  }

  st = EnsureApplyTxn();
  if (!st.ok()) {
    RollbackTransaction();
    return st;
  }

  st = ApplyWatermark();
  if (!st.ok()) {
    RollbackTransaction();
    return st;
  }

  st = adapter_->CommitApplyTxn(apply_txn_);
  if (!st.ok()) {
    RollbackTransaction();
    return st;
  }

  apply_txn_ = ApplyTxn{};
  in_txn_ = false;
  ResetBuffers();
  return Status::Ok();
}

Status DuckDBBinlogApplier::RollbackTransaction() {
  if (!in_txn_) {
    return Status::Error(StatusCode::kInvalid, "No active binlog transaction");
  }

  Status st = Status::Ok();
  if (apply_txn_.active) {
    st = adapter_->RollbackApplyTxn(apply_txn_);
  }

  apply_txn_ = ApplyTxn{};
  in_txn_ = false;
  ResetBuffers();
  return st;
}

Status DuckDBBinlogApplier::EnsureApplyTxn() {
  if (apply_txn_.active) {
    return Status::Ok();
  }
  if (!adapter_) {
    return Status::Error(StatusCode::kInvalid, "DuckDBAdapter is null");
  }

  apply_txn_ = adapter_->BeginApplyTxn(current_gtid_);
  if (!apply_txn_.status.ok()) {
    return apply_txn_.status;
  }
  if (!apply_txn_.active) {
    return Status::Error(StatusCode::kDuckDBError,
                         "DuckDB apply transaction not active");
  }
  return Status::Ok();
}

Status DuckDBBinlogApplier::FlushBuffered(bool force) {
  if (!have_buffered_) {
    return Status::Ok();
  }
  if (!force && !ShouldFlush()) {
    return Status::Ok();
  }

  Status st = EnsureApplyTxn();
  if (!st.ok()) {
    return st;
  }

  for (auto &entry : buffers_) {
    auto &buffer = entry.second;
    if (!buffer.inserts.rows.empty()) {
      RowBatch batch = std::move(buffer.inserts);
      st = adapter_->AppendRows(apply_txn_, batch.table, std::move(batch));
      if (!st.ok()) return st;
    }
    if (!buffer.updates.statements.empty()) {
      UpdateBatch batch = std::move(buffer.updates);
      st = adapter_->ApplyUpdates(apply_txn_, batch.table, std::move(batch));
      if (!st.ok()) return st;
    }
    if (!buffer.deletes.statements.empty()) {
      DeleteBatch batch = std::move(buffer.deletes);
      st = adapter_->ApplyDeletes(apply_txn_, batch.table, std::move(batch));
      if (!st.ok()) return st;
    }
  }

  ResetBuffers();
  return Status::Ok();
}

bool DuckDBBinlogApplier::ShouldFlush() const {
  if (!have_buffered_) {
    return false;
  }
  if (options_.max_rows > 0 && buffered_rows_ >= options_.max_rows) {
    return true;
  }
  if (options_.max_bytes > 0 && buffered_bytes_ >= options_.max_bytes) {
    return true;
  }
  if (options_.max_delay.count() > 0) {
    auto now = std::chrono::steady_clock::now();
    if (now - first_event_time_ >= options_.max_delay) {
      return true;
    }
  }
  return false;
}

void DuckDBBinlogApplier::ResetBuffers() {
  buffers_.clear();
  buffered_rows_ = 0;
  buffered_bytes_ = 0;
  have_buffered_ = false;
  first_event_time_ = std::chrono::steady_clock::time_point{};
}

Status DuckDBBinlogApplier::ApplyWatermark() {
  if (!apply_txn_.active || !apply_txn_.conn) {
    return Status::Error(StatusCode::kInvalid, "Apply transaction not active");
  }

  const std::string create_sql =
      "CREATE TABLE IF NOT EXISTS __duckdb_replication_watermark ("
      "id INTEGER PRIMARY KEY, "
      "gtid VARCHAR)";
  const std::string delete_sql =
      "DELETE FROM __duckdb_replication_watermark";
  const std::string insert_sql =
      "INSERT INTO __duckdb_replication_watermark (id, gtid) VALUES (1, '" +
      EscapeLiteral(current_gtid_.value) + "')";

  try {
    auto result = apply_txn_.conn->Query(create_sql);
    if (result->HasError()) {
      return Status::Error(StatusCode::kDuckDBError, result->GetError());
    }
    result = apply_txn_.conn->Query(delete_sql);
    if (result->HasError()) {
      return Status::Error(StatusCode::kDuckDBError, result->GetError());
    }
    result = apply_txn_.conn->Query(insert_sql);
    if (result->HasError()) {
      return Status::Error(StatusCode::kDuckDBError, result->GetError());
    }
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return Status::Ok();
}

std::string DuckDBBinlogApplier::EscapeLiteral(const std::string &value) const {
  std::string out;
  out.reserve(value.size() + 4);
  for (char ch : value) {
    if (ch == '\'') out.push_back('\'');
    out.push_back(ch);
  }
  return out;
}

}  // namespace duckdb_se
