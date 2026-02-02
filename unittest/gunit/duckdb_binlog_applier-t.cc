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

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "storage/duckdb/duckdb_adapter.h"
#include "storage/duckdb/duckdb_binlog_applier.h"
#include "storage/duckdb/duckdb_gtid_utils.h"

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
using duckdb_se::DDLChange;
using duckdb_se::Gtid;
using duckdb_se::MySQLTableDef;
using duckdb_se::Row;
using duckdb_se::SessionCtx;
using duckdb_se::Status;
using duckdb_se::TableId;
using duckdb_se::GetBinlogApplyMetrics;
using duckdb_se::SetBinlogApplyPaused;
using duckdb_se::SetBinlogApplyThrottleRowsPerSec;
using duckdb_se::BuildGtidSetFromList;
using duckdb_se::GtidSetContains;
using duckdb_se::IsGtidSetSubset;
using duckdb_se::MergeGtidIntoSet;

const std::string kTestUuid = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";

// Forward declarations of helper functions
std::string TempDirectory();
std::string MakeTempPath(const std::string &prefix);
void CleanupDuckdbFiles(const std::string &path);
void ExpectOk(const Status &st);

std::string MakeGtid(int seq) {
  return kTestUuid + ":" + std::to_string(seq);
}

TEST(DuckDBGtidUtilsTest, MergeAndContains) {
  std::string set;
  std::string error;
  EXPECT_TRUE(MergeGtidIntoSet("", MakeGtid(1), &set, &error)) << error;
  bool contains = false;
  EXPECT_TRUE(GtidSetContains(set, MakeGtid(1), &contains, &error)) << error;
  EXPECT_TRUE(contains);
  contains = false;
  EXPECT_TRUE(GtidSetContains(set, MakeGtid(2), &contains, &error)) << error;
  EXPECT_FALSE(contains);

  EXPECT_TRUE(MergeGtidIntoSet(set, MakeGtid(2), &set, &error)) << error;
  contains = false;
  EXPECT_TRUE(GtidSetContains(set, MakeGtid(2), &contains, &error)) << error;
  EXPECT_TRUE(contains);
}

TEST(DuckDBGtidUtilsTest, BuildSetFromList) {
  std::vector<std::string> gtids{MakeGtid(3), MakeGtid(5)};
  std::string set;
  std::string error;
  EXPECT_TRUE(BuildGtidSetFromList(gtids, &set, &error)) << error;
  bool contains = false;
  EXPECT_TRUE(GtidSetContains(set, MakeGtid(3), &contains, &error)) << error;
  EXPECT_TRUE(contains);
  contains = false;
  EXPECT_TRUE(GtidSetContains(set, MakeGtid(4), &contains, &error)) << error;
  EXPECT_FALSE(contains);
}

TEST(DuckDBGtidUtilsTest, SubsetChecks) {
  std::string error;
  std::string superset;
  std::string candidate;
  EXPECT_TRUE(MergeGtidIntoSet("", MakeGtid(1), &superset, &error)) << error;
  EXPECT_TRUE(MergeGtidIntoSet(superset, MakeGtid(2), &superset, &error))
      << error;
  EXPECT_TRUE(MergeGtidIntoSet("", MakeGtid(1), &candidate, &error)) << error;
  bool subset = false;
  EXPECT_TRUE(IsGtidSetSubset(candidate, superset, &subset, &error)) << error;
  EXPECT_TRUE(subset);

  subset = false;
  EXPECT_TRUE(IsGtidSetSubset(superset, candidate, &subset, &error)) << error;
  EXPECT_FALSE(subset);
}

TEST(DuckDBGtidUtilsTest, AdapterIsGtidAppliedUsesSet) {
  const std::string path = MakeTempPath("duckdb_gtid_set");
  CleanupDuckdbFiles(path);

  DuckDBAdapter adapter;
  DuckDBConfig cfg;
  cfg.read_only = false;
  ExpectOk(adapter.Init(path, cfg));

  std::vector<std::string> gtids{MakeGtid(7), MakeGtid(9)};
  std::string set;
  std::string error;
  ASSERT_TRUE(BuildGtidSetFromList(gtids, &set, &error)) << error;

  const std::string create_sql =
      "CREATE TABLE IF NOT EXISTS __repl_state ("
      "channel VARCHAR PRIMARY KEY, "
      "snapshot_gtid_set VARCHAR, "
      "applied_gtid_set VARCHAR, "
      "last_commit_ts TIMESTAMP)";
  auto create = adapter.ExecuteQuery(create_sql, {});
  ASSERT_TRUE(create.ok) << create.error;

  const std::string insert_sql =
      "INSERT INTO __repl_state (channel, snapshot_gtid_set, applied_gtid_set, "
      "last_commit_ts) VALUES ('default', NULL, '" +
      set + "', '2026-01-01 00:00:00')";
  auto insert = adapter.ExecuteQuery(insert_sql, {});
  ASSERT_TRUE(insert.ok) << insert.error;

  bool applied = false;
  ExpectOk(adapter.IsGtidApplied(Gtid{MakeGtid(7)}, &applied));
  EXPECT_TRUE(applied);
  applied = false;
  ExpectOk(adapter.IsGtidApplied(Gtid{MakeGtid(8)}, &applied));
  EXPECT_FALSE(applied);

  adapter.Shutdown();
  CleanupDuckdbFiles(path);
}

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

int64_t QueryChecksum(DuckDBAdapter &adapter) {
  const std::string sql =
      "SELECT COALESCE(SUM(CAST(\"id\" AS BIGINT)), 0) + "
      "COALESCE(SUM(LENGTH(\"val\")), 0) FROM t";
  SessionCtx ctx;
  auto result = adapter.ExecuteQuery(sql, ctx);
  if (!result.ok || !result.result) {
    ADD_FAILURE() << "DuckDB query failed: " << result.error;
    return 0;
  }
  auto chunk = result.result->Fetch();
  if (!chunk || chunk->size() == 0) {
    ADD_FAILURE() << "DuckDB checksum query returned no rows";
    return 0;
  }
  return chunk->GetValue(0, 0).GetValue<int64_t>();
}

duckdb::Value QuerySingleValue(DuckDBAdapter &adapter,
                               const std::string &sql) {
  SessionCtx ctx;
  auto result = adapter.ExecuteQuery(sql, ctx);
  if (!result.ok || !result.result) {
    ADD_FAILURE() << "DuckDB query failed: " << result.error;
    return duckdb::Value();
  }
  auto chunk = result.result->Fetch();
  if (!chunk || chunk->size() == 0) {
    ADD_FAILURE() << "DuckDB query returned no rows";
    return duckdb::Value();
  }
  return chunk->GetValue(0, 0);
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
  ExpectOk(applier.BeginTransaction(Gtid{MakeGtid(1)}));
  ExpectOk(applier.AppendInsert(TableId{"", "t"}, MakeRow("1", "alpha")));
  ExpectOk(applier.CommitTransaction());
  const int64_t checksum_before = QueryChecksum(adapter);
  adapter.Shutdown();

  DuckDBAdapter reader;
  DuckDBConfig ro_cfg;
  ro_cfg.read_only = true;
  ExpectOk(reader.Init(path, ro_cfg));
  EXPECT_EQ(1, QueryCount(reader, "SELECT COUNT(*) FROM t"));
  EXPECT_EQ(checksum_before, QueryChecksum(reader));

  Gtid latest;
  ExpectOk(reader.GetLatestWatermark(&latest));
  EXPECT_EQ(MakeGtid(1), latest.value);
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
  ExpectOk(applier.BeginTransaction(Gtid{MakeGtid(1)}));
  ExpectOk(applier.AppendInsert(TableId{"", "t"}, MakeRow("1", "alpha")));
  ExpectOk(applier.CommitTransaction());
  const int64_t checksum_before = QueryChecksum(adapter);
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
    st = child_applier.BeginTransaction(Gtid{MakeGtid(2)});
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
  EXPECT_EQ(checksum_before, QueryChecksum(reader));

  Gtid latest;
  ExpectOk(reader.GetLatestWatermark(&latest));
  EXPECT_EQ(MakeGtid(1), latest.value);
  reader.Shutdown();

  CleanupDuckdbFiles(path);
#endif
}

TEST(DuckDBBinlogApplierTest, IdempotentReplaySkipsAppliedGtid) {
  const std::string path = MakeTempPath("duckdb_idempotent");
  CleanupDuckdbFiles(path);

  DuckDBAdapter adapter;
  DuckDBConfig cfg;
  cfg.read_only = false;
  ExpectOk(adapter.Init(path, cfg));
  ExpectOk(adapter.CreateTable(MakeSimpleTable()));

  DuckDBBinlogApplier applier(&adapter);
  ExpectOk(applier.BeginTransaction(Gtid{MakeGtid(10)}));
  ExpectOk(applier.AppendInsert(TableId{"", "t"}, MakeRow("1", "alpha")));
  ExpectOk(applier.CommitTransaction());
  const int64_t checksum_before = QueryChecksum(adapter);

  // Reapply the same GTID with a different row: should be ignored.
  ExpectOk(applier.BeginTransaction(Gtid{MakeGtid(10)}));
  ExpectOk(applier.AppendInsert(TableId{"", "t"}, MakeRow("2", "beta")));
  ExpectOk(applier.CommitTransaction());

  EXPECT_EQ(1, QueryCount(adapter, "SELECT COUNT(*) FROM t"));
  EXPECT_EQ(checksum_before, QueryChecksum(adapter));

  Gtid latest;
  ExpectOk(adapter.GetLatestWatermark(&latest));
  EXPECT_EQ(MakeGtid(10), latest.value);

  adapter.Shutdown();
  CleanupDuckdbFiles(path);
}

TEST(DuckDBBinlogApplierTest, BulkUpdateDeleteFullRowImage) {
  const std::string path = MakeTempPath("duckdb_bulk_update");
  CleanupDuckdbFiles(path);

  DuckDBAdapter adapter;
  DuckDBConfig cfg;
  cfg.read_only = false;
  ExpectOk(adapter.Init(path, cfg));
  ExpectOk(adapter.CreateTable(MakeSimpleTable()));

  DuckDBBinlogApplier applier(&adapter);
  ExpectOk(applier.BeginTransaction(Gtid{MakeGtid(20)}));
  ExpectOk(applier.AppendInsert(TableId{"", "t"}, MakeRow("1", "alpha")));
  ExpectOk(applier.CommitTransaction());

  const std::string update_sql =
      "UPDATE \"t\" SET \"val\" = 'beta' WHERE \"id\" IS NOT DISTINCT FROM '1' "
      "AND \"val\" IS NOT DISTINCT FROM 'alpha'";
  ExpectOk(applier.BeginTransaction(Gtid{MakeGtid(21)}));
  ExpectOk(applier.AddUpdateStatement(TableId{"", "t"}, update_sql));
  ExpectOk(applier.CommitTransaction());
  EXPECT_EQ(1, QueryCount(adapter,
                          "SELECT COUNT(*) FROM t WHERE val = 'beta'"));

  const std::string delete_sql =
      "DELETE FROM \"t\" WHERE \"id\" IS NOT DISTINCT FROM '1' AND \"val\" IS "
      "NOT DISTINCT FROM 'beta'";
  ExpectOk(applier.BeginTransaction(Gtid{MakeGtid(22)}));
  ExpectOk(applier.AddDeleteStatement(TableId{"", "t"}, delete_sql));
  ExpectOk(applier.CommitTransaction());
  EXPECT_EQ(0, QueryCount(adapter, "SELECT COUNT(*) FROM t"));

  adapter.Shutdown();
  CleanupDuckdbFiles(path);
}

TEST(DuckDBBinlogApplierTest, BulkUpdatePreservesOrderOnChainedUpdates) {
  const std::string path = MakeTempPath("duckdb_bulk_chain");
  CleanupDuckdbFiles(path);

  DuckDBAdapter adapter;
  DuckDBConfig cfg;
  cfg.read_only = false;
  ExpectOk(adapter.Init(path, cfg));
  ExpectOk(adapter.CreateTable(MakeSimpleTable()));

  DuckDBBinlogApplier applier(&adapter);
  ExpectOk(applier.BeginTransaction(Gtid{MakeGtid(30)}));
  ExpectOk(applier.AppendInsert(TableId{"", "t"}, MakeRow("1", "alpha")));
  ExpectOk(applier.CommitTransaction());

  const std::string update1 =
      "UPDATE \"t\" SET \"val\" = 'beta' WHERE \"id\" IS NOT DISTINCT FROM '1' "
      "AND \"val\" IS NOT DISTINCT FROM 'alpha'";
  const std::string update2 =
      "UPDATE \"t\" SET \"val\" = 'gamma' WHERE \"id\" IS NOT DISTINCT FROM '1' "
      "AND \"val\" IS NOT DISTINCT FROM 'beta'";
  ExpectOk(applier.BeginTransaction(Gtid{MakeGtid(31)}));
  ExpectOk(applier.AddUpdateStatement(TableId{"", "t"}, update1));
  ExpectOk(applier.AddUpdateStatement(TableId{"", "t"}, update2));
  ExpectOk(applier.CommitTransaction());

  EXPECT_EQ(1, QueryCount(adapter,
                          "SELECT COUNT(*) FROM t WHERE val = 'gamma'"));
  EXPECT_EQ(0, QueryCount(adapter,
                          "SELECT COUNT(*) FROM t WHERE val = 'beta'"));

  adapter.Shutdown();
  CleanupDuckdbFiles(path);
}

TEST(DuckDBBinlogApplierTest, ApplyDDLInTransaction) {
  const std::string path = MakeTempPath("duckdb_apply_ddl");
  CleanupDuckdbFiles(path);

  DuckDBAdapter adapter;
  DuckDBConfig cfg;
  cfg.read_only = false;
  ExpectOk(adapter.Init(path, cfg));

  DuckDBBinlogApplier applier(&adapter);
  DDLChange create_change;
  create_change.type = DDLChange::Type::kCreate;
  create_change.new_def = MakeSimpleTable();

  ExpectOk(applier.BeginTransaction(Gtid{MakeGtid(40)}));
  ExpectOk(applier.ApplyDDL(std::move(create_change)));
  ExpectOk(applier.CommitTransaction());

  EXPECT_EQ(0, QueryCount(adapter, "SELECT COUNT(*) FROM t"));

  ExpectOk(applier.BeginTransaction(Gtid{MakeGtid(41)}));
  ExpectOk(applier.AppendInsert(TableId{"", "t"}, MakeRow("1", "alpha")));
  ExpectOk(applier.CommitTransaction());
  EXPECT_EQ(1, QueryCount(adapter, "SELECT COUNT(*) FROM t"));

  adapter.Shutdown();
  CleanupDuckdbFiles(path);
}

TEST(DuckDBBinlogApplierTest, PauseResumeBlocksApply) {
  const std::string path = MakeTempPath("duckdb_pause");
  CleanupDuckdbFiles(path);

  DuckDBAdapter adapter;
  DuckDBConfig cfg;
  cfg.read_only = false;
  ExpectOk(adapter.Init(path, cfg));
  ExpectOk(adapter.CreateTable(MakeSimpleTable()));

  SetBinlogApplyPaused(true);
  std::atomic<bool> finished{false};
  std::promise<void> started;
  auto started_future = started.get_future();
  DuckDBBinlogApplier applier(&adapter);

  std::thread worker([&]() {
    started.set_value();
    Status st = applier.BeginTransaction(Gtid{MakeGtid(50)});
    if (st.ok()) {
      st = applier.AppendInsert(TableId{"", "t"}, MakeRow("1", "alpha"));
    }
    if (st.ok()) {
      st = applier.CommitTransaction();
    }
    finished.store(true);
  });

  started_future.wait();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(finished.load());

  SetBinlogApplyPaused(false);
  worker.join();
  EXPECT_TRUE(finished.load());

  adapter.Shutdown();
  CleanupDuckdbFiles(path);
}

TEST(DuckDBBinlogApplierTest, ThrottleAppliesSleep) {
  const std::string path = MakeTempPath("duckdb_throttle");
  CleanupDuckdbFiles(path);

  DuckDBAdapter adapter;
  DuckDBConfig cfg;
  cfg.read_only = false;
  ExpectOk(adapter.Init(path, cfg));
  ExpectOk(adapter.CreateTable(MakeSimpleTable()));

  SetBinlogApplyThrottleRowsPerSec(1);
  DuckDBBinlogApplier applier(&adapter);
  ExpectOk(applier.BeginTransaction(Gtid{MakeGtid(60)}));
  ExpectOk(applier.AppendInsert(TableId{"", "t"}, MakeRow("1", "alpha")));
  const auto start = std::chrono::steady_clock::now();
  ExpectOk(applier.CommitTransaction());
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  const auto metrics = GetBinlogApplyMetrics();
  EXPECT_GT(metrics.last_throttle_ms, 0u);
  EXPECT_GE(elapsed.count(), 800);

  SetBinlogApplyThrottleRowsPerSec(0);

  adapter.Shutdown();
  CleanupDuckdbFiles(path);
}

TEST(DuckDBBinlogApplierTest, LagMetricsUpdateOnCommit) {
  const std::string path = MakeTempPath("duckdb_lag_metrics");
  CleanupDuckdbFiles(path);

  DuckDBAdapter adapter;
  DuckDBConfig cfg;
  cfg.read_only = false;
  ExpectOk(adapter.Init(path, cfg));
  ExpectOk(adapter.CreateTable(MakeSimpleTable()));

  DuckDBBinlogApplier applier(&adapter);
  ExpectOk(applier.BeginTransaction(Gtid{MakeGtid(70)}));
  ExpectOk(applier.AppendInsert(TableId{"", "t"}, MakeRow("1", "alpha")));
  ExpectOk(applier.CommitTransaction());

  const auto metrics = GetBinlogApplyMetrics();
  EXPECT_GT(metrics.last_commit_epoch_ms, 0u);
  EXPECT_GE(metrics.lag_ms, 0u);

  adapter.Shutdown();
  CleanupDuckdbFiles(path);
}

TEST(DuckDBBinlogApplierTest, FlushDoesNotCommitMidTransaction) {
  const std::string path = MakeTempPath("duckdb_flush_boundary");
  CleanupDuckdbFiles(path);

  DuckDBAdapter adapter;
  DuckDBConfig cfg;
  cfg.read_only = false;
  ExpectOk(adapter.Init(path, cfg));
  ExpectOk(adapter.CreateTable(MakeSimpleTable()));

  BinlogApplierOptions options;
  options.max_rows = 1;
  options.max_bytes = 0;
  options.max_delay = std::chrono::milliseconds(0);
  DuckDBBinlogApplier applier(&adapter, options);

  ExpectOk(applier.BeginTransaction(Gtid{MakeGtid(80)}));
  ExpectOk(applier.AppendInsert(TableId{"", "t"}, MakeRow("1", "alpha")));
  EXPECT_EQ(0, QueryCount(adapter, "SELECT COUNT(*) FROM t"));

  ExpectOk(applier.AppendInsert(TableId{"", "t"}, MakeRow("2", "beta")));
  EXPECT_EQ(0, QueryCount(adapter, "SELECT COUNT(*) FROM t"));

  ExpectOk(applier.CommitTransaction());
  EXPECT_EQ(2, QueryCount(adapter, "SELECT COUNT(*) FROM t"));

  auto value = QuerySingleValue(
      adapter,
      "SELECT last_commit_ts FROM __repl_state WHERE channel='default'");
  EXPECT_FALSE(value.IsNull());
  EXPECT_FALSE(value.ToString().empty());

  adapter.Shutdown();
  CleanupDuckdbFiles(path);
}

}  // namespace
