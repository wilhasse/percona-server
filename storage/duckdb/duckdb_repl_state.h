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

#ifndef PLUGIN_DUCKDB_REPL_STATE_H_
#define PLUGIN_DUCKDB_REPL_STATE_H_

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "duckdb.hpp"

namespace duckdb_se {

// Generate a DuckDB-compatible timestamp literal for the current time.
// Uses C++ chrono to avoid reliance on DuckDB's now()/CURRENT_TIMESTAMP
// functions which may require core_functions extension.
inline std::string ReplStateTimestampLiteral() {
  auto now = std::chrono::system_clock::now();
  auto time_t_now = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) %
            1000;
  std::tm tm_buf;
  gmtime_r(&time_t_now, &tm_buf);
  char buf[80];
  std::snprintf(buf, sizeof(buf), "'%04d-%02d-%02d %02d:%02d:%02d.%03d'",
                tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                static_cast<int>(ms.count()));
  return buf;
}

inline std::string EscapeReplStateLiteral(const std::string &value) {
  std::string out;
  out.reserve(value.size() + 2);
  for (char ch : value) {
    if (ch == '\'') out.push_back('\'');
    out.push_back(ch);
  }
  return out;
}

inline bool IsMissingReplTableError(const std::string &error,
                                    const std::string &table) {
  return error.find("does not exist") != std::string::npos &&
         error.find(table) != std::string::npos;
}

inline bool FetchReplTableColumns(duckdb::Connection &conn,
                                  const std::string &table,
                                  std::vector<std::string> *columns,
                                  bool *missing, std::string *error) {
  if (!columns || !missing) return false;
  columns->clear();
  *missing = false;
  const std::string sql =
      "PRAGMA table_info('" + EscapeReplStateLiteral(table) + "')";
  auto result = conn.Query(sql);
  if (result->HasError()) {
    if (IsMissingReplTableError(result->GetError(), table)) {
      *missing = true;
      return true;
    }
    if (error) *error = result->GetError();
    return false;
  }
  while (true) {
    auto chunk = result->Fetch();
    if (!chunk || chunk->size() == 0) break;
    for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
      auto val = chunk->GetValue(1, row);
      if (!val.IsNull()) {
        columns->push_back(val.ToString());
      }
    }
  }
  return true;
}

inline bool EnsureReplStateTable(duckdb::Connection &conn,
                                 std::string *error) {
  constexpr const char *kStateTable = "__repl_state";
  std::vector<std::string> columns;
  bool missing = false;
  if (!FetchReplTableColumns(conn, kStateTable, &columns, &missing, error)) {
    return false;
  }
  if (missing) {
    const std::string create_sql =
        "CREATE TABLE IF NOT EXISTS __repl_state ("
        "channel VARCHAR PRIMARY KEY, "
        "snapshot_gtid_set VARCHAR, "
        "applied_gtid_set VARCHAR, "
        "binlog_file VARCHAR, "
        "binlog_pos BIGINT, "
        "last_commit_ts TIMESTAMP, "
        "schema_version BIGINT)";
    auto create = conn.Query(create_sql);
    if (create->HasError()) {
      if (error) *error = create->GetError();
      return false;
    }
    return true;
  }

  bool has_channel = false;
  bool has_snapshot = false;
  bool has_applied = false;
  bool has_last_commit = false;
  bool has_schema_version = false;
  bool has_binlog_file = false;
  bool has_binlog_pos = false;
  bool has_id = false;
  bool has_updated = false;
  for (const auto &col : columns) {
    if (col == "channel") has_channel = true;
    if (col == "snapshot_gtid_set") has_snapshot = true;
    if (col == "applied_gtid_set") has_applied = true;
    if (col == "binlog_file") has_binlog_file = true;
    if (col == "binlog_pos") has_binlog_pos = true;
    if (col == "last_commit_ts") has_last_commit = true;
    if (col == "schema_version") has_schema_version = true;
    if (col == "id") has_id = true;
    if (col == "updated_ts") has_updated = true;
  }
  if (has_channel && has_snapshot && has_applied && has_last_commit) {
    if (!has_schema_version) {
      auto alter =
          conn.Query("ALTER TABLE __repl_state ADD COLUMN schema_version BIGINT");
      if (alter->HasError()) {
        if (error) *error = alter->GetError();
        return false;
      }
    }
    if (!has_binlog_file) {
      auto alter =
          conn.Query("ALTER TABLE __repl_state ADD COLUMN binlog_file VARCHAR");
      if (alter->HasError()) {
        if (error) *error = alter->GetError();
        return false;
      }
    }
    if (!has_binlog_pos) {
      auto alter =
          conn.Query("ALTER TABLE __repl_state ADD COLUMN binlog_pos BIGINT");
      if (alter->HasError()) {
        if (error) *error = alter->GetError();
        return false;
      }
    }
    return true;
  }

  if (has_id) {
    std::string snapshot;
    std::string updated;
    const std::string select_sql =
        has_updated
            ? "SELECT snapshot_gtid_set, updated_ts FROM __repl_state WHERE id = 1"
            : "SELECT snapshot_gtid_set FROM __repl_state WHERE id = 1";
    auto sel = conn.Query(select_sql);
    if (!sel->HasError()) {
      auto chunk = sel->Fetch();
      if (chunk && chunk->size() > 0) {
        auto snap_val = chunk->GetValue(0, 0);
        if (!snap_val.IsNull()) snapshot = snap_val.ToString();
        if (has_updated) {
          auto ts_val = chunk->GetValue(1, 0);
          if (!ts_val.IsNull()) updated = ts_val.ToString();
        }
      }
    }
    auto create = conn.Query(
        "CREATE TABLE __repl_state_new ("
        "channel VARCHAR PRIMARY KEY, "
        "snapshot_gtid_set VARCHAR, "
        "applied_gtid_set VARCHAR, "
        "binlog_file VARCHAR, "
        "binlog_pos BIGINT, "
        "last_commit_ts TIMESTAMP, "
        "schema_version BIGINT)");
    if (create->HasError()) {
      if (error) *error = create->GetError();
      return false;
    }
    std::string snapshot_sql =
        snapshot.empty() ? "NULL" : "'" + EscapeReplStateLiteral(snapshot) + "'";
    std::string ts_sql = updated.empty()
                             ? ReplStateTimestampLiteral()
                             : "'" + EscapeReplStateLiteral(updated) + "'";
    const std::string insert_sql =
        "INSERT INTO __repl_state_new (channel, snapshot_gtid_set, "
        "applied_gtid_set, last_commit_ts, schema_version, binlog_file, "
        "binlog_pos) "
        "VALUES ('default', " +
        snapshot_sql + ", " + snapshot_sql + ", " + ts_sql +
        ", NULL, NULL, NULL)";
    auto insert = conn.Query(insert_sql);
    if (insert->HasError()) {
      if (error) *error = insert->GetError();
      return false;
    }
    auto drop_old = conn.Query("DROP TABLE __repl_state");
    if (drop_old->HasError()) {
      if (error) *error = drop_old->GetError();
      return false;
    }
    auto rename = conn.Query("ALTER TABLE __repl_state_new RENAME TO __repl_state");
    if (rename->HasError()) {
      if (error) *error = rename->GetError();
      return false;
    }
    return true;
  }

  if (error) {
    *error = "Unexpected __repl_state schema; manual migration required";
  }
  return false;
}

}  // namespace duckdb_se

#endif  // PLUGIN_DUCKDB_REPL_STATE_H_
