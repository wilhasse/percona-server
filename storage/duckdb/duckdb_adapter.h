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

#ifndef PLUGIN_DUCKDB_ADAPTER_H_
#define PLUGIN_DUCKDB_ADAPTER_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "duckdb.hpp"

namespace duckdb_se {

enum class StatusCode {
  kOk,
  kInvalid,
  kNotInitialized,
  kAlreadyInitialized,
  kDuckDBError,
  kNotImplemented
};

struct Status {
  StatusCode code{StatusCode::kOk};
  std::string message;

  bool ok() const { return code == StatusCode::kOk; }

  static Status Ok() { return {}; }

  static Status Error(StatusCode code_in, std::string message_in) {
    Status st;
    st.code = code_in;
    st.message = std::move(message_in);
    return st;
  }
};

struct DuckDBConfig {
  bool read_only{true};
};

struct ColumnDef {
  std::string name;
  std::string type;
  bool not_null{false};
};

struct MySQLTableDef {
  std::string schema;
  std::string name;
  std::vector<ColumnDef> columns;
  std::vector<std::string> primary_key;
  bool allow_lossy{false};
  std::string ddl_sql;
};

struct TypeCheck {
  std::string type;
  bool lossy{false};
  std::string reason;
};

TypeCheck CheckMySQLTypeString(const std::string &mysql_type);

struct Gtid {
  std::string value;
};

struct TableId {
  std::string schema;
  std::string table;
};

struct DDLChange {
  enum class Type {
    kUnknown,
    kCreate,
    kDrop,
    kAlter,
    kRename,
    kTruncate
  };

  Type type{Type::kUnknown};
  std::string sql;
  TableId table;
  TableId new_table;
  MySQLTableDef new_def;
  bool copy_ddl{false};
};

struct Cell {
  bool is_null{false};
  bool is_blob{false};
  std::string value;
};

using Row = std::vector<Cell>;

struct RowBatch {
  TableId table;
  std::vector<Row> rows;
};

struct BulkUpdateBatch {
  TableId table;
  std::vector<Row> old_rows;
  std::vector<Row> new_rows;
};

struct BulkDeleteBatch {
  TableId table;
  std::vector<Row> old_rows;
};

struct UpdateBatch {
  TableId table;
  std::vector<std::string> statements;
};

struct DeleteBatch {
  TableId table;
  std::vector<std::string> statements;
};

struct ApplyOperationMetrics {
  uint64_t stage_ms{0};
  uint64_t merge_delete_ms{0};
};

struct ApplyTxn {
  Gtid gtid;
  bool active{false};
  Status status;
  std::unique_ptr<duckdb::DuckDB> db;
  std::unique_ptr<duckdb::Connection> conn;
  std::map<std::string, std::unique_ptr<duckdb::Appender>> appenders;
};

struct SessionCtx {
  std::string user;
};

struct QueryResult {
  bool ok{false};
  std::string error;
  std::unique_ptr<duckdb::QueryResult> result;
};

class DuckDBAdapter {
 public:
  Status Init(std::string db_path, DuckDBConfig cfg);
  void Shutdown();

  Status CreateTable(MySQLTableDef def);
  Status AlterTable(DDLChange change);
  Status DropTable(std::string schema, std::string table);
  Status ApplyDDL(DDLChange change);
  Status ApplyDDLInTxn(ApplyTxn &txn, DDLChange change);

  ApplyTxn BeginApplyTxn(Gtid gtid);
  enum class InsertDeltaMode { kInsert, kUpsert };
  Status AppendRows(ApplyTxn &txn, TableId table, RowBatch batch);
  Status ApplyInsertDelta(ApplyTxn &txn, TableId table, RowBatch batch,
                          InsertDeltaMode mode = InsertDeltaMode::kInsert,
                          ApplyOperationMetrics *metrics = nullptr);
  Status ApplyUpdates(ApplyTxn &txn, TableId table, UpdateBatch batch);
  Status ApplyDeletes(ApplyTxn &txn, TableId table, DeleteBatch batch);
  Status ApplyBulkUpdates(ApplyTxn &txn, TableId table, BulkUpdateBatch batch,
                          ApplyOperationMetrics *metrics = nullptr);
  Status ApplyBulkDeletes(ApplyTxn &txn, TableId table, BulkDeleteBatch batch,
                          ApplyOperationMetrics *metrics = nullptr);
  Status CloseAppenders(ApplyTxn &txn);
  Status CleanupInsertDeltaTables(const std::vector<TableId> &tables);
  Status CleanupDeltaTables(const std::vector<TableId> &tables);
  Status CommitApplyTxn(ApplyTxn &txn);
  Status RollbackApplyTxn(ApplyTxn &txn);

  QueryResult ExecuteQuery(std::string mysql_sql, SessionCtx ctx);
  Status GetLatestWatermark(Gtid *gtid);
  Status GetAppliedGtids(std::vector<Gtid> *gtids);
  Status IsGtidApplied(const Gtid &gtid, bool *applied);
  Status GetTableColumns(TableId table, std::vector<std::string> *columns);
  Status GetSchemaVersion(int64_t *version, bool *found);

 private:
  Status EnsureInitialized() const;
  Status ExecuteDDL(const std::string &sql);
  Status ExecuteDDLOn(duckdb::Connection &conn, const std::string &sql);
  Status ApplyDDLOn(duckdb::Connection &conn, DDLChange change,
                    bool manage_copy_txn);
  Status CreateTableOn(duckdb::Connection &conn, MySQLTableDef def);
  Status DropTableOn(duckdb::Connection &conn, std::string schema,
                     std::string table);
  Status RenameTableOn(duckdb::Connection &conn, TableId from, TableId to);
  Status TruncateTableOn(duckdb::Connection &conn, TableId table);
  Status CopyTableOn(duckdb::Connection &conn, TableId source,
                     const MySQLTableDef &target_def, bool manage_txn);
  Status EnsureDeltaTable(duckdb::Connection &conn, TableId table);
  Status EnsureDeltaTableWithColumns(duckdb::Connection &conn, TableId table,
                                     const std::vector<std::string> &columns);
  Status EnsureInsertDeltaTable(duckdb::Connection &conn, TableId table);
  std::string DeltaTableName(const TableId &table) const;
  std::string InsertDeltaTableName(const TableId &table) const;
  Status RenameTable(TableId from, TableId to);
  Status TruncateTable(TableId table);
 Status CopyTable(TableId source, const MySQLTableDef &target_def);
  std::string QuoteIdent(const std::string &name) const;
  std::string QualifiedName(const TableId &table) const;
  std::string EscapeLiteral(const std::string &value) const;
  std::string NormalizeDDL(const std::string &sql) const;
  DDLChange::Type InferDDLType(const std::string &sql) const;
  struct ColumnInfo {
    std::string name;
    std::string type;
    int pk{0};
  };
  Status GetTableInfo(TableId table, std::vector<ColumnInfo> *columns);
  Status GetTableInfoOn(duckdb::Connection &conn, TableId table,
                        std::vector<ColumnInfo> *columns);
  Status GetTableColumnsOn(duckdb::Connection &conn, TableId table,
                           std::vector<std::string> *columns);
  Status GetPrimaryKeyColumns(TableId table, std::vector<std::string> *columns);
  Status GetPrimaryKeyColumnsOn(duckdb::Connection &conn, TableId table,
                                std::vector<std::string> *columns);

  std::string db_path_;
  DuckDBConfig cfg_{};
  bool initialized_{false};
  std::unique_ptr<duckdb::DuckDB> db_;
  std::unique_ptr<duckdb::Connection> conn_;
};

}  // namespace duckdb_se

#endif  // PLUGIN_DUCKDB_ADAPTER_H_
