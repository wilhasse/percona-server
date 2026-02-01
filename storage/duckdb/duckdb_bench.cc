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

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "storage/duckdb/duckdb_adapter.h"
#include "storage/duckdb/duckdb_binlog_applier.h"

namespace {

using duckdb_se::BinlogApplierOptions;
using duckdb_se::Cell;
using duckdb_se::DuckDBAdapter;
using duckdb_se::DuckDBBinlogApplier;
using duckdb_se::DuckDBConfig;
using duckdb_se::Gtid;
using duckdb_se::MySQLTableDef;
using duckdb_se::Row;
using duckdb_se::RowBatch;
using duckdb_se::SessionCtx;
using duckdb_se::Status;
using duckdb_se::TableId;
using duckdb_se::UpdateBatch;

struct Options {
  std::string db_path;
  std::string mode = "all";
  std::string output;
  size_t rows = 100000;
  size_t batch = 1000;
  size_t query_iters = 5;
};

struct Result {
  std::string name;
  std::string metric;
  size_t rows = 0;
  size_t batch = 0;
  double seconds = 0.0;
  double rows_per_sec = 0.0;
  double latency_ms = 0.0;
};

int GetPid() {
#ifdef _WIN32
  return _getpid();
#else
  return ::getpid();
#endif
}

std::string TempDirectory() {
  try {
    return std::filesystem::temp_directory_path().string();
  } catch (...) {
    return "/tmp";
  }
}

std::string DefaultDbPath() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::ostringstream oss;
  oss << "duckdb_bench_" << GetPid() << "_" << now << ".duckdb";
  return (std::filesystem::path(TempDirectory()) / oss.str()).string();
}

void CleanupDuckdbFiles(const std::string &path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::remove(path + ".wal", ec);
}

bool EnsureOk(const Status &st, const std::string &context) {
  if (st.ok()) return true;
  std::cerr << context << " failed: " << st.message << "\n";
  return false;
}

MySQLTableDef MakeTableDef() {
  MySQLTableDef def;
  def.name = "t";
  def.columns.push_back({"id", "INTEGER", true});
  def.columns.push_back({"val", "VARCHAR", false});
  return def;
}

bool ResetTable(DuckDBAdapter &adapter) {
  EnsureOk(adapter.DropTable("", "t"), "DropTable");
  return EnsureOk(adapter.CreateTable(MakeTableDef()), "CreateTable");
}

Row MakeRow(size_t id) {
  Row row;
  row.push_back(Cell{false, false, std::to_string(id)});
  row.push_back(Cell{false, false, "val_" + std::to_string(id)});
  return row;
}

double SecondsSince(const std::chrono::steady_clock::time_point &start) {
  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::duration<double>>(end -
                                                                   start)
      .count();
}

Result BenchAppender(DuckDBAdapter &adapter, size_t rows, size_t batch) {
  Result res;
  res.name = "appender";
  res.metric = "throughput";
  res.rows = rows;
  res.batch = batch;

  auto txn = adapter.BeginApplyTxn(Gtid{"bench:appender"});
  if (!EnsureOk(txn.status, "BeginApplyTxn(appender)")) {
    return res;
  }

  TableId table{"", "t"};
  size_t inserted = 0;
  auto start = std::chrono::steady_clock::now();
  while (inserted < rows) {
    const size_t chunk = std::min(batch, rows - inserted);
    RowBatch batch_rows;
    batch_rows.table = table;
    batch_rows.rows.reserve(chunk);
    for (size_t i = 0; i < chunk; ++i) {
      batch_rows.rows.push_back(MakeRow(inserted + i + 1));
    }
    if (!EnsureOk(adapter.AppendRows(txn, table, std::move(batch_rows)),
                  "AppendRows(appender)")) {
      break;
    }
    inserted += chunk;
  }
  EnsureOk(adapter.CommitApplyTxn(txn), "CommitApplyTxn(appender)");

  res.seconds = SecondsSince(start);
  if (res.seconds > 0.0) res.rows_per_sec = inserted / res.seconds;
  return res;
}

Result BenchSqlInsert(DuckDBAdapter &adapter, size_t rows, size_t batch) {
  Result res;
  res.name = "sql_insert";
  res.metric = "throughput";
  res.rows = rows;
  res.batch = batch;

  auto txn = adapter.BeginApplyTxn(Gtid{"bench:sql"});
  if (!EnsureOk(txn.status, "BeginApplyTxn(sql_insert)")) {
    return res;
  }

  TableId table{"", "t"};
  size_t inserted = 0;
  auto start = std::chrono::steady_clock::now();
  while (inserted < rows) {
    const size_t chunk = std::min(batch, rows - inserted);
    std::ostringstream sql;
    sql << "INSERT INTO t VALUES ";
    for (size_t i = 0; i < chunk; ++i) {
      const size_t id = inserted + i + 1;
      sql << "(" << id << ", 'val_" << id << "')";
      if (i + 1 < chunk) sql << ",";
    }
    UpdateBatch update;
    update.table = table;
    update.statements.push_back(sql.str());
    if (!EnsureOk(adapter.ApplyUpdates(txn, table, std::move(update)),
                  "ApplyUpdates(sql_insert)")) {
      break;
    }
    inserted += chunk;
  }
  EnsureOk(adapter.CommitApplyTxn(txn), "CommitApplyTxn(sql_insert)");

  res.seconds = SecondsSince(start);
  if (res.seconds > 0.0) res.rows_per_sec = inserted / res.seconds;
  return res;
}

Result BenchBinlogApplier(DuckDBAdapter &adapter, size_t rows, size_t batch) {
  Result res;
  res.name = "binlog_applier";
  res.metric = "throughput";
  res.rows = rows;
  res.batch = batch;

  BinlogApplierOptions options;
  options.max_rows = batch;
  options.max_bytes = 0;
  options.max_delay = std::chrono::milliseconds(0);
  DuckDBBinlogApplier applier(&adapter, options);

  if (!EnsureOk(applier.BeginTransaction(Gtid{"bench:applier"}),
                "BeginTransaction(applier)")) {
    return res;
  }

  TableId table{"", "t"};
  auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < rows; ++i) {
    if (!EnsureOk(applier.AppendInsert(table, MakeRow(i + 1)),
                  "AppendInsert(applier)")) {
      break;
    }
  }
  EnsureOk(applier.CommitTransaction(), "CommitTransaction(applier)");

  res.seconds = SecondsSince(start);
  if (res.seconds > 0.0) res.rows_per_sec = rows / res.seconds;
  return res;
}

Result BenchQueryLatency(DuckDBAdapter &adapter, size_t rows,
                         size_t batch, size_t iters) {
  Result res;
  res.name = "query_count";
  res.metric = "query";
  res.rows = rows;
  res.batch = batch;

  if (!ResetTable(adapter)) return res;
  (void)BenchAppender(adapter, rows, batch);

  SessionCtx ctx;
  const std::string sql = "SELECT COUNT(*) FROM t WHERE id > 0";
  auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < iters; ++i) {
    auto result = adapter.ExecuteQuery(sql, ctx);
    if (!result.ok || !result.result) {
      std::cerr << "ExecuteQuery failed: " << result.error << "\n";
      break;
    }
    auto chunk = result.result->Fetch();
    if (!chunk || chunk->size() == 0) {
      std::cerr << "Query returned no rows\n";
      break;
    }
  }
  res.seconds = SecondsSince(start);
  if (iters > 0) res.latency_ms = (res.seconds * 1000.0) / iters;
  return res;
}

bool MatchesMode(const std::string &mode, const std::string &value) {
  return mode == "all" || mode == value;
}

void WriteCsv(std::ostream &out, const std::vector<Result> &results) {
  out << "name,metric,rows,batch,seconds,rows_per_sec,latency_ms\n";
  out << std::fixed << std::setprecision(6);
  for (const auto &res : results) {
    out << res.name << "," << res.metric << "," << res.rows << ","
        << res.batch << "," << res.seconds << "," << res.rows_per_sec << ","
        << res.latency_ms << "\n";
  }
}

Options ParseArgs(int argc, char **argv) {
  Options opts;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--rows" && i + 1 < argc) {
      opts.rows = std::stoull(argv[++i]);
    } else if (arg == "--batch" && i + 1 < argc) {
      opts.batch = std::stoull(argv[++i]);
    } else if (arg == "--db" && i + 1 < argc) {
      opts.db_path = argv[++i];
    } else if (arg == "--mode" && i + 1 < argc) {
      opts.mode = argv[++i];
    } else if (arg == "--out" && i + 1 < argc) {
      opts.output = argv[++i];
    } else if (arg == "--query-iters" && i + 1 < argc) {
      opts.query_iters = std::stoull(argv[++i]);
    }
  }
  return opts;
}

void PrintUsage() {
  std::cout << "duckdb_bench [--rows N] [--batch N] [--db PATH] "
               "[--mode all|sql|appender|applier|query] [--out CSV] "
               "[--query-iters N]\n";
}

}  // namespace

int main(int argc, char **argv) {
  Options opts = ParseArgs(argc, argv);
  if (opts.db_path.empty()) {
    opts.db_path = DefaultDbPath();
  }

  if (opts.rows == 0 || opts.batch == 0) {
    PrintUsage();
    return 1;
  }

  CleanupDuckdbFiles(opts.db_path);

  DuckDBAdapter adapter;
  DuckDBConfig cfg;
  cfg.read_only = false;
  if (!EnsureOk(adapter.Init(opts.db_path, cfg), "Init")) return 1;
  if (!ResetTable(adapter)) return 1;

  std::vector<Result> results;
  if (MatchesMode(opts.mode, "sql")) {
    results.push_back(BenchSqlInsert(adapter, opts.rows, opts.batch));
  }
  if (MatchesMode(opts.mode, "appender")) {
    if (!ResetTable(adapter)) return 1;
    results.push_back(BenchAppender(adapter, opts.rows, opts.batch));
  }
  if (MatchesMode(opts.mode, "applier")) {
    if (!ResetTable(adapter)) return 1;
    results.push_back(BenchBinlogApplier(adapter, opts.rows, opts.batch));
  }
  if (MatchesMode(opts.mode, "query")) {
    results.push_back(BenchQueryLatency(adapter, opts.rows, opts.batch,
                                        opts.query_iters));
  }

  if (opts.mode == "all") {
    if (!ResetTable(adapter)) return 1;
    results.clear();
    results.push_back(BenchSqlInsert(adapter, opts.rows, opts.batch));
    if (!ResetTable(adapter)) return 1;
    results.push_back(BenchAppender(adapter, opts.rows, opts.batch));
    if (!ResetTable(adapter)) return 1;
    results.push_back(BenchBinlogApplier(adapter, opts.rows, opts.batch));
    results.push_back(
        BenchQueryLatency(adapter, opts.rows, opts.batch, opts.query_iters));
  }

  if (!opts.output.empty()) {
    std::ofstream out(opts.output);
    WriteCsv(out, results);
  } else {
    WriteCsv(std::cout, results);
  }

  adapter.Shutdown();
  CleanupDuckdbFiles(opts.db_path);
  return 0;
}
