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

#include "storage/duckdb/duckdb_adapter.h"

#include <sstream>

namespace duckdb_se {

Status DuckDBAdapter::Init(std::string db_path, DuckDBConfig cfg) {
  if (initialized_) {
    return Status::Error(StatusCode::kAlreadyInitialized,
                         "DuckDBAdapter already initialized");
  }

  try {
    duckdb::DBConfig config(cfg.read_only);
    db_ = std::make_unique<duckdb::DuckDB>(db_path, &config);
    conn_ = std::make_unique<duckdb::Connection>(*db_);
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  db_path_ = std::move(db_path);
  cfg_ = cfg;
  initialized_ = true;
  return Status::Ok();
}

void DuckDBAdapter::Shutdown() {
  conn_.reset();
  db_.reset();
  initialized_ = false;
  db_path_.clear();
}

Status DuckDBAdapter::EnsureInitialized() const {
  if (!initialized_) {
    return Status::Error(StatusCode::kNotInitialized,
                         "DuckDBAdapter not initialized");
  }
  return Status::Ok();
}

std::string DuckDBAdapter::QuoteIdent(const std::string &name) const {
  std::string out;
  out.reserve(name.size() + 2);
  out.push_back('"');
  for (char ch : name) {
    if (ch == '"') out.push_back('"');
    out.push_back(ch);
  }
  out.push_back('"');
  return out;
}

std::string DuckDBAdapter::QualifiedName(const TableId &table) const {
  if (table.schema.empty()) return QuoteIdent(table.table);
  return QuoteIdent(table.schema) + "." + QuoteIdent(table.table);
}

Status DuckDBAdapter::CreateTable(MySQLTableDef def) {
  auto st = EnsureInitialized();
  if (!st.ok()) return st;

  std::string sql = def.ddl_sql;
  if (sql.empty()) {
    TableId id{def.schema, def.name};
    std::ostringstream ddl;
    ddl << "CREATE TABLE " << QualifiedName(id) << " (";
    for (size_t i = 0; i < def.columns.size(); ++i) {
      const auto &col = def.columns[i];
      ddl << QuoteIdent(col.name) << " " << col.type;
      if (col.not_null) ddl << " NOT NULL";
      if (i + 1 < def.columns.size()) ddl << ", ";
    }
    ddl << ")";
    sql = ddl.str();
  }

  try {
    auto result = conn_->Query(sql);
    if (result->HasError()) {
      return Status::Error(StatusCode::kDuckDBError, result->GetError());
    }
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return Status::Ok();
}

Status DuckDBAdapter::AlterTable(DDLChange change) {
  auto st = EnsureInitialized();
  if (!st.ok()) return st;
  if (change.sql.empty()) {
    return Status::Error(StatusCode::kInvalid, "Empty ALTER TABLE SQL");
  }

  try {
    auto result = conn_->Query(change.sql);
    if (result->HasError()) {
      return Status::Error(StatusCode::kDuckDBError, result->GetError());
    }
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return Status::Ok();
}

Status DuckDBAdapter::DropTable(std::string schema, std::string table) {
  auto st = EnsureInitialized();
  if (!st.ok()) return st;

  TableId id{std::move(schema), std::move(table)};
  const std::string sql = "DROP TABLE IF EXISTS " + QualifiedName(id);

  try {
    auto result = conn_->Query(sql);
    if (result->HasError()) {
      return Status::Error(StatusCode::kDuckDBError, result->GetError());
    }
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return Status::Ok();
}

ApplyTxn DuckDBAdapter::BeginApplyTxn(Gtid gtid) {
  ApplyTxn txn;
  txn.gtid = std::move(gtid);

  try {
    duckdb::DBConfig config(false);
    txn.db = std::make_unique<duckdb::DuckDB>(db_path_, &config);
    txn.conn = std::make_unique<duckdb::Connection>(*txn.db);
    auto result = txn.conn->Query("BEGIN TRANSACTION");
    if (result->HasError()) {
      txn.status = Status::Error(StatusCode::kDuckDBError, result->GetError());
      return txn;
    }
    txn.active = true;
    txn.status = Status::Ok();
  } catch (const std::exception &ex) {
    txn.status = Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return txn;
}

Status DuckDBAdapter::AppendRows(ApplyTxn &txn, TableId table, RowBatch batch) {
  if (!txn.active || !txn.conn) {
    return Status::Error(StatusCode::kInvalid, "Apply transaction not active");
  }

  try {
    std::unique_ptr<duckdb::Appender> appender;
    if (table.schema.empty()) {
      appender = std::make_unique<duckdb::Appender>(*txn.conn, table.table);
    } else {
      appender = std::make_unique<duckdb::Appender>(*txn.conn, table.schema,
                                                    table.table);
    }

    for (const auto &row : batch.rows) {
      appender->BeginRow();
      for (const auto &cell : row) {
        if (cell.is_null) {
          appender->Append(duckdb::Value());
        } else if (cell.is_blob) {
          appender->Append(duckdb::Value::BLOB(cell.value));
        } else {
          appender->Append(cell.value.c_str(),
                           static_cast<uint32_t>(cell.value.size()));
        }
      }
      appender->EndRow();
    }

    appender->Close();
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return Status::Ok();
}

Status DuckDBAdapter::ApplyUpdates(ApplyTxn &txn, TableId,
                                  UpdateBatch batch) {
  if (!txn.active || !txn.conn) {
    return Status::Error(StatusCode::kInvalid, "Apply transaction not active");
  }

  try {
    for (const auto &sql : batch.statements) {
      auto result = txn.conn->Query(sql);
      if (result->HasError()) {
        return Status::Error(StatusCode::kDuckDBError, result->GetError());
      }
    }
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return Status::Ok();
}

Status DuckDBAdapter::ApplyDeletes(ApplyTxn &txn, TableId,
                                  DeleteBatch batch) {
  if (!txn.active || !txn.conn) {
    return Status::Error(StatusCode::kInvalid, "Apply transaction not active");
  }

  try {
    for (const auto &sql : batch.statements) {
      auto result = txn.conn->Query(sql);
      if (result->HasError()) {
        return Status::Error(StatusCode::kDuckDBError, result->GetError());
      }
    }
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return Status::Ok();
}

Status DuckDBAdapter::CommitApplyTxn(ApplyTxn &txn) {
  if (!txn.active || !txn.conn) {
    return Status::Error(StatusCode::kInvalid, "Apply transaction not active");
  }

  try {
    auto result = txn.conn->Query("COMMIT");
    if (result->HasError()) {
      return Status::Error(StatusCode::kDuckDBError, result->GetError());
    }
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  txn.conn.reset();
  txn.db.reset();
  txn.active = false;
  return Status::Ok();
}

Status DuckDBAdapter::RollbackApplyTxn(ApplyTxn &txn) {
  if (!txn.active || !txn.conn) {
    return Status::Error(StatusCode::kInvalid, "Apply transaction not active");
  }

  try {
    auto result = txn.conn->Query("ROLLBACK");
    if (result->HasError()) {
      return Status::Error(StatusCode::kDuckDBError, result->GetError());
    }
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  txn.conn.reset();
  txn.db.reset();
  txn.active = false;
  return Status::Ok();
}

QueryResult DuckDBAdapter::ExecuteQuery(std::string mysql_sql,
                                       SessionCtx) {
  QueryResult result;
  auto st = EnsureInitialized();
  if (!st.ok()) {
    result.ok = false;
    result.error = st.message;
    return result;
  }

  try {
    result.result = conn_->SendQuery(mysql_sql);
    if (!result.result || result.result->HasError()) {
      result.ok = false;
      result.error = result.result ? result.result->GetError()
                                   : "DuckDB query failed";
      return result;
    }
  } catch (const std::exception &ex) {
    result.ok = false;
    result.error = ex.what();
    return result;
  }

  result.ok = true;
  return result;
}

}  // namespace duckdb_se
