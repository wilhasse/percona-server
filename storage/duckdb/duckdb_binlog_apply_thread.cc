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
#include <cstdint>
#include <cstring>
#include <filesystem>
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
#include "storage/duckdb/duckdb_binlog_ddl.h"
#include "storage/duckdb/duckdb_binlog_streamer.h"
#include "storage/duckdb/duckdb_gtid_utils.h"
#include "storage/duckdb/duckdb_repl_state.h"
#include "storage/duckdb/duckdb_row_decoder.h"

namespace duckdb_se {
namespace {

struct SchemaApplierState {
  std::string schema;
  std::unique_ptr<DuckDBAdapter> adapter;
  std::unique_ptr<DuckDBBinlogApplier> applier;
  bool txn_active{false};
  int64_t schema_version{0};
  bool schema_version_loaded{false};
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

// Forward declaration for EnsureSchemaApplier used before definition
Status EnsureSchemaApplier(const std::string &schema,
                           const BinlogApplyThreadOptions &options,
                           const DuckDBBinlogApplier::Options &applier_options,
                           std::map<std::string, SchemaApplierState> *states,
                           SchemaApplierState **out_state);

std::string DuckdbPathForSchema(const BinlogApplyThreadOptions &options,
                                const std::string &schema) {
  const char *base_dir = options.duckdb_dir.empty()
                             ? mysql_real_data_home
                             : options.duckdb_dir.c_str();
  std::string dir = base_dir ? base_dir : "";
  if (!dir.empty() && dir.back() != FN_LIBCHAR) dir.push_back(FN_LIBCHAR);
  return dir + schema + ".duckdb";
}

struct ReplStateSnapshot {
  std::string applied_gtid_set;
  std::string snapshot_gtid_set;
  std::string last_commit_ts;
  std::string binlog_file;
  uint64_t binlog_pos{0};
  std::string source_path;
};

bool IsDuckdbFile(const std::filesystem::path &path) {
  return path.has_extension() && path.extension() == ".duckdb";
}

Status LoadReplStateFromFile(const std::string &path,
                             ReplStateSnapshot *out_state) {
  if (!out_state) {
    return Status::Error(StatusCode::kInvalid, "State output is null");
  }
  out_state->applied_gtid_set.clear();
  out_state->snapshot_gtid_set.clear();
  out_state->last_commit_ts.clear();
  out_state->binlog_file.clear();
  out_state->binlog_pos = 0;
  out_state->source_path.clear();

  try {
    duckdb::DBConfig cfg(true);
    duckdb::DuckDB db(path, &cfg);
    duckdb::Connection conn(db);

    std::vector<std::string> columns;
    bool missing = false;
    std::string column_error;
    if (!FetchReplTableColumns(conn, "__repl_state", &columns, &missing,
                               &column_error)) {
      return Status::Error(
          StatusCode::kDuckDBError,
          column_error.empty() ? "Failed to read __repl_state schema"
                               : column_error);
    }

    if (!missing) {
      const bool has_binlog_file =
          std::find(columns.begin(), columns.end(), "binlog_file") !=
          columns.end();
      const bool has_binlog_pos =
          std::find(columns.begin(), columns.end(), "binlog_pos") !=
          columns.end();
      std::string sql =
          "SELECT snapshot_gtid_set, applied_gtid_set, last_commit_ts";
      if (has_binlog_file) sql += ", binlog_file";
      if (has_binlog_pos) sql += ", binlog_pos";
      sql += " FROM __repl_state WHERE channel = 'default' LIMIT 1";
      auto result = conn.Query(sql);
      if (result->HasError()) {
        return Status::Error(StatusCode::kDuckDBError, result->GetError());
      }
      auto chunk = result->Fetch();
      if (chunk && chunk->size() > 0) {
        auto snap_val = chunk->GetValue(0, 0);
        if (!snap_val.IsNull()) {
          out_state->snapshot_gtid_set = snap_val.ToString();
        }
        auto applied_val = chunk->GetValue(1, 0);
        if (!applied_val.IsNull()) {
          out_state->applied_gtid_set = applied_val.ToString();
        }
        auto ts_val = chunk->GetValue(2, 0);
        if (!ts_val.IsNull()) {
          out_state->last_commit_ts = ts_val.ToString();
        }
        duckdb::idx_t idx = 3;
        if (has_binlog_file) {
          auto file_val = chunk->GetValue(idx++, 0);
          if (!file_val.IsNull()) {
            out_state->binlog_file = file_val.ToString();
          }
        }
        if (has_binlog_pos) {
          auto pos_val = chunk->GetValue(idx, 0);
          if (!pos_val.IsNull()) {
            out_state->binlog_pos = pos_val.GetValue<uint64_t>();
          }
        }
      }
    }

    if (out_state->applied_gtid_set.empty() &&
        !out_state->snapshot_gtid_set.empty()) {
      out_state->applied_gtid_set = out_state->snapshot_gtid_set;
    }
    if (out_state->applied_gtid_set.empty()) {
      auto wm = conn.Query(
          "SELECT gtid FROM __repl_watermark ORDER BY commit_ts");
      if (!wm->HasError()) {
        std::vector<std::string> gtids;
        while (true) {
          auto chunk = wm->Fetch();
          if (!chunk || chunk->size() == 0) break;
          for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
            auto val = chunk->GetValue(0, row);
            if (!val.IsNull()) {
              gtids.push_back(val.ToString());
            }
          }
        }
        if (!gtids.empty()) {
          std::string error;
          std::string set;
          if (!BuildGtidSetFromList(gtids, &set, &error)) {
            return Status::Error(StatusCode::kInvalid,
                                 error.empty() ? "Failed to build GTID set"
                                               : error);
          }
          out_state->applied_gtid_set = std::move(set);
        }
      } else if (!IsMissingReplTableError(wm->GetError(),
                                          "__repl_watermark")) {
        return Status::Error(StatusCode::kDuckDBError, wm->GetError());
      }
    }
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  if (!out_state->applied_gtid_set.empty() ||
      !out_state->snapshot_gtid_set.empty() ||
      !out_state->binlog_file.empty() || out_state->binlog_pos != 0) {
    out_state->source_path = path;
  }
  return Status::Ok();
}

Status ResolveReplState(const BinlogApplyThreadOptions &options,
                        ReplStateSnapshot *state) {
  if (!state) {
    return Status::Error(StatusCode::kInvalid, "State output is null");
  }
  state->applied_gtid_set.clear();
  state->snapshot_gtid_set.clear();
  state->last_commit_ts.clear();
  state->binlog_file.clear();
  state->binlog_pos = 0;
  state->source_path.clear();

  if (!options.schema_filter.empty()) {
    const std::string path =
        DuckdbPathForSchema(options, options.schema_filter);
    if (std::filesystem::exists(path)) {
      return LoadReplStateFromFile(path, state);
    }
    return Status::Ok();
  }

  std::string dir = mysql_real_data_home;
  if (dir.empty()) return Status::Ok();
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) break;
    if (!entry.is_regular_file()) continue;
    const auto &path = entry.path();
    if (!IsDuckdbFile(path)) continue;
    ReplStateSnapshot candidate;
    Status st = LoadReplStateFromFile(path.string(), &candidate);
    if (!st.ok()) return st;
    const bool candidate_has_state =
        !candidate.applied_gtid_set.empty() ||
        !candidate.snapshot_gtid_set.empty() ||
        !candidate.binlog_file.empty() || candidate.binlog_pos != 0;
    if (!candidate_has_state) {
      continue;
    }
    const bool state_has_state =
        !state->applied_gtid_set.empty() ||
        !state->snapshot_gtid_set.empty() || !state->binlog_file.empty() ||
        state->binlog_pos != 0;
    if (!state_has_state) {
      *state = std::move(candidate);
      continue;
    }
    if (state->applied_gtid_set.empty() &&
        !candidate.applied_gtid_set.empty()) {
      *state = std::move(candidate);
      continue;
    }
    if (!candidate.last_commit_ts.empty() &&
        candidate.last_commit_ts > state->last_commit_ts) {
      *state = std::move(candidate);
    }
  }
  return Status::Ok();
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

Status LoadSchemaVersion(SchemaApplierState *state) {
  if (!state || !state->adapter) {
    return Status::Error(StatusCode::kInvalid, "Missing schema adapter");
  }
  int64_t version = 0;
  bool found = false;
  Status st = state->adapter->GetSchemaVersion(&version, &found);
  if (!st.ok()) return st;
  if (found) {
    state->schema_version = version;
    state->schema_version_loaded = true;
  } else {
    state->schema_version = 0;
    state->schema_version_loaded = false;
  }
  return Status::Ok();
}

Status EnsureSchemaVersionFresh(SchemaApplierState *state) {
  if (!state || !state->adapter) {
    return Status::Error(StatusCode::kInvalid, "Missing schema adapter");
  }
  int64_t version = 0;
  bool found = false;
  Status st = state->adapter->GetSchemaVersion(&version, &found);
  if (!st.ok()) return st;
  if (!state->schema_version_loaded) {
    state->schema_version = found ? version : 0;
    state->schema_version_loaded = found;
    return Status::Ok();
  }
  if (!found) {
    return Status::Error(StatusCode::kInvalid,
                         "DuckDB schema_version missing");
  }
  if (version != state->schema_version) {
    return Status::Error(StatusCode::kInvalid,
                         "DuckDB schema_version changed; restart apply");
  }
  return Status::Ok();
}

std::string QuoteMySQLLiteral(MYSQL *mysql, const std::string &value) {
  std::string escaped;
  escaped.resize(value.size() * 2 + 1);
  const unsigned long len = mysql_real_escape_string(
      mysql, &escaped[0], value.c_str(), static_cast<unsigned long>(value.size()));
  escaped.resize(len);
  return "'" + escaped + "'";
}

bool FetchTableDef(MYSQL *mysql, const std::string &schema,
                   const std::string &table, MySQLTableDef *def) {
  if (!mysql || !def) return false;
  def->schema = schema;
  def->name = table;
  def->columns.clear();
  def->primary_key.clear();

  const std::string sql =
      "SELECT COLUMN_NAME, COLUMN_TYPE, IS_NULLABLE "
      "FROM INFORMATION_SCHEMA.COLUMNS "
      "WHERE TABLE_SCHEMA = " +
      QuoteMySQLLiteral(mysql, schema) +
      " AND TABLE_NAME = " + QuoteMySQLLiteral(mysql, table) +
      " ORDER BY ORDINAL_POSITION";

  if (mysql_real_query(mysql, sql.c_str(), sql.size()) != 0) {
    return false;
  }
  MYSQL_RES *res = mysql_store_result(mysql);
  if (!res) return false;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    if (!row[0] || !row[1] || !row[2]) continue;
    ColumnDef col;
    col.name = row[0];
    col.type = row[1];
    col.not_null = std::strcmp(row[2], "NO") == 0;
    def->columns.push_back(col);
  }
  mysql_free_result(res);
  if (!def->columns.empty()) {
    const std::string pk_sql =
        "SELECT COLUMN_NAME FROM INFORMATION_SCHEMA.KEY_COLUMN_USAGE "
        "WHERE TABLE_SCHEMA = " +
        QuoteMySQLLiteral(mysql, schema) +
        " AND TABLE_NAME = " + QuoteMySQLLiteral(mysql, table) +
        " AND CONSTRAINT_NAME = 'PRIMARY' "
        "ORDER BY ORDINAL_POSITION";
    if (mysql_real_query(mysql, pk_sql.c_str(), pk_sql.size()) != 0) {
      return false;
    }
    MYSQL_RES *pk_res = mysql_store_result(mysql);
    if (!pk_res) return false;
    MYSQL_ROW pk_row;
    while ((pk_row = mysql_fetch_row(pk_res))) {
      if (pk_row[0]) def->primary_key.push_back(pk_row[0]);
    }
    mysql_free_result(pk_res);
  }
  return !def->columns.empty();
}

Status FetchTableDefFromSource(const BinlogApplyThreadOptions &options,
                               const std::string &schema,
                               const std::string &table,
                               MySQLTableDef *def) {
  MYSQL *mysql = mysql_init(nullptr);
  if (!mysql) {
    return Status::Error(StatusCode::kInvalid, "mysql_init failed");
  }
  if (!mysql_real_connect(mysql, options.host.c_str(), options.user.c_str(),
                          options.password.empty()
                              ? nullptr
                              : options.password.c_str(),
                          nullptr, options.port,
                          options.socket.empty() ? nullptr
                                                  : options.socket.c_str(),
                          0)) {
    std::string err = mysql_error(mysql);
    mysql_close(mysql);
    return Status::Error(StatusCode::kInvalid,
                         "mysql_real_connect failed: " + err);
  }
  bool ok = FetchTableDef(mysql, schema, table, def);
  std::string err = ok ? "" : mysql_error(mysql);
  mysql_close(mysql);
  if (!ok) {
    return Status::Error(StatusCode::kInvalid,
                         err.empty() ? "Failed to fetch table definition"
                                     : err);
  }
  return Status::Ok();
}

bool DuckdbTableExists(DuckDBAdapter &adapter, const std::string &table) {
  const std::string sql =
      "SELECT 1 FROM information_schema.tables WHERE table_schema = 'main' "
      "AND table_name = '" + table + "' LIMIT 1";
  auto result = adapter.ExecuteQuery(sql, {});
  if (!result.ok) return false;
  auto chunk = result.result->Fetch();
  return chunk && chunk->size() > 0;
}

Status ApplyDdlEvent(const BinlogEvent &event,
                     const BinlogApplyThreadOptions &options,
                     const DuckDBBinlogApplier::Options &applier_options,
                     std::map<std::string, SchemaApplierState> *schema_states,
                     const std::string &gtid) {
  if (!schema_states) {
    return Status::Error(StatusCode::kInvalid, "Missing schema states");
  }
  if (event.query.empty()) return Status::Ok();
  ParsedDdl parsed;
  std::string parse_error;
  if (!ParseDdlQuery(event.query, event.schema, &parsed, &parse_error)) {
    if (!parse_error.empty()) {
      sql_print_warning("DuckDB binlog applier: DDL parse skipped: %s",
                        parse_error.c_str());
    }
    return Status::Ok();
  }
  if (!ShouldApplySchema(parsed.schema, options.schema_filter)) {
    return Status::Ok();
  }

  SchemaApplierState *state = nullptr;
  Status st =
      EnsureSchemaApplier(parsed.schema, options, applier_options,
                          schema_states, &state);
  if (!st.ok()) return st;
  if (!state->applier) {
    return Status::Error(StatusCode::kInvalid, "Missing applier state");
  }

  if (!state->txn_active) {
    st = state->applier->BeginTransaction(Gtid{gtid});
    if (!st.ok()) return st;
    state->txn_active = true;
  }

  DDLChange change = std::move(parsed.change);
  if (change.type == DDLChange::Type::kCreate) {
    if (parsed.if_not_exists &&
        DuckdbTableExists(*state->adapter, change.table.table)) {
      return Status::Ok();
    }
    MySQLTableDef def;
    st = FetchTableDefFromSource(options, parsed.schema, change.table.table,
                                 &def);
    if (!st.ok()) return st;
    change.new_def = std::move(def);
    st = state->applier->ApplyDDL(std::move(change));
    if (!st.ok()) return st;
    state->schema_version = state->schema_version_loaded
                                ? state->schema_version + 1
                                : 1;
    state->schema_version_loaded = true;
    return Status::Ok();
  }

  if (change.type == DDLChange::Type::kAlter) {
    std::string reason;
    if (ShouldCopyAlter(change.sql, &reason)) {
      if (!reason.empty()) {
        sql_print_information(
            "DuckDB binlog applier: Using copy-DDL fallback for ALTER (%s)",
            reason.c_str());
      }
      MySQLTableDef def;
      st = FetchTableDefFromSource(options, parsed.schema, change.table.table,
                                   &def);
      if (!st.ok()) return st;
      change.new_def = std::move(def);
      change.copy_ddl = true;
    }
    st = state->applier->ApplyDDL(std::move(change));
    if (!st.ok()) return st;
    state->schema_version = state->schema_version_loaded
                                ? state->schema_version + 1
                                : 1;
    state->schema_version_loaded = true;
    return Status::Ok();
  }

  st = state->applier->ApplyDDL(std::move(change));
  if (!st.ok()) return st;
  state->schema_version =
      state->schema_version_loaded ? state->schema_version + 1 : 1;
  state->schema_version_loaded = true;
  return Status::Ok();
}

Status EnsureSchemaApplier(const std::string &schema,
                           const BinlogApplyThreadOptions &options,
                           const DuckDBBinlogApplier::Options &applier_options,
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
    const std::string path = DuckdbPathForSchema(options, schema);
    Status st = entry.adapter->Init(path, cfg);
    if (!st.ok()) return st;
    entry.applier = std::make_unique<DuckDBBinlogApplier>(entry.adapter.get(),
                                                          applier_options);
    st = LoadSchemaVersion(&entry);
    if (!st.ok()) return st;
  }
  *out_state = &entry;
  return Status::Ok();
}

Status CommitActiveTransactions(
    std::map<std::string, SchemaApplierState> *states,
    const std::string &log_file, uint64_t log_pos) {
  if (!states) return Status::Ok();
  for (auto &entry : *states) {
    auto &state = entry.second;
    if (!state.txn_active || !state.applier) continue;
    if (!log_file.empty() && log_pos > 0) {
      state.applier->SetBinlogPosition(log_file, log_pos);
    }
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
    Status st = EnsureSchemaVersionFresh(&state);
    if (!st.ok()) return st;
    st = state.applier->BeginTransaction(Gtid{gtid});
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
  if (!options.start_file.empty() && !options.start_gtid_set.empty()) {
    return Status::Error(StatusCode::kInvalid,
                         "Cannot use start_file with start_gtid_set");
  }
  if (options.start_file.empty() && options.start_position != 0) {
    return Status::Error(StatusCode::kInvalid,
                         "start_position requires start_file");
  }

  const bool use_gtid = options.start_file.empty();
  std::string gtid_set;
  std::string source_gtid_set;
  std::string start_file;
  uint64_t start_pos = 0;

  ReplStateSnapshot state;
  Status st = ResolveReplState(options, &state);
  if (!st.ok()) return st;

  if (use_gtid) {
    if (!FetchGtidExecuted(options, &source_gtid_set)) {
      return Status::Error(StatusCode::kInvalid,
                           "Failed to fetch @@GLOBAL.GTID_EXECUTED");
    }
    if (source_gtid_set.empty()) {
      return Status::Error(StatusCode::kInvalid,
                           "Source GTID_EXECUTED is empty; cannot resume");
    }

    if (!options.start_gtid_set.empty()) {
      gtid_set = options.start_gtid_set;
      sql_print_information(
          "DuckDB binlog applier: Using configured start GTID set override");
    } else if (!state.applied_gtid_set.empty()) {
      bool subset = false;
      std::string error;
      if (!IsGtidSetSubset(state.applied_gtid_set, source_gtid_set, &subset,
                           &error)) {
        return Status::Error(StatusCode::kInvalid,
                             error.empty() ? "Failed to compare GTID sets"
                                           : error);
      }
      if (!subset) {
        return Status::Error(
            StatusCode::kInvalid,
            "DuckDB applied GTID set is not a subset of source GTID_EXECUTED; "
            "refusing to start to avoid data loss");
      }
      if (state.applied_gtid_set != source_gtid_set) {
        sql_print_information(
            "DuckDB binlog applier: Resuming from applied_gtid_set in %s "
            "(source has additional transactions)",
            state.source_path.empty() ? "(unknown)" : state.source_path.c_str());
      }
      gtid_set = std::move(state.applied_gtid_set);
    } else {
      gtid_set = source_gtid_set;
    }
  } else {
    if (!state.binlog_file.empty() && state.binlog_pos > 0) {
      start_file = state.binlog_file;
      start_pos = state.binlog_pos;
      sql_print_information(
          "DuckDB binlog applier: Resuming from binlog position %s:%llu",
          start_file.c_str(),
          static_cast<unsigned long long>(start_pos));
    } else {
      start_file = options.start_file;
      start_pos = options.start_position;
    }
    if (start_file.empty() || start_pos == 0) {
      return Status::Error(StatusCode::kInvalid,
                           "start_file and start_position are required for "
                           "file/position mode");
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
  stream_opts.start_file = start_file;
  stream_opts.start_position = start_pos;
  stream_opts.non_blocking = true;

  if (use_gtid) {
    sql_print_information(
        "DuckDB binlog applier: Opening GTID stream to %s:%u as %s, "
        "server_id=%u, gtid_set='%.200s'",
        stream_opts.host.c_str(), stream_opts.port, stream_opts.user.c_str(),
        stream_opts.server_id,
        stream_opts.gtid_set.empty() ? "(empty)" : stream_opts.gtid_set.c_str());
  } else {
    sql_print_information(
        "DuckDB binlog applier: Opening file/pos stream to %s:%u as %s, "
        "server_id=%u, start=%s:%llu",
        stream_opts.host.c_str(), stream_opts.port, stream_opts.user.c_str(),
        stream_opts.server_id, start_file.c_str(),
        static_cast<unsigned long long>(start_pos));
  }

  DuckDBBinlogStreamer streamer;
  st = streamer.Open(stream_opts);
  if (!st.ok()) {
    sql_print_warning("DuckDB binlog applier: Failed to open stream: %s",
                      st.message.c_str());
    return st;
  }
  sql_print_information("DuckDB binlog applier: Stream opened successfully");

  DuckDBBinlogApplier::Options applier_options;
  applier_options.use_gtid = use_gtid;

  std::map<std::string, SchemaApplierState> schema_states;
  std::string current_gtid;
  bool txn_open = false;
  std::string current_log_file;
  uint64_t current_log_pos = 0;

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

    if (!event.log_file.empty() && event.log_pos > 0) {
      current_log_file = event.log_file;
      current_log_pos = event.log_pos;
    }

    switch (event.type) {
      case BinlogEvent::Type::kGtid: {
        if (txn_open) {
          st = CommitActiveTransactions(&schema_states, current_log_file,
                                        current_log_pos);
          if (!st.ok()) return st;
        }
        current_gtid = event.gtid;
        txn_open = true;
        break;
      }
      case BinlogEvent::Type::kWriteRows:
      case BinlogEvent::Type::kUpdateRows:
      case BinlogEvent::Type::kDeleteRows: {
        if (use_gtid && (current_gtid.empty() || !txn_open)) {
          sql_print_warning("DuckDB binlog applier: row event without GTID");
          break;
        }
        if (!txn_open) {
          txn_open = true;
          current_gtid.clear();
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
        st = EnsureSchemaApplier(map->schema, options, applier_options,
                                 &schema_states, &state);
        if (!st.ok()) return st;
        st = ApplyRowEvent(event, *map, *state, current_gtid);
        if (!st.ok()) return st;
        break;
      }
      case BinlogEvent::Type::kQuery: {
        if (use_gtid && (current_gtid.empty() || !txn_open)) {
          break;
        }
        if (!txn_open) {
          txn_open = true;
          current_gtid.clear();
        }
        st = ApplyDdlEvent(event, options, applier_options, &schema_states,
                           current_gtid);
        if (!st.ok()) return st;
        break;
      }
      case BinlogEvent::Type::kXid: {
        st = CommitActiveTransactions(&schema_states, current_log_file,
                                      current_log_pos);
        if (!st.ok()) return st;
        txn_open = false;
        current_gtid.clear();
        break;
      }
      default:
        break;
    }
  }

  (void)CommitActiveTransactions(&schema_states, current_log_file,
                                 current_log_pos);
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
