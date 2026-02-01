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

#include "storage/duckdb/duckdb_binlog_apply_thread.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "mysql.h"
#include "my_sys.h"
#include "sql/log.h"
#include "sql/mysqld.h"
#include "storage/duckdb/duckdb_adapter.h"
#include "storage/duckdb/duckdb_binlog_applier.h"
#include "storage/duckdb/duckdb_binlog_streamer.h"
#include "storage/duckdb/duckdb_row_decoder.h"

namespace duckdb_se {
namespace {

struct SchemaApplierState {
  std::string schema;
  std::unique_ptr<DuckDBAdapter> adapter;
  std::unique_ptr<DuckDBBinlogApplier> applier;
  bool txn_active{false};
};

struct ApplyThreadState {
  std::thread worker;
  std::atomic<bool> stop{false};
  std::atomic<bool> running{false};
  std::mutex mutex;
};

ApplyThreadState &GetThreadState() {
  static ApplyThreadState state;
  return state;
}

std::string DuckdbPathForSchema(const std::string &schema) {
  std::string dir = mysql_real_data_home;
  if (!dir.empty() && dir.back() != FN_LIBCHAR) dir.push_back(FN_LIBCHAR);
  return dir + schema + ".duckdb";
}

uint32_t ResolveServerId(uint32_t configured) {
  if (configured != 0) return configured;
  if (server_id != 0) return static_cast<uint32_t>(server_id + 1000);
  return 24844;
}

bool FetchGtidExecuted(const BinlogApplyThreadOptions &options,
                       std::string *gtid_set) {
  if (!gtid_set) return false;
  gtid_set->clear();

  MYSQL *mysql = mysql_init(nullptr);
  if (!mysql) return false;
  if (!mysql_real_connect(mysql, options.host.c_str(), options.user.c_str(),
                          options.password.empty()
                              ? nullptr
                              : options.password.c_str(),
                          nullptr, options.port,
                          options.socket.empty() ? nullptr
                                                  : options.socket.c_str(),
                          0)) {
    mysql_close(mysql);
    return false;
  }

  const char *sql = "SELECT @@GLOBAL.GTID_EXECUTED";
  if (mysql_real_query(mysql, sql, std::strlen(sql)) != 0) {
    mysql_close(mysql);
    return false;
  }
  MYSQL_RES *res = mysql_store_result(mysql);
  if (!res) {
    mysql_close(mysql);
    return false;
  }
  MYSQL_ROW row = mysql_fetch_row(res);
  if (row && row[0]) {
    gtid_set->assign(row[0]);
  }
  mysql_free_result(res);
  mysql_close(mysql);
  return !gtid_set->empty();
}

bool ShouldApplySchema(const std::string &schema,
                       const std::string &schema_filter) {
  if (schema_filter.empty()) return true;
  return schema == schema_filter;
}

Status EnsureSchemaApplier(const std::string &schema,
                           std::map<std::string, SchemaApplierState> *states,
                           SchemaApplierState **out_state) {
  if (!states || !out_state) {
    return Status::Error(StatusCode::kInvalid, "Invalid schema state output");
  }
  auto &entry = (*states)[schema];
  if (!entry.adapter) {
    entry.schema = schema;
    entry.adapter = std::make_unique<DuckDBAdapter>();
    DuckDBConfig cfg;
    cfg.read_only = false;
    const std::string path = DuckdbPathForSchema(schema);
    Status st = entry.adapter->Init(path, cfg);
    if (!st.ok()) return st;
    entry.applier = std::make_unique<DuckDBBinlogApplier>(entry.adapter.get());
  }
  *out_state = &entry;
  return Status::Ok();
}

Status CommitActiveTransactions(
    std::map<std::string, SchemaApplierState> *states) {
  if (!states) return Status::Ok();
  for (auto &entry : *states) {
    auto &state = entry.second;
    if (!state.txn_active || !state.applier) continue;
    Status st = state.applier->CommitTransaction();
    if (!st.ok()) {
      (void)state.applier->RollbackTransaction();
      return st;
    }
    state.txn_active = false;
  }
  return Status::Ok();
}

Status ApplyRowEvent(const BinlogEvent &event, const BinlogTableMap &map,
                     SchemaApplierState &state, const std::string &gtid) {
  if (!state.applier) {
    return Status::Error(StatusCode::kInvalid, "Missing applier state");
  }
  if (!state.txn_active) {
    Status st = state.applier->BeginTransaction(Gtid{gtid});
    if (!st.ok()) return st;
    state.txn_active = true;
  }

  TableId table_id{map.schema, map.table};
  if (event.type == BinlogEvent::Type::kWriteRows) {
    std::vector<Row> rows;
    const auto &columns = event.columns_after.empty()
                              ? event.columns_before
                              : event.columns_after;
    Status st = DecodeWriteRows(map, columns, event.row_data, &rows);
    if (!st.ok()) return st;
    RowBatch batch;
    batch.table = table_id;
    batch.rows = std::move(rows);
    return state.applier->AppendInsertRows(table_id, std::move(batch));
  }
  if (event.type == BinlogEvent::Type::kUpdateRows) {
    std::vector<Row> before_rows;
    std::vector<Row> after_rows;
    Status st = DecodeUpdateRows(map, event.columns_before, event.columns_after,
                                 event.row_data, &before_rows, &after_rows);
    if (!st.ok()) return st;
    BulkUpdateBatch batch;
    batch.table = table_id;
    batch.old_rows = std::move(before_rows);
    batch.new_rows = std::move(after_rows);
    return state.applier->AppendUpdateRows(table_id, std::move(batch));
  }
  if (event.type == BinlogEvent::Type::kDeleteRows) {
    std::vector<Row> rows;
    const auto &columns = event.columns_before.empty()
                              ? event.columns_after
                              : event.columns_before;
    Status st = DecodeDeleteRows(map, columns, event.row_data, &rows);
    if (!st.ok()) return st;
    BulkDeleteBatch batch;
    batch.table = table_id;
    batch.old_rows = std::move(rows);
    return state.applier->AppendDeleteRows(table_id, std::move(batch));
  }
  return Status::Ok();
}

Status RunApplyLoop(const BinlogApplyThreadOptions &options,
                    std::atomic<bool> *stop_flag) {
  std::string gtid_set = options.start_gtid_set;
  if (gtid_set.empty()) {
    if (!FetchGtidExecuted(options, &gtid_set)) {
      return Status::Error(StatusCode::kInvalid,
                           "Failed to fetch @@GLOBAL.GTID_EXECUTED");
    }
  }

  BinlogStreamOptions stream_opts;
  stream_opts.host = options.host;
  stream_opts.user = options.user;
  stream_opts.password = options.password;
  stream_opts.socket = options.socket;
  stream_opts.port = options.port;
  stream_opts.server_id = ResolveServerId(options.server_id);
  stream_opts.gtid_set = gtid_set;
  stream_opts.non_blocking = true;

  sql_print_information(
      "DuckDB binlog applier: Opening stream to %s:%u as %s, "
      "server_id=%u, gtid_set='%.200s'",
      stream_opts.host.c_str(), stream_opts.port, stream_opts.user.c_str(),
      stream_opts.server_id,
      stream_opts.gtid_set.empty() ? "(empty)" : stream_opts.gtid_set.c_str());

  DuckDBBinlogStreamer streamer;
  Status st = streamer.Open(stream_opts);
  if (!st.ok()) {
    sql_print_warning("DuckDB binlog applier: Failed to open stream: %s",
                      st.message.c_str());
    return st;
  }
  sql_print_information("DuckDB binlog applier: Stream opened successfully");

  std::map<std::string, SchemaApplierState> schema_states;
  std::string current_gtid;
  bool seen_gtid = false;

  while (!stop_flag->load()) {
    BinlogEvent event;
    st = streamer.NextEvent(&event);
    if (!st.ok()) {
      if (stop_flag->load()) break;
      if (st.code == StatusCode::kInvalid &&
          st.message == "No binlog data available") {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
      return st;
    }

    switch (event.type) {
      case BinlogEvent::Type::kGtid: {
        if (seen_gtid) {
          st = CommitActiveTransactions(&schema_states);
          if (!st.ok()) return st;
        }
        current_gtid = event.gtid;
        seen_gtid = true;
        break;
      }
      case BinlogEvent::Type::kWriteRows:
      case BinlogEvent::Type::kUpdateRows:
      case BinlogEvent::Type::kDeleteRows: {
        if (!seen_gtid || current_gtid.empty()) {
          sql_print_warning("DuckDB binlog applier: row event without GTID");
          break;
        }
        const BinlogTableMap *map = streamer.GetTableMap(event.table_id);
        if (!map) {
          sql_print_warning("DuckDB binlog applier: missing table map");
          break;
        }
        if (!ShouldApplySchema(map->schema, options.schema_filter)) {
          break;
        }
        SchemaApplierState *state = nullptr;
        st = EnsureSchemaApplier(map->schema, &schema_states, &state);
        if (!st.ok()) return st;
        st = ApplyRowEvent(event, *map, *state, current_gtid);
        if (!st.ok()) return st;
        break;
      }
      case BinlogEvent::Type::kXid: {
        st = CommitActiveTransactions(&schema_states);
        if (!st.ok()) return st;
        seen_gtid = false;
        current_gtid.clear();
        break;
      }
      default:
        break;
    }
  }

  (void)CommitActiveTransactions(&schema_states);
  streamer.Close();
  return Status::Ok();
}

void ApplyThreadMain(BinlogApplyThreadOptions options) {
  auto &state = GetThreadState();
  uint64_t backoff_ms = 1000;
  const uint64_t max_backoff_ms = 30000;

  sql_print_information("DuckDB binlog applier thread started");

  while (!state.stop.load()) {
    Status st = RunApplyLoop(options, &state.stop);
    if (state.stop.load()) break;
    if (!st.ok()) {
      sql_print_warning(
          "DuckDB binlog applier error: %s (backoff %llu ms)",
          st.message.c_str(), static_cast<unsigned long long>(backoff_ms));
      std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
      backoff_ms = std::min(max_backoff_ms, backoff_ms * 2);
      continue;
    }
    backoff_ms = 1000;
  }

  sql_print_information("DuckDB binlog applier thread stopped");
  state.running.store(false);
}

}  // namespace

bool StartBinlogApplyThread(const BinlogApplyThreadOptions &options) {
  if (!options.enabled) return false;
  auto &state = GetThreadState();
  std::lock_guard<std::mutex> guard(state.mutex);
  if (state.running.load()) return true;
  state.stop.store(false);
  state.running.store(true);
  state.worker = std::thread(ApplyThreadMain, options);
  return true;
}

void StopBinlogApplyThread() {
  auto &state = GetThreadState();
  {
    std::lock_guard<std::mutex> guard(state.mutex);
    if (!state.running.load()) return;
    state.stop.store(true);
  }
  if (state.worker.joinable()) state.worker.join();
  state.running.store(false);
}

bool BinlogApplyThreadRunning() {
  return GetThreadState().running.load();
}

}  // namespace duckdb_se
