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

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "storage/duckdb/duckdb_adapter.h"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

using duckdb_se::Cell;
using duckdb_se::DDLChange;
using duckdb_se::DuckDBAdapter;
using duckdb_se::DuckDBConfig;
using duckdb_se::Gtid;
using duckdb_se::MySQLTableDef;
using duckdb_se::Row;
using duckdb_se::RowBatch;
using duckdb_se::SessionCtx;
using duckdb_se::Status;
using duckdb_se::TableId;

std::string TempDirectory() {
  try {
    return std::filesystem::temp_directory_path().string();
  } catch (...) {
    return "/tmp";
  }
}

std::string MakeTempPath(const std::string &prefix) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
  const int pid = _getpid();
#else
  const int pid = ::getpid();
#endif
  const std::string name =
      prefix + "_" + std::to_string(pid) + "_" + std::to_string(now);
  return (std::filesystem::path(TempDirectory()) / name).string();
}

void CleanupDuckdbFiles(const std::string &path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::remove(path + ".wal", ec);
}

void ExpectOk(const Status &st) { ASSERT_TRUE(st.ok()) << st.message; }

int64_t QueryCount(DuckDBAdapter &adapter, const std::string &sql) {
  SessionCtx ctx;
  auto result = adapter.ExecuteQuery(sql, ctx);
  if (!result.ok || !result.result) {
    ADD_FAILURE() << "DuckDB query failed: " << result.error;
    return 0;
  }
  auto chunk = result.result->Fetch();
  if (!chunk || chunk->size() == 0) {
    ADD_FAILURE() << "DuckDB query returned no rows";
    return 0;
  }
  return chunk->GetValue(0, 0).GetValue<int64_t>();
}

std::vector<std::string> ColumnNames(DuckDBAdapter &adapter,
                                     const std::string &table) {
  SessionCtx ctx;
  std::vector<std::string> names;
  auto result =
      adapter.ExecuteQuery("PRAGMA table_info('" + table + "')", ctx);
  if (!result.ok || !result.result) {
    ADD_FAILURE() << "PRAGMA table_info failed: " << result.error;
    return names;
  }

  while (true) {
    auto chunk = result.result->Fetch();
    if (!chunk || chunk->size() == 0) break;
    for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
      names.push_back(chunk->GetValue(1, row).GetValue<std::string>());
    }
  }
  return names;
}

MySQLTableDef MakeBaseTable(const std::string &name) {
  MySQLTableDef def;
  def.name = name;
  def.columns.push_back({"id", "INTEGER", true});
  def.columns.push_back({"val", "VARCHAR", false});
  return def;
}

Row MakeRow(const std::string &id, const std::string &val,
            const std::string &extra = "") {
  Row row;
  row.push_back(Cell{false, false, id});
  row.push_back(Cell{false, false, val});
  if (!extra.empty()) {
    row.push_back(Cell{false, false, extra});
  } else {
    row.push_back(Cell{true, false, ""});
  }
  return row;
}

TEST(DuckDBAdapterDDLTest, BasicDDLFlow) {
  const std::string path = MakeTempPath("duckdb_ddl_basic");
  CleanupDuckdbFiles(path);

  DuckDBAdapter adapter;
  DuckDBConfig cfg;
  cfg.read_only = false;
  ExpectOk(adapter.Init(path, cfg));

  ExpectOk(adapter.CreateTable(MakeBaseTable("t")));
  EXPECT_EQ(0, QueryCount(adapter, "SELECT COUNT(*) FROM t"));

  DDLChange alter;
  alter.type = DDLChange::Type::kAlter;
  alter.sql = "ALTER TABLE t ADD COLUMN extra VARCHAR";
  ExpectOk(adapter.ApplyDDL(alter));

  auto cols = ColumnNames(adapter, "t");
  ASSERT_EQ(3u, cols.size());
  EXPECT_EQ("id", cols[0]);
  EXPECT_EQ("val", cols[1]);
  EXPECT_EQ("extra", cols[2]);

  auto txn = adapter.BeginApplyTxn(Gtid{"gtid:ddl1"});
  ExpectOk(txn.status);
  RowBatch batch;
  batch.table = TableId{"", "t"};
  batch.rows.push_back(MakeRow("1", "alpha", "x"));
  TableId table_id = batch.table;  // Copy before move to avoid UB
  ExpectOk(adapter.AppendRows(txn, std::move(table_id), std::move(batch)));
  ExpectOk(adapter.CommitApplyTxn(txn));

  DDLChange rename;
  rename.type = DDLChange::Type::kRename;
  rename.table = TableId{"", "t"};
  rename.new_table = TableId{"", "t2"};
  ExpectOk(adapter.ApplyDDL(rename));

  EXPECT_EQ(1, QueryCount(adapter, "SELECT COUNT(*) FROM t2"));

  DDLChange truncate;
  truncate.type = DDLChange::Type::kTruncate;
  truncate.table = TableId{"", "t2"};
  ExpectOk(adapter.ApplyDDL(truncate));
  EXPECT_EQ(0, QueryCount(adapter, "SELECT COUNT(*) FROM t2"));

  DDLChange drop;
  drop.type = DDLChange::Type::kDrop;
  drop.table = TableId{"", "t2"};
  ExpectOk(adapter.ApplyDDL(drop));

  SessionCtx ctx;
  auto result = adapter.ExecuteQuery("SELECT COUNT(*) FROM t2", ctx);
  EXPECT_FALSE(result.ok);

  adapter.Shutdown();
  CleanupDuckdbFiles(path);
}

TEST(DuckDBAdapterDDLTest, CopyDDLReordersColumns) {
  const std::string path = MakeTempPath("duckdb_ddl_copy");
  CleanupDuckdbFiles(path);

  DuckDBAdapter adapter;
  DuckDBConfig cfg;
  cfg.read_only = false;
  ExpectOk(adapter.Init(path, cfg));

  ExpectOk(adapter.CreateTable(MakeBaseTable("t")));

  auto txn = adapter.BeginApplyTxn(Gtid{"gtid:ddl2"});
  ExpectOk(txn.status);
  RowBatch batch;
  batch.table = TableId{"", "t"};
  Row row;
  row.push_back(Cell{false, false, "1"});
  row.push_back(Cell{false, false, "alpha"});
  batch.rows.push_back(std::move(row));
  TableId table_id = batch.table;  // Copy before move to avoid UB
  ExpectOk(adapter.AppendRows(txn, std::move(table_id), std::move(batch)));
  ExpectOk(adapter.CommitApplyTxn(txn));

  MySQLTableDef new_def;
  new_def.name = "t";
  new_def.columns.push_back({"val", "VARCHAR", false});
  new_def.columns.push_back({"id", "INTEGER", true});
  new_def.columns.push_back({"extra", "VARCHAR", false});

  DDLChange copy;
  copy.type = DDLChange::Type::kAlter;
  copy.copy_ddl = true;
  copy.table = TableId{"", "t"};
  copy.new_def = std::move(new_def);
  ExpectOk(adapter.ApplyDDL(copy));

  auto cols = ColumnNames(adapter, "t");
  ASSERT_EQ(3u, cols.size());
  EXPECT_EQ("val", cols[0]);
  EXPECT_EQ("id", cols[1]);
  EXPECT_EQ("extra", cols[2]);

  EXPECT_EQ(1, QueryCount(adapter, "SELECT COUNT(*) FROM t"));

  adapter.Shutdown();
  CleanupDuckdbFiles(path);
}

}  // namespace
