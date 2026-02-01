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

#include <cctype>
#include <chrono>
#include <sstream>
#include <unordered_set>

namespace duckdb_se {
namespace {

bool IsMissingTableError(const std::string &error, const std::string &table) {
  return error.find("does not exist") != std::string::npos &&
         error.find(table) != std::string::npos;
}

bool StartsWithCI(const std::string &input, const char *prefix) {
  size_t i = 0;
  while (i < input.size() && std::isspace(static_cast<unsigned char>(input[i]))) {
    ++i;
  }
  size_t j = 0;
  while (prefix[j] != '\0') {
    if (i >= input.size()) return false;
    const char lhs = static_cast<char>(
        std::toupper(static_cast<unsigned char>(input[i++])));
    const char rhs = static_cast<char>(
        std::toupper(static_cast<unsigned char>(prefix[j++])));
    if (lhs != rhs) return false;
  }
  return true;
}

}  // namespace

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

Status DuckDBAdapter::ExecuteDDLOn(duckdb::Connection &conn,
                                   const std::string &sql) {
  if (sql.empty()) {
    return Status::Error(StatusCode::kInvalid, "Empty DDL statement");
  }
  try {
    auto result = conn.Query(sql);
    if (result->HasError()) {
      return Status::Error(StatusCode::kDuckDBError, result->GetError());
    }
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }
  return Status::Ok();
}

std::string DuckDBAdapter::DeltaTableName(const TableId &table) const {
  return "__delta_" + table.table;
}

Status DuckDBAdapter::EnsureDeltaTable(duckdb::Connection &conn,
                                       TableId table) {
  if (table.table.empty()) {
    return Status::Error(StatusCode::kInvalid, "Missing table name");
  }

  std::vector<std::string> columns;
  Status st = GetTableColumns(table, &columns);
  if (!st.ok()) return st;
  if (columns.empty()) {
    return Status::Error(StatusCode::kInvalid, "No columns for delta table");
  }

  std::string sql = "CREATE TABLE IF NOT EXISTS ";
  TableId delta{table.schema, DeltaTableName(table)};
  sql += QualifiedName(delta);
  sql += " (";
  for (size_t i = 0; i < columns.size(); ++i) {
    sql += QuoteIdent(columns[i]) + " VARCHAR";
    if (i + 1 < columns.size()) sql += ", ";
  }
  sql += ")";

  return ExecuteDDLOn(conn, sql);
}

Status DuckDBAdapter::ExecuteDDL(const std::string &sql) {
  auto st = EnsureInitialized();
  if (!st.ok()) return st;
  return ExecuteDDLOn(*conn_, sql);
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

std::string DuckDBAdapter::EscapeLiteral(const std::string &value) const {
  std::string out;
  out.reserve(value.size() + 4);
  for (char ch : value) {
    if (ch == '\'') out.push_back('\'');
    out.push_back(ch);
  }
  return out;
}

std::string DuckDBAdapter::NormalizeDDL(const std::string &sql) const {
  std::string out;
  out.reserve(sql.size());
  for (char ch : sql) {
    out.push_back(ch == '`' ? '"' : ch);
  }
  if (StartsWithCI(out, "CREATE TABLE")) {
    const auto pos = out.rfind(')');
    if (pos != std::string::npos) {
      out.resize(pos + 1);
    }
  }
  return out;
}

DDLChange::Type DuckDBAdapter::InferDDLType(const std::string &sql) const {
  if (StartsWithCI(sql, "CREATE TABLE")) return DDLChange::Type::kCreate;
  if (StartsWithCI(sql, "DROP TABLE")) return DDLChange::Type::kDrop;
  if (StartsWithCI(sql, "ALTER TABLE")) return DDLChange::Type::kAlter;
  if (StartsWithCI(sql, "RENAME TABLE")) return DDLChange::Type::kRename;
  if (StartsWithCI(sql, "TRUNCATE TABLE")) return DDLChange::Type::kTruncate;
  return DDLChange::Type::kUnknown;
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
  change.type = DDLChange::Type::kAlter;
  return ApplyDDL(std::move(change));
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

Status DuckDBAdapter::RenameTable(TableId from, TableId to) {
  auto st = EnsureInitialized();
  if (!st.ok()) return st;
  if (from.table.empty() || to.table.empty()) {
    return Status::Error(StatusCode::kInvalid, "Missing table name for rename");
  }
  if (!from.schema.empty() && !to.schema.empty() && from.schema != to.schema) {
    return Status::Error(StatusCode::kInvalid,
                         "Cross-schema rename not supported");
  }
  if (from.schema.empty()) {
    from.schema = to.schema;
  }
  const std::string sql = "ALTER TABLE " + QualifiedName(from) +
                          " RENAME TO " + QuoteIdent(to.table);
  return ExecuteDDL(sql);
}

Status DuckDBAdapter::TruncateTable(TableId table) {
  auto st = EnsureInitialized();
  if (!st.ok()) return st;
  if (table.table.empty()) {
    return Status::Error(StatusCode::kInvalid, "Missing table name to truncate");
  }
  const std::string sql = "DELETE FROM " + QualifiedName(table);
  return ExecuteDDL(sql);
}

Status DuckDBAdapter::GetTableColumns(TableId table,
                                      std::vector<std::string> *columns) {
  if (!columns) {
    return Status::Error(StatusCode::kInvalid, "Columns output is null");
  }
  columns->clear();

  auto st = EnsureInitialized();
  if (!st.ok()) return st;

  const std::string qualified = QualifiedName(table);
  const std::string sql =
      "PRAGMA table_info('" + EscapeLiteral(qualified) + "')";

  try {
    auto result = conn_->Query(sql);
    if (result->HasError()) {
      return Status::Error(StatusCode::kDuckDBError, result->GetError());
    }
    while (true) {
      auto chunk = result->Fetch();
      if (!chunk || chunk->size() == 0) {
        break;
      }
      for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
        auto name = chunk->GetValue(1, row);
        if (!name.IsNull()) {
          columns->push_back(name.ToString());
        }
      }
    }
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return Status::Ok();
}

Status DuckDBAdapter::CopyTable(TableId source,
                                const MySQLTableDef &target_def) {
  auto st = EnsureInitialized();
  if (!st.ok()) return st;
  if (source.table.empty()) {
    return Status::Error(StatusCode::kInvalid, "Missing source table");
  }
  if (target_def.name.empty()) {
    return Status::Error(StatusCode::kInvalid, "Missing target table name");
  }

  std::string target_schema = target_def.schema.empty() ? source.schema
                                                        : target_def.schema;
  if (!target_schema.empty() && !source.schema.empty() &&
      target_schema != source.schema) {
    return Status::Error(StatusCode::kInvalid,
                         "Copy DDL across schemas not supported");
  }

  const auto suffix = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const std::string temp_name = target_def.name + "__duckdb_tmp_" + suffix;
  const std::string backup_name = source.table + "__duckdb_old_" + suffix;

  st = ExecuteDDLOn(*conn_, "BEGIN TRANSACTION");
  if (!st.ok()) return st;
  bool committed = false;

  auto rollback = [&]() {
    if (!committed) {
      ExecuteDDLOn(*conn_, "ROLLBACK");
    }
  };

  MySQLTableDef temp_def = target_def;
  temp_def.schema = target_schema;
  temp_def.name = temp_name;

  st = CreateTable(std::move(temp_def));
  if (!st.ok()) {
    rollback();
    return st;
  }

  std::vector<std::string> old_cols;
  st = GetTableColumns(source, &old_cols);
  if (!st.ok()) {
    rollback();
    return st;
  }

  std::unordered_set<std::string> old_set(old_cols.begin(), old_cols.end());
  std::vector<std::string> copy_cols;
  if (!target_def.columns.empty()) {
    for (const auto &col : target_def.columns) {
      if (old_set.find(col.name) != old_set.end()) {
        copy_cols.push_back(col.name);
      }
    }
  } else {
    copy_cols = old_cols;
  }

  if (!copy_cols.empty()) {
    std::string cols_sql;
    for (size_t i = 0; i < copy_cols.size(); ++i) {
      cols_sql += QuoteIdent(copy_cols[i]);
      if (i + 1 < copy_cols.size()) cols_sql += ", ";
    }
    TableId temp_table{target_schema, temp_name};
    const std::string insert_sql =
        "INSERT INTO " + QualifiedName(temp_table) + " (" + cols_sql + ") " +
        "SELECT " + cols_sql + " FROM " + QualifiedName(source);
    st = ExecuteDDLOn(*conn_, insert_sql);
    if (!st.ok()) {
      rollback();
      return st;
    }
  }

  TableId backup{source.schema, backup_name};
  st = RenameTable(source, backup);
  if (!st.ok()) {
    rollback();
    return st;
  }

  TableId temp_table{target_schema, temp_name};
  TableId final_table{target_schema, target_def.name};
  st = RenameTable(temp_table, final_table);
  if (!st.ok()) {
    rollback();
    return st;
  }

  st = DropTable(backup.schema, backup.table);
  if (!st.ok()) {
    rollback();
    return st;
  }

  st = ExecuteDDLOn(*conn_, "COMMIT");
  if (!st.ok()) {
    rollback();
    return st;
  }
  committed = true;
  return Status::Ok();
}

Status DuckDBAdapter::ApplyDDL(DDLChange change) {
  auto st = EnsureInitialized();
  if (!st.ok()) return st;

  auto type = change.type;
  if (type == DDLChange::Type::kUnknown && !change.sql.empty()) {
    type = InferDDLType(change.sql);
  }

  switch (type) {
    case DDLChange::Type::kCreate:
      if (!change.new_def.name.empty()) {
        return CreateTable(std::move(change.new_def));
      }
      if (change.sql.empty()) {
        return Status::Error(StatusCode::kInvalid, "Missing CREATE TABLE SQL");
      }
      return ExecuteDDL(NormalizeDDL(change.sql));
    case DDLChange::Type::kDrop:
      if (!change.table.table.empty()) {
        return DropTable(change.table.schema, change.table.table);
      }
      if (change.sql.empty()) {
        return Status::Error(StatusCode::kInvalid, "Missing DROP TABLE SQL");
      }
      return ExecuteDDL(NormalizeDDL(change.sql));
    case DDLChange::Type::kAlter:
      if (change.copy_ddl) {
        return CopyTable(change.table, change.new_def);
      }
      if (change.sql.empty()) {
        return Status::Error(StatusCode::kInvalid, "Missing ALTER TABLE SQL");
      }
      return ExecuteDDL(NormalizeDDL(change.sql));
    case DDLChange::Type::kRename:
      if (!change.table.table.empty() && !change.new_table.table.empty()) {
        return RenameTable(change.table, change.new_table);
      }
      if (change.sql.empty()) {
        return Status::Error(StatusCode::kInvalid, "Missing RENAME TABLE SQL");
      }
      return ExecuteDDL(NormalizeDDL(change.sql));
    case DDLChange::Type::kTruncate:
      if (!change.table.table.empty()) {
        return TruncateTable(change.table);
      }
      if (change.sql.empty()) {
        return Status::Error(StatusCode::kInvalid, "Missing TRUNCATE TABLE SQL");
      }
      return ExecuteDDL(NormalizeDDL(change.sql));
    case DDLChange::Type::kUnknown:
    default:
      if (change.sql.empty()) {
        return Status::Error(StatusCode::kInvalid, "Missing DDL SQL");
      }
      return ExecuteDDL(NormalizeDDL(change.sql));
  }
}

ApplyTxn DuckDBAdapter::BeginApplyTxn(Gtid gtid) {
  ApplyTxn txn;
  txn.gtid = std::move(gtid);

  if (!db_) {
    txn.status = Status::Error(StatusCode::kNotInitialized,
                               "DuckDBAdapter not initialized");
    return txn;
  }

  try {
    // Use existing database instance instead of creating a new one.
    // DuckDB instances are isolated and don't share catalog changes.
    txn.conn = std::make_unique<duckdb::Connection>(*db_);
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

Status DuckDBAdapter::ApplyBulkUpdates(ApplyTxn &txn, TableId table,
                                       BulkUpdateBatch batch) {
  if (!txn.active || !txn.conn) {
    return Status::Error(StatusCode::kInvalid, "Apply transaction not active");
  }
  if (batch.new_rows.empty()) {
    return Status::Ok();
  }
  if (batch.old_rows.size() != batch.new_rows.size()) {
    return Status::Error(StatusCode::kInvalid,
                         "Bulk update row counts do not match");
  }

  Status st = EnsureDeltaTable(*txn.conn, table);
  if (!st.ok()) return st;

  try {
    TableId delta{table.schema, DeltaTableName(table)};
    std::unique_ptr<duckdb::Appender> appender;
    if (delta.schema.empty()) {
      appender = std::make_unique<duckdb::Appender>(*txn.conn, delta.table);
    } else {
      appender =
          std::make_unique<duckdb::Appender>(*txn.conn, delta.schema,
                                             delta.table);
    }

    for (const auto &row : batch.old_rows) {
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

    std::vector<std::string> columns;
    st = GetTableColumns(table, &columns);
    if (!st.ok()) return st;
    if (columns.empty()) {
      return Status::Error(StatusCode::kInvalid,
                           "No columns available for bulk update");
    }

    for (const auto &row : batch.old_rows) {
      if (row.size() != columns.size()) {
        return Status::Error(StatusCode::kInvalid,
                             "Bulk update row does not match column count");
      }
    }
    for (const auto &row : batch.new_rows) {
      if (row.size() != columns.size()) {
        return Status::Error(StatusCode::kInvalid,
                             "Bulk update row does not match column count");
      }
    }

    const std::string target = QualifiedName(table);
    const std::string delta_name = QualifiedName(delta);

    std::string join_sql;
    for (size_t i = 0; i < columns.size(); ++i) {
      if (i > 0) join_sql += " AND ";
      const std::string col = QuoteIdent(columns[i]);
      join_sql += target + "." + col + " IS NOT DISTINCT FROM " + delta_name +
                  "." + col;
    }

    const std::string delete_sql =
        "DELETE FROM " + target + " USING " + delta_name + " WHERE " + join_sql;
    st = ExecuteDDLOn(*txn.conn, delete_sql);
    if (!st.ok()) return st;

    const std::string cleanup_sql = "DELETE FROM " + delta_name;
    st = ExecuteDDLOn(*txn.conn, cleanup_sql);
    if (!st.ok()) return st;

    RowBatch insert_batch;
    insert_batch.table = table;
    insert_batch.rows = std::move(batch.new_rows);
    st = AppendRows(txn, std::move(table), std::move(insert_batch));
    if (!st.ok()) return st;
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return Status::Ok();
}

Status DuckDBAdapter::ApplyBulkDeletes(ApplyTxn &txn, TableId table,
                                       BulkDeleteBatch batch) {
  if (!txn.active || !txn.conn) {
    return Status::Error(StatusCode::kInvalid, "Apply transaction not active");
  }
  if (batch.old_rows.empty()) {
    return Status::Ok();
  }

  Status st = EnsureDeltaTable(*txn.conn, table);
  if (!st.ok()) return st;

  try {
    TableId delta{table.schema, DeltaTableName(table)};
    std::unique_ptr<duckdb::Appender> appender;
    if (delta.schema.empty()) {
      appender = std::make_unique<duckdb::Appender>(*txn.conn, delta.table);
    } else {
      appender =
          std::make_unique<duckdb::Appender>(*txn.conn, delta.schema,
                                             delta.table);
    }

    for (const auto &row : batch.old_rows) {
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

    std::vector<std::string> columns;
    st = GetTableColumns(table, &columns);
    if (!st.ok()) return st;
    if (columns.empty()) {
      return Status::Error(StatusCode::kInvalid,
                           "No columns available for bulk delete");
    }

    for (const auto &row : batch.old_rows) {
      if (row.size() != columns.size()) {
        return Status::Error(StatusCode::kInvalid,
                             "Bulk delete row does not match column count");
      }
    }

    const std::string target = QualifiedName(table);
    const std::string delta_name = QualifiedName(delta);

    std::string join_sql;
    for (size_t i = 0; i < columns.size(); ++i) {
      if (i > 0) join_sql += " AND ";
      const std::string col = QuoteIdent(columns[i]);
      join_sql += target + "." + col + " IS NOT DISTINCT FROM " + delta_name +
                  "." + col;
    }

    const std::string delete_sql =
        "DELETE FROM " + target + " USING " + delta_name + " WHERE " + join_sql;
    st = ExecuteDDLOn(*txn.conn, delete_sql);
    if (!st.ok()) return st;

    const std::string cleanup_sql = "DELETE FROM " + delta_name;
    st = ExecuteDDLOn(*txn.conn, cleanup_sql);
    if (!st.ok()) return st;
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

Status DuckDBAdapter::GetLatestWatermark(Gtid *gtid) {
  if (!gtid) {
    return Status::Error(StatusCode::kInvalid, "GTID output is null");
  }
  gtid->value.clear();

  auto st = EnsureInitialized();
  if (!st.ok()) return st;

  try {
    auto result = conn_->Query(
        "SELECT gtid FROM __repl_watermark "
        "ORDER BY commit_ts DESC LIMIT 1");
    if (result->HasError()) {
      if (IsMissingTableError(result->GetError(), "__repl_watermark")) {
        return Status::Ok();
      }
      return Status::Error(StatusCode::kDuckDBError, result->GetError());
    }
    auto chunk = result->Fetch();
    if (!chunk || chunk->size() == 0) {
      return Status::Ok();
    }
    auto val = chunk->GetValue(0, 0);
    if (!val.IsNull()) {
      gtid->value = val.ToString();
    }
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return Status::Ok();
}

Status DuckDBAdapter::GetAppliedGtids(std::vector<Gtid> *gtids) {
  if (!gtids) {
    return Status::Error(StatusCode::kInvalid, "GTID list output is null");
  }
  gtids->clear();

  auto st = EnsureInitialized();
  if (!st.ok()) return st;

  try {
    auto result = conn_->Query(
        "SELECT gtid FROM __repl_watermark ORDER BY commit_ts");
    if (result->HasError()) {
      if (IsMissingTableError(result->GetError(), "__repl_watermark")) {
        return Status::Ok();
      }
      return Status::Error(StatusCode::kDuckDBError, result->GetError());
    }
    while (true) {
      auto chunk = result->Fetch();
      if (!chunk || chunk->size() == 0) {
        break;
      }
      for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
        auto val = chunk->GetValue(0, row);
        if (!val.IsNull()) {
          Gtid entry;
          entry.value = val.ToString();
          gtids->push_back(std::move(entry));
        }
      }
    }
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return Status::Ok();
}

Status DuckDBAdapter::IsGtidApplied(const Gtid &gtid, bool *applied) {
  if (!applied) {
    return Status::Error(StatusCode::kInvalid, "Applied flag is null");
  }
  *applied = false;

  auto st = EnsureInitialized();
  if (!st.ok()) return st;

  try {
    const std::string sql =
        "SELECT 1 FROM __repl_watermark WHERE gtid = '" +
        EscapeLiteral(gtid.value) + "' LIMIT 1";
    auto result = conn_->Query(sql);
    if (result->HasError()) {
      if (IsMissingTableError(result->GetError(), "__repl_watermark")) {
        return Status::Ok();
      }
      return Status::Error(StatusCode::kDuckDBError, result->GetError());
    }
    auto chunk = result->Fetch();
    if (chunk && chunk->size() > 0) {
      *applied = true;
    }
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return Status::Ok();
}

}  // namespace duckdb_se
