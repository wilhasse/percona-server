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

#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace duckdb_se {
namespace {

struct ParsedUpdate {
  std::map<std::string, Cell> set_values;
  std::map<std::string, Cell> where_values;
};

size_t EstimateRowBytes(const Row &row) {
  size_t bytes = 0;
  for (const auto &cell : row) {
    bytes += cell.value.size();
  }
  return bytes;
}

std::string GetCurrentTimestampString() {
  auto now = std::chrono::system_clock::now();
  auto time_t_now = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) %
            1000;
  std::tm tm_buf{};
#ifdef _WIN32
  gmtime_s(&tm_buf, &time_t_now);
#else
  gmtime_r(&time_t_now, &tm_buf);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S") << '.'
      << std::setfill('0') << std::setw(3) << ms.count();
  return oss.str();
}

uint64_t HashRow(const Row &row) {
  uint64_t hash = 1469598103934665603ull;
  auto mix = [&](const void *data, size_t len) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < len; ++i) {
      hash ^= bytes[i];
      hash *= 1099511628211ull;
    }
  };

  for (const auto &cell : row) {
    uint8_t flags = 0;
    if (cell.is_null) flags |= 0x1;
    if (cell.is_blob) flags |= 0x2;
    mix(&flags, sizeof(flags));
    const uint64_t len = static_cast<uint64_t>(cell.value.size());
    mix(&len, sizeof(len));
    if (!cell.value.empty()) {
      mix(cell.value.data(), cell.value.size());
    }
  }
  return hash;
}

bool RowsEqual(const Row &lhs, const Row &rhs) {
  if (lhs.size() != rhs.size()) return false;
  for (size_t i = 0; i < lhs.size(); ++i) {
    const auto &l = lhs[i];
    const auto &r = rhs[i];
    if (l.is_null != r.is_null || l.is_blob != r.is_blob ||
        l.value != r.value) {
      return false;
    }
  }
  return true;
}

bool HasRowWithHash(const std::vector<Row> &rows,
                    const std::map<uint64_t, std::vector<size_t>> &hash_index,
                    uint64_t hash, const Row &row) {
  auto it = hash_index.find(hash);
  if (it == hash_index.end()) return false;
  for (size_t idx : it->second) {
    if (idx < rows.size() && RowsEqual(rows[idx], row)) {
      return true;
    }
  }
  return false;
}

bool HasBulkUpdateConflict(
    const std::vector<Row> &old_rows, const std::vector<Row> &new_rows,
    const std::map<uint64_t, std::vector<size_t>> &old_hashes,
    const std::map<uint64_t, std::vector<size_t>> &new_hashes,
    uint64_t old_hash, const Row &old_row, uint64_t new_hash,
    const Row &new_row) {
  if (HasRowWithHash(new_rows, new_hashes, old_hash, old_row)) {
    return true;
  }
  if (HasRowWithHash(old_rows, old_hashes, new_hash, new_row)) {
    return true;
  }
  return false;
}

bool match_ci(const std::string &sql, size_t pos, const std::string &token) {
  if (pos + token.size() > sql.size()) return false;
  for (size_t i = 0; i < token.size(); ++i) {
    const char lhs = static_cast<char>(
        std::toupper(static_cast<unsigned char>(sql[pos + i])));
    const char rhs = static_cast<char>(
        std::toupper(static_cast<unsigned char>(token[i])));
    if (lhs != rhs) return false;
  }
  return true;
}

void SkipSpaces(const std::string &sql, size_t *pos) {
  while (*pos < sql.size() &&
         std::isspace(static_cast<unsigned char>(sql[*pos]))) {
    ++(*pos);
  }
}

bool ConsumeKeyword(const std::string &sql, size_t *pos,
                    const std::string &keyword) {
  SkipSpaces(sql, pos);
  if (!match_ci(sql, *pos, keyword)) return false;
  const size_t end = *pos + keyword.size();
  if (end < sql.size() &&
      !std::isspace(static_cast<unsigned char>(sql[end]))) {
    return false;
  }
  *pos = end;
  return true;
}

bool PeekKeyword(const std::string &sql, size_t pos,
                 const std::string &keyword) {
  SkipSpaces(sql, &pos);
  if (!match_ci(sql, pos, keyword)) return false;
  const size_t end = pos + keyword.size();
  if (end < sql.size() &&
      !std::isspace(static_cast<unsigned char>(sql[end]))) {
    return false;
  }
  return true;
}

bool ConsumeChar(const std::string &sql, size_t *pos, char ch) {
  SkipSpaces(sql, pos);
  if (*pos >= sql.size() || sql[*pos] != ch) return false;
  ++(*pos);
  return true;
}

bool ParseIdentifier(const std::string &sql, size_t *pos, std::string *out) {
  SkipSpaces(sql, pos);
  if (*pos >= sql.size()) return false;
  out->clear();
  if (sql[*pos] == '"') {
    ++(*pos);
    std::string value;
    while (*pos < sql.size()) {
      char ch = sql[*pos];
      if (ch == '"') {
        if (*pos + 1 < sql.size() && sql[*pos + 1] == '"') {
          value.push_back('"');
          *pos += 2;
          continue;
        }
        ++(*pos);
        break;
      }
      value.push_back(ch);
      ++(*pos);
    }
    *out = std::move(value);
    return !out->empty();
  }

  size_t start = *pos;
  while (*pos < sql.size()) {
    char ch = sql[*pos];
    if (std::isspace(static_cast<unsigned char>(ch)) || ch == '.' ||
        ch == '=' || ch == ',' || ch == ')') {
      break;
    }
    ++(*pos);
  }
  if (*pos == start) return false;
  *out = sql.substr(start, *pos - start);
  return true;
}

bool ParseQualifiedIdentifier(const std::string &sql, size_t *pos,
                              TableId *table) {
  if (!table) return false;
  table->schema.clear();
  table->table.clear();

  std::string first;
  if (!ParseIdentifier(sql, pos, &first)) return false;
  SkipSpaces(sql, pos);
  if (*pos < sql.size() && sql[*pos] == '.') {
    ++(*pos);
    std::string second;
    if (!ParseIdentifier(sql, pos, &second)) return false;
    table->schema = std::move(first);
    table->table = std::move(second);
    return true;
  }
  table->table = std::move(first);
  return true;
}

bool ParseValue(const std::string &sql, size_t *pos, Cell *out) {
  SkipSpaces(sql, pos);
  if (*pos >= sql.size()) return false;

  if (match_ci(sql, *pos, "NULL")) {
    const size_t end = *pos + 4;
    if (end == sql.size() || sql[end] == ',' ||
        std::isspace(static_cast<unsigned char>(sql[end]))) {
      out->is_null = true;
      out->is_blob = false;
      out->value.clear();
      *pos = end;
      return true;
    }
  }

  if (sql[*pos] == '\'') {
    ++(*pos);
    std::string value;
    while (*pos < sql.size()) {
      char ch = sql[*pos];
      if (ch == '\'') {
        if (*pos + 1 < sql.size() && sql[*pos + 1] == '\'') {
          value.push_back('\'');
          *pos += 2;
          continue;
        }
        ++(*pos);
        break;
      }
      value.push_back(ch);
      ++(*pos);
    }
    out->is_null = false;
    out->is_blob = false;
    out->value = std::move(value);
    return true;
  }

  size_t start = *pos;
  while (*pos < sql.size()) {
    char ch = sql[*pos];
    if (ch == ',' || std::isspace(static_cast<unsigned char>(ch))) {
      break;
    }
    ++(*pos);
  }
  if (*pos == start) return false;
  out->is_null = false;
  out->is_blob = false;
  out->value = sql.substr(start, *pos - start);
  return true;
}

bool ParseAssignments(const std::string &sql, size_t *pos,
                      std::map<std::string, Cell> *out) {
  if (!out) return false;
  out->clear();
  while (true) {
    std::string column;
    if (!ParseIdentifier(sql, pos, &column)) return false;
    if (!ConsumeChar(sql, pos, '=')) return false;
    Cell value;
    if (!ParseValue(sql, pos, &value)) return false;
    (*out)[column] = value;

    SkipSpaces(sql, pos);
    if (*pos < sql.size() && sql[*pos] == ',') {
      ++(*pos);
      continue;
    }
    return true;
  }
}

bool ParseWhereClause(const std::string &sql, size_t *pos,
                      std::map<std::string, Cell> *out) {
  if (!out) return false;
  out->clear();
  while (true) {
    std::string column;
    if (!ParseIdentifier(sql, pos, &column)) return false;
    if (!ConsumeKeyword(sql, pos, "IS")) return false;
    if (!ConsumeKeyword(sql, pos, "NOT")) return false;
    if (!ConsumeKeyword(sql, pos, "DISTINCT")) return false;
    if (!ConsumeKeyword(sql, pos, "FROM")) return false;
    Cell value;
    if (!ParseValue(sql, pos, &value)) return false;
    (*out)[column] = value;

    SkipSpaces(sql, pos);
    if (PeekKeyword(sql, *pos, "AND")) {
      ConsumeKeyword(sql, pos, "AND");
      continue;
    }
    return true;
  }
}

bool ParseUpdateStatement(const std::string &sql, ParsedUpdate *parsed) {
  if (!parsed) return false;
  parsed->set_values.clear();
  parsed->where_values.clear();

  size_t pos = 0;
  if (!ConsumeKeyword(sql, &pos, "UPDATE")) return false;

  TableId table;
  if (!ParseQualifiedIdentifier(sql, &pos, &table)) return false;

  if (!ConsumeKeyword(sql, &pos, "SET")) return false;
  if (!ParseAssignments(sql, &pos, &parsed->set_values)) return false;
  if (!ConsumeKeyword(sql, &pos, "WHERE")) return false;
  if (!ParseWhereClause(sql, &pos, &parsed->where_values)) return false;
  return true;
}

bool ParseDeleteStatement(const std::string &sql,
                          std::map<std::string, Cell> *where_values) {
  if (!where_values) return false;
  where_values->clear();

  size_t pos = 0;
  if (!ConsumeKeyword(sql, &pos, "DELETE")) return false;
  if (!ConsumeKeyword(sql, &pos, "FROM")) return false;

  TableId table;
  if (!ParseQualifiedIdentifier(sql, &pos, &table)) return false;

  if (!ConsumeKeyword(sql, &pos, "WHERE")) return false;
  if (!ParseWhereClause(sql, &pos, where_values)) return false;
  return true;
}

bool BuildUpdateRows(const std::vector<std::string> &columns,
                     const std::map<std::string, size_t> &column_index,
                     const ParsedUpdate &parsed, Row *old_row, Row *new_row) {
  if (!old_row || !new_row) return false;
  if (columns.empty()) return false;

  old_row->assign(columns.size(), Cell{});
  new_row->assign(columns.size(), Cell{});
  std::vector<bool> old_set(columns.size(), false);
  std::vector<bool> new_set(columns.size(), false);

  for (const auto &entry : parsed.where_values) {
    auto it = column_index.find(entry.first);
    if (it == column_index.end()) return false;
    old_row->at(it->second) = entry.second;
    old_set[it->second] = true;
  }
  for (const auto &entry : parsed.set_values) {
    auto it = column_index.find(entry.first);
    if (it == column_index.end()) return false;
    new_row->at(it->second) = entry.second;
    new_set[it->second] = true;
  }

  for (size_t i = 0; i < columns.size(); ++i) {
    if (!old_set[i]) return false;
    if (!new_set[i]) {
      new_row->at(i) = old_row->at(i);
    }
  }

  return true;
}

bool BuildDeleteRow(const std::vector<std::string> &columns,
                    const std::map<std::string, size_t> &column_index,
                    const std::map<std::string, Cell> &where_values,
                    Row *old_row) {
  if (!old_row) return false;
  if (columns.empty()) return false;

  old_row->assign(columns.size(), Cell{});
  std::vector<bool> old_set(columns.size(), false);
  for (const auto &entry : where_values) {
    auto it = column_index.find(entry.first);
    if (it == column_index.end()) return false;
    old_row->at(it->second) = entry.second;
    old_set[it->second] = true;
  }
  for (bool set : old_set) {
    if (!set) return false;
  }
  return true;
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
  skip_txn_ = false;
  ResetBuffers();
  apply_txn_ = ApplyTxn{};
  bool already_applied = false;
  Status st = adapter_->IsGtidApplied(current_gtid_, &already_applied);
  if (!st.ok()) {
    in_txn_ = false;
    return st;
  }
  if (already_applied) {
    skip_txn_ = true;
  }
  return Status::Ok();
}

Status DuckDBBinlogApplier::AppendInsert(TableId table, Row row) {
  if (skip_txn_) {
    return Status::Ok();
  }
  RowBatch batch;
  batch.table = table;  // Copy table ID to batch
  batch.rows.emplace_back(std::move(row));
  return AppendInsertBatch(std::move(table), std::move(batch));
}

Status DuckDBBinlogApplier::AppendInsertBatch(TableId table, RowBatch batch) {
  if (!in_txn_) {
    return Status::Error(StatusCode::kInvalid, "No active binlog transaction");
  }
  if (skip_txn_) {
    return Status::Ok();
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
  if (skip_txn_) {
    return Status::Ok();
  }
  if (sql.empty()) {
    return Status::Error(StatusCode::kInvalid, "Empty UPDATE statement");
  }

  const size_t bytes_added = sql.size();
  ParsedUpdate parsed;
  Row old_row;
  Row new_row;
  bool parsed_ok = ParseUpdateStatement(sql, &parsed);
  bool use_bulk = false;
  bool columns_ready = false;
  std::vector<std::string> columns;
  std::map<std::string, size_t> column_index;
  uint64_t old_hash = 0;
  uint64_t new_hash = 0;

  if (parsed_ok) {
    const TableKey key{table.schema, table.table};
    auto &buffer = buffers_[key];
    if (buffer.columns_loaded) {
      columns = buffer.columns;
      column_index = buffer.column_index;
      columns_ready = true;
    } else {
      Status st = adapter_->GetTableColumns(table, &columns);
      if (st.ok()) {
        columns_ready = true;
        for (size_t i = 0; i < columns.size(); ++i) {
          column_index.emplace(columns[i], i);
        }
      }
    }
    if (columns_ready &&
        BuildUpdateRows(columns, column_index, parsed, &old_row, &new_row)) {
      use_bulk = true;
      old_hash = HashRow(old_row);
      new_hash = HashRow(new_row);
    }
  }

  const TableKey key{table.schema, table.table};
  if (use_bulk) {
    const auto &buffer = buffers_[key];
    if (!buffer.updates.statements.empty() ||
        HasBulkUpdateConflict(buffer.bulk_updates.old_rows,
                              buffer.bulk_updates.new_rows,
                              buffer.bulk_update_old_hashes,
                              buffer.bulk_update_new_hashes, old_hash,
                              old_row, new_hash, new_row)) {
      Status st = FlushBuffered(true);
      if (!st.ok()) return st;
    }
  } else {
    if (!buffers_[key].bulk_updates.old_rows.empty()) {
      Status st = FlushBuffered(true);
      if (!st.ok()) return st;
    }
  }

  auto &buffer = buffers_[key];
  if (!have_buffered_) {
    have_buffered_ = true;
    first_event_time_ = std::chrono::steady_clock::now();
  }
  if (use_bulk) {
    if (!buffer.columns_loaded && columns_ready) {
      buffer.columns = std::move(columns);
      buffer.column_index = std::move(column_index);
      buffer.columns_loaded = true;
    }
    if (buffer.bulk_updates.table.table.empty()) {
      buffer.bulk_updates.table = table;
    }
    const size_t row_index = buffer.bulk_updates.old_rows.size();
    buffer.bulk_updates.old_rows.push_back(std::move(old_row));
    buffer.bulk_updates.new_rows.push_back(std::move(new_row));
    buffer.bulk_update_old_hashes[old_hash].push_back(row_index);
    buffer.bulk_update_new_hashes[new_hash].push_back(row_index);
  } else {
    if (buffer.updates.table.table.empty()) {
      buffer.updates.table = table;
    }
    buffer.updates.statements.push_back(std::move(sql));
  }
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
  if (skip_txn_) {
    return Status::Ok();
  }
  if (sql.empty()) {
    return Status::Error(StatusCode::kInvalid, "Empty DELETE statement");
  }

  const size_t bytes_added = sql.size();
  std::map<std::string, Cell> where_values;
  Row old_row;
  bool parsed_ok = ParseDeleteStatement(sql, &where_values);
  bool use_bulk = false;
  bool columns_ready = false;
  std::vector<std::string> columns;
  std::map<std::string, size_t> column_index;

  if (parsed_ok) {
    const TableKey key{table.schema, table.table};
    auto &buffer = buffers_[key];
    if (buffer.columns_loaded) {
      columns = buffer.columns;
      column_index = buffer.column_index;
      columns_ready = true;
    } else {
      Status st = adapter_->GetTableColumns(table, &columns);
      if (st.ok()) {
        columns_ready = true;
        for (size_t i = 0; i < columns.size(); ++i) {
          column_index.emplace(columns[i], i);
        }
      }
    }
    if (columns_ready &&
        BuildDeleteRow(columns, column_index, where_values, &old_row)) {
      use_bulk = true;
    }
  }

  const TableKey key{table.schema, table.table};
  if (use_bulk && !buffers_[key].deletes.statements.empty()) {
    Status st = FlushBuffered(true);
    if (!st.ok()) return st;
  }
  if (!use_bulk && !buffers_[key].bulk_deletes.old_rows.empty()) {
    Status st = FlushBuffered(true);
    if (!st.ok()) return st;
  }

  auto &buffer = buffers_[key];
  if (!have_buffered_) {
    have_buffered_ = true;
    first_event_time_ = std::chrono::steady_clock::now();
  }
  if (use_bulk) {
    if (!buffer.columns_loaded && columns_ready) {
      buffer.columns = std::move(columns);
      buffer.column_index = std::move(column_index);
      buffer.columns_loaded = true;
    }
    if (buffer.bulk_deletes.table.table.empty()) {
      buffer.bulk_deletes.table = table;
    }
    buffer.bulk_deletes.old_rows.push_back(std::move(old_row));
  } else {
    if (buffer.deletes.table.table.empty()) {
      buffer.deletes.table = table;
    }
    buffer.deletes.statements.push_back(std::move(sql));
  }
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
  if (skip_txn_) {
    in_txn_ = false;
    skip_txn_ = false;
    apply_txn_ = ApplyTxn{};
    ResetBuffers();
    return Status::Ok();
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
  if (skip_txn_) {
    st = Status::Ok();
  } else if (apply_txn_.active) {
    st = adapter_->RollbackApplyTxn(apply_txn_);
  }

  apply_txn_ = ApplyTxn{};
  in_txn_ = false;
  skip_txn_ = false;
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
      TableId table = batch.table;  // Copy before move to avoid UB
      st = adapter_->AppendRows(apply_txn_, std::move(table), std::move(batch));
      if (!st.ok()) return st;
    }
    if (!buffer.bulk_updates.old_rows.empty()) {
      BulkUpdateBatch batch = std::move(buffer.bulk_updates);
      TableId table = batch.table;  // Copy before move to avoid UB
      st = adapter_->ApplyBulkUpdates(apply_txn_, std::move(table),
                                      std::move(batch));
      if (!st.ok()) return st;
    }
    if (!buffer.updates.statements.empty()) {
      UpdateBatch batch = std::move(buffer.updates);
      TableId table = batch.table;  // Copy before move to avoid UB
      st = adapter_->ApplyUpdates(apply_txn_, std::move(table), std::move(batch));
      if (!st.ok()) return st;
    }
    if (!buffer.bulk_deletes.old_rows.empty()) {
      BulkDeleteBatch batch = std::move(buffer.bulk_deletes);
      TableId table = batch.table;  // Copy before move to avoid UB
      st = adapter_->ApplyBulkDeletes(apply_txn_, std::move(table),
                                      std::move(batch));
      if (!st.ok()) return st;
    }
    if (!buffer.deletes.statements.empty()) {
      DeleteBatch batch = std::move(buffer.deletes);
      TableId table = batch.table;  // Copy before move to avoid UB
      st = adapter_->ApplyDeletes(apply_txn_, std::move(table), std::move(batch));
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
      "CREATE TABLE IF NOT EXISTS __repl_watermark ("
      "gtid VARCHAR PRIMARY KEY, "
      "commit_ts TIMESTAMP)";
  const std::string delete_sql =
      "DELETE FROM __repl_watermark WHERE gtid = '" +
      EscapeLiteral(current_gtid_.value) + "'";
  const std::string insert_sql =
      "INSERT INTO __repl_watermark (gtid, commit_ts) VALUES ('" +
      EscapeLiteral(current_gtid_.value) + "', '" +
      GetCurrentTimestampString() + "')";

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
