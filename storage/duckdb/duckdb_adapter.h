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
  std::string ddl_sql;
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

struct Gtid {
  std::string value;
};

struct TableId {
  std::string schema;
  std::string table;
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

struct UpdateBatch {
  TableId table;
  std::vector<std::string> statements;
};

struct DeleteBatch {
  TableId table;
  std::vector<std::string> statements;
};

struct ApplyTxn {
  Gtid gtid;
  bool active{false};
  Status status;
  std::unique_ptr<duckdb::DuckDB> db;
  std::unique_ptr<duckdb::Connection> conn;
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

  ApplyTxn BeginApplyTxn(Gtid gtid);
  Status AppendRows(ApplyTxn &txn, TableId table, RowBatch batch);
  Status ApplyUpdates(ApplyTxn &txn, TableId table, UpdateBatch batch);
  Status ApplyDeletes(ApplyTxn &txn, TableId table, DeleteBatch batch);
  Status CommitApplyTxn(ApplyTxn &txn);
  Status RollbackApplyTxn(ApplyTxn &txn);

  QueryResult ExecuteQuery(std::string mysql_sql, SessionCtx ctx);
  Status GetLatestWatermark(Gtid *gtid);
  Status GetAppliedGtids(std::vector<Gtid> *gtids);

 private:
  Status EnsureInitialized() const;
  Status ExecuteDDL(const std::string &sql);
  Status ExecuteDDLOn(duckdb::Connection &conn, const std::string &sql);
  Status RenameTable(TableId from, TableId to);
  Status TruncateTable(TableId table);
  Status GetTableColumns(TableId table, std::vector<std::string> *columns);
  Status CopyTable(TableId source, const MySQLTableDef &target_def);
  std::string QuoteIdent(const std::string &name) const;
  std::string QualifiedName(const TableId &table) const;
  std::string EscapeLiteral(const std::string &value) const;
  std::string NormalizeDDL(const std::string &sql) const;
  DDLChange::Type InferDDLType(const std::string &sql) const;

  std::string db_path_;
  DuckDBConfig cfg_{};
  bool initialized_{false};
  std::unique_ptr<duckdb::DuckDB> db_;
  std::unique_ptr<duckdb::Connection> conn_;
};

}  // namespace duckdb_se

#endif  // PLUGIN_DUCKDB_ADAPTER_H_
