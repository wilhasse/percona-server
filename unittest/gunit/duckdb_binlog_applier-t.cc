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
#include <thread>
#include <vector>

#include "storage/duckdb/duckdb_adapter.h"
#include "storage/duckdb/duckdb_binlog_applier.h"

#ifdef _WIN32
#include <process.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using duckdb_se::BinlogApplierOptions;
using duckdb_se::Cell;
using duckdb_se::DuckDBAdapter;
using duckdb_se::DuckDBBinlogApplier;
using duckdb_se::DuckDBConfig;
using duckdb_se::Gtid;
using duckdb_se::MySQLTableDef;
using duckdb_se::Row;
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

MySQLTableDef MakeSimpleTable() {
  MySQLTableDef def;
  def.name = "t";
  def.columns.push_back({"id", "INTEGER", true});
  def.columns.push_back({"val", "VARCHAR", false});
  return def;
}

Row MakeRow(const std::string &id, const std::string &val) {
  Row row;
  row.push_back(Cell{false, false, id});
  row.push_back(Cell{false, false, val});
  return row;
}

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

TEST(DuckDBBinlogApplierTest, RestartPersistsDataAndWatermark) {
  const std::string path = MakeTempPath("duckdb_restart");
  CleanupDuckdbFiles(path);

  DuckDBAdapter adapter;
  DuckDBConfig cfg;
  cfg.read_only = false;
  ExpectOk(adapter.Init(path, cfg));
  ExpectOk(adapter.CreateTable(MakeSimpleTable()));

  DuckDBBinlogApplier applier(&adapter);
  ExpectOk(applier.BeginTransaction(Gtid{"gtid:1"}));
  ExpectOk(applier.AppendInsert(TableId{"", "t"}, MakeRow("1", "alpha")));
  ExpectOk(applier.CommitTransaction());
  adapter.Shutdown();

  DuckDBAdapter reader;
  DuckDBConfig ro_cfg;
  ro_cfg.read_only = true;
  ExpectOk(reader.Init(path, ro_cfg));
  EXPECT_EQ(1, QueryCount(reader, "SELECT COUNT(*) FROM t"));

  Gtid latest;
  ExpectOk(reader.GetLatestWatermark(&latest));
  EXPECT_EQ("gtid:1", latest.value);
  reader.Shutdown();

  CleanupDuckdbFiles(path);
}

TEST(DuckDBBinlogApplierTest, KillDuringApplyKeepsCommittedData) {
#ifdef _WIN32
  GTEST_SKIP() << "Process kill test not supported on Windows.";
#else
  const std::string path = MakeTempPath("duckdb_kill");
  CleanupDuckdbFiles(path);

  DuckDBAdapter adapter;
  DuckDBConfig cfg;
  cfg.read_only = false;
  ExpectOk(adapter.Init(path, cfg));
  ExpectOk(adapter.CreateTable(MakeSimpleTable()));

  DuckDBBinlogApplier applier(&adapter);
  ExpectOk(applier.BeginTransaction(Gtid{"gtid:1"}));
  ExpectOk(applier.AppendInsert(TableId{"", "t"}, MakeRow("1", "alpha")));
  ExpectOk(applier.CommitTransaction());
  adapter.Shutdown();

  int pipe_fds[2];
  ASSERT_EQ(0, ::pipe(pipe_fds));
  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    ::close(pipe_fds[0]);
    DuckDBAdapter child_adapter;
    DuckDBConfig child_cfg;
    child_cfg.read_only = false;
    auto st = child_adapter.Init(path, child_cfg);
    if (!st.ok()) _exit(2);

    BinlogApplierOptions options;
    options.max_rows = 1;
    DuckDBBinlogApplier child_applier(&child_adapter, options);
    st = child_applier.BeginTransaction(Gtid{"gtid:2"});
    if (!st.ok()) _exit(3);
    st = child_applier.AppendInsert(TableId{"", "t"}, MakeRow("2", "beta"));
    if (!st.ok()) _exit(4);

    char ready = '1';
    (void)!::write(pipe_fds[1], &ready, 1);
    ::close(pipe_fds[1]);
    for (;;) {
      ::pause();
    }
  }

  ::close(pipe_fds[1]);
  char ready = 0;
  ASSERT_EQ(1, ::read(pipe_fds[0], &ready, 1));
  ::close(pipe_fds[0]);

  ASSERT_EQ(0, ::kill(pid, SIGKILL));
  int status = 0;
  ASSERT_EQ(pid, ::waitpid(pid, &status, 0));
  ASSERT_TRUE(WIFSIGNALED(status));

  DuckDBAdapter reader;
  DuckDBConfig ro_cfg;
  ro_cfg.read_only = true;
  ExpectOk(reader.Init(path, ro_cfg));
  EXPECT_EQ(1, QueryCount(reader, "SELECT COUNT(*) FROM t"));

  Gtid latest;
  ExpectOk(reader.GetLatestWatermark(&latest));
  EXPECT_EQ("gtid:1", latest.value);
  reader.Shutdown();

  CleanupDuckdbFiles(path);
#endif
}

}  // namespace
