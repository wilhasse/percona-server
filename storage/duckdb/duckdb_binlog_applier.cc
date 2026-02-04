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

#include "storage/duckdb/duckdb_binlog_applier.h"

#include "sql/log.h"
#include "storage/duckdb/duckdb_engine_utils.h"
#include "storage/duckdb/duckdb_gtid_utils.h"
#include "storage/duckdb/duckdb_repl_state.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

#define DUCKDB_APPLY_VERBOSE(...)                                           \
  do {                                                                      \
    if (duckdb_se::DuckdbBinlogApplyVerbose())                              \
      sql_print_information(__VA_ARGS__);                                   \
  } while (0)

namespace duckdb_se {
namespace {

struct ParsedUpdate {
  std::map<std::string, Cell> set_values;
  std::map<std::string, Cell> where_values;
};

struct BinlogApplyState {
  std::atomic<bool> paused{false};
  std::atomic<uint64_t> throttle_rows_per_sec{0};
  std::atomic<uint64_t> throttle_bytes_per_sec{0};
  std::atomic<uint64_t> lag_alert_threshold_ms{0};
  std::atomic<uint64_t> applied_transactions{0};
  std::atomic<uint64_t> applied_rows{0};
  std::atomic<uint64_t> applied_bytes{0};
  std::atomic<uint64_t> last_flush_rows{0};
  std::atomic<uint64_t> last_flush_bytes{0};
  std::atomic<uint64_t> last_flush_ms{0};
  std::atomic<uint64_t> last_throttle_ms{0};
  std::atomic<uint64_t> total_throttle_ms{0};
  std::atomic<uint64_t> last_commit_epoch_ms{0};
  std::atomic<uint64_t> last_commit_ms{0};
  std::atomic<uint64_t> gtid_lag{0};
  std::mutex pause_mutex;
  std::condition_variable pause_cv;
  std::mutex gtid_mutex;
  std::string last_gtid;
  std::string stop_at_gtid;
  std::string source_gtid_set;
};

constexpr const char *kReplChannel = "default";

Status ReadReplStateRow(duckdb::Connection &conn, std::string *snapshot,
                        std::string *applied, bool *found) {
  DUCKDB_APPLY_VERBOSE("DuckDB ReadReplStateRow: starting");
  if (snapshot) snapshot->clear();
  if (applied) applied->clear();
  if (found) *found = false;
  auto result = conn.Query(
      "SELECT snapshot_gtid_set, applied_gtid_set "
      "FROM __repl_state WHERE channel = '" +
      std::string(kReplChannel) + "' LIMIT 1");
  if (result->HasError()) {
    sql_print_warning("DuckDB ReadReplStateRow: query error: %s",
                      result->GetError().c_str());
    return Status::Error(StatusCode::kDuckDBError, result->GetError());
  }
  DUCKDB_APPLY_VERBOSE(
      "DuckDB ReadReplStateRow: query succeeded, fetching chunk");
  auto chunk = result->Fetch();
  if (!chunk || chunk->size() == 0) {
    DUCKDB_APPLY_VERBOSE("DuckDB ReadReplStateRow: no rows found");
    return Status::Ok();
  }
  DUCKDB_APPLY_VERBOSE("DuckDB ReadReplStateRow: found %zu rows",
                       static_cast<size_t>(chunk->size()));
  if (found) *found = true;
  DUCKDB_APPLY_VERBOSE(
      "DuckDB ReadReplStateRow: getting snapshot_gtid_set (col 0)");
  auto snap_val = chunk->GetValue(0, 0);
  DUCKDB_APPLY_VERBOSE(
      "DuckDB ReadReplStateRow: snapshot_gtid_set IsNull=%d",
      snap_val.IsNull() ? 1 : 0);
  if (snapshot && !snap_val.IsNull()) {
    DUCKDB_APPLY_VERBOSE(
        "DuckDB ReadReplStateRow: calling ToString on snapshot");
    *snapshot = snap_val.ToString();
    DUCKDB_APPLY_VERBOSE("DuckDB ReadReplStateRow: snapshot=%s",
                         snapshot->c_str());
  }
  DUCKDB_APPLY_VERBOSE(
      "DuckDB ReadReplStateRow: getting applied_gtid_set (col 1)");
  auto applied_val = chunk->GetValue(1, 0);
  DUCKDB_APPLY_VERBOSE(
      "DuckDB ReadReplStateRow: applied_gtid_set IsNull=%d",
      applied_val.IsNull() ? 1 : 0);
  if (applied && !applied_val.IsNull()) {
    DUCKDB_APPLY_VERBOSE(
        "DuckDB ReadReplStateRow: calling ToString on applied");
    *applied = applied_val.ToString();
    DUCKDB_APPLY_VERBOSE("DuckDB ReadReplStateRow: applied=%s",
                         applied->c_str());
  }
  DUCKDB_APPLY_VERBOSE("DuckDB ReadReplStateRow: done");
  return Status::Ok();
}

Status LoadWatermarkGtidSet(duckdb::Connection &conn, std::string *out_set) {
  DUCKDB_APPLY_VERBOSE("DuckDB LoadWatermarkGtidSet: starting");
  if (!out_set) {
    return Status::Error(StatusCode::kInvalid, "GTID set output is null");
  }
  out_set->clear();
  auto result =
      conn.Query("SELECT gtid FROM __repl_watermark ORDER BY commit_ts");
  if (result->HasError()) {
    if (IsMissingReplTableError(result->GetError(), "__repl_watermark")) {
      DUCKDB_APPLY_VERBOSE("DuckDB LoadWatermarkGtidSet: no watermark table");
      return Status::Ok();
    }
    sql_print_warning("DuckDB LoadWatermarkGtidSet: query error: %s",
                      result->GetError().c_str());
    return Status::Error(StatusCode::kDuckDBError, result->GetError());
  }
  DUCKDB_APPLY_VERBOSE("DuckDB LoadWatermarkGtidSet: query succeeded");
  std::vector<std::string> gtids;
  while (true) {
    auto chunk = result->Fetch();
    if (!chunk || chunk->size() == 0) break;
    DUCKDB_APPLY_VERBOSE(
        "DuckDB LoadWatermarkGtidSet: processing chunk of %zu rows",
        static_cast<size_t>(chunk->size()));
    for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
      DUCKDB_APPLY_VERBOSE(
          "DuckDB LoadWatermarkGtidSet: getting gtid at row %zu",
          static_cast<size_t>(row));
      auto gtid_val = chunk->GetValue(0, row);
      DUCKDB_APPLY_VERBOSE(
          "DuckDB LoadWatermarkGtidSet: gtid IsNull=%d",
          gtid_val.IsNull() ? 1 : 0);
      if (!gtid_val.IsNull()) {
        DUCKDB_APPLY_VERBOSE("DuckDB LoadWatermarkGtidSet: calling ToString");
        gtids.push_back(gtid_val.ToString());
        DUCKDB_APPLY_VERBOSE("DuckDB LoadWatermarkGtidSet: gtid=%s",
                             gtids.back().c_str());
      }
    }
  }
  DUCKDB_APPLY_VERBOSE("DuckDB LoadWatermarkGtidSet: collected %zu gtids",
                       gtids.size());
  if (gtids.empty()) return Status::Ok();
  std::string error;
  if (!BuildGtidSetFromList(gtids, out_set, &error)) {
    sql_print_warning("DuckDB LoadWatermarkGtidSet: BuildGtidSetFromList failed: %s",
                      error.c_str());
    return Status::Error(StatusCode::kInvalid,
                         error.empty() ? "Failed to build GTID set"
                                       : error);
  }
  DUCKDB_APPLY_VERBOSE("DuckDB LoadWatermarkGtidSet: done, set=%s",
                       out_set->c_str());
  return Status::Ok();
}

Status UpdateSchemaVersion(duckdb::Connection &conn) {
  std::string error;
  if (!EnsureReplStateTable(conn, &error)) {
    return Status::Error(StatusCode::kDuckDBError,
                         error.empty() ? "Failed to ensure __repl_state"
                                       : error);
  }
  // Initialize all columns with empty strings rather than NULL to avoid
  // potential UTF-8 validation issues in DuckDB when updating later.
  const std::string sql =
      "INSERT INTO __repl_state (channel, snapshot_gtid_set, applied_gtid_set, "
      "binlog_file, binlog_pos, last_commit_ts, schema_version) VALUES ('" +
      std::string(kReplChannel) +
      "', '', '', '', 0, '1970-01-01 00:00:00', 1) "
      "ON CONFLICT(channel) DO UPDATE SET schema_version = "
      "COALESCE(__repl_state.schema_version, 0) + 1";
  auto result = conn.Query(sql);
  if (result->HasError()) {
    return Status::Error(StatusCode::kDuckDBError, result->GetError());
  }
  return Status::Ok();
}

BinlogApplyState &GetApplyState() {
  static BinlogApplyState state;
  return state;
}

uint64_t NowEpochMs() {
  auto now = std::chrono::system_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch());
  return static_cast<uint64_t>(ms.count());
}

size_t EstimateRowBytes(const Row &row) {
  size_t bytes = 0;
  for (const auto &cell : row) {
    bytes += cell.value.size();
  }
  return bytes;
}

std::string GetCurrentTimestampString() {
  auto now = std::chrono::system_clock::now();
  auto time_t_now = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) %
            1000;
  std::tm tm_buf{};
#ifdef _WIN32
  gmtime_s(&tm_buf, &time_t_now);
#else
  gmtime_r(&time_t_now, &tm_buf);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S") << '.'
      << std::setfill('0') << std::setw(3) << ms.count();
  return oss.str();
}

uint64_t HashRow(const Row &row) {
  uint64_t hash = 1469598103934665603ull;
  auto mix = [&](const void *data, size_t len) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < len; ++i) {
      hash ^= bytes[i];
      hash *= 1099511628211ull;
    }
  };

  for (const auto &cell : row) {
    uint8_t flags = 0;
    if (cell.is_null) flags |= 0x1;
    if (cell.is_blob) flags |= 0x2;
    mix(&flags, sizeof(flags));
    const uint64_t len = static_cast<uint64_t>(cell.value.size());
    mix(&len, sizeof(len));
    if (!cell.value.empty()) {
      mix(cell.value.data(), cell.value.size());
    }
  }
  return hash;
}

bool RowsEqual(const Row &lhs, const Row &rhs) {
  if (lhs.size() != rhs.size()) return false;
  for (size_t i = 0; i < lhs.size(); ++i) {
    const auto &l = lhs[i];
    const auto &r = rhs[i];
    if (l.is_null != r.is_null || l.is_blob != r.is_blob ||
        l.value != r.value) {
      return false;
    }
  }
  return true;
}

bool HasRowWithHash(const std::vector<Row> &rows,
                    const std::map<uint64_t, std::vector<size_t>> &hash_index,
                    uint64_t hash, const Row &row) {
  auto it = hash_index.find(hash);
  if (it == hash_index.end()) return false;
  for (size_t idx : it->second) {
    if (idx < rows.size() && RowsEqual(rows[idx], row)) {
      return true;
    }
  }
  return false;
}

bool HasBulkUpdateConflict(
    const std::vector<Row> &old_rows, const std::vector<Row> &new_rows,
    const std::map<uint64_t, std::vector<size_t>> &old_hashes,
    const std::map<uint64_t, std::vector<size_t>> &new_hashes,
    uint64_t old_hash, const Row &old_row, uint64_t new_hash,
    const Row &new_row) {
  if (HasRowWithHash(new_rows, new_hashes, old_hash, old_row)) {
    return true;
  }
  if (HasRowWithHash(old_rows, old_hashes, new_hash, new_row)) {
    return true;
  }
  return false;
}

bool match_ci(const std::string &sql, size_t pos, const std::string &token) {
  if (pos + token.size() > sql.size()) return false;
  for (size_t i = 0; i < token.size(); ++i) {
    const char lhs = static_cast<char>(
        std::toupper(static_cast<unsigned char>(sql[pos + i])));
    const char rhs = static_cast<char>(
        std::toupper(static_cast<unsigned char>(token[i])));
    if (lhs != rhs) return false;
  }
  return true;
}

void SkipSpaces(const std::string &sql, size_t *pos) {
  while (*pos < sql.size() &&
         std::isspace(static_cast<unsigned char>(sql[*pos]))) {
    ++(*pos);
  }
}

bool ConsumeKeyword(const std::string &sql, size_t *pos,
                    const std::string &keyword) {
  SkipSpaces(sql, pos);
  if (!match_ci(sql, *pos, keyword)) return false;
  const size_t end = *pos + keyword.size();
  if (end < sql.size() &&
      !std::isspace(static_cast<unsigned char>(sql[end]))) {
    return false;
  }
  *pos = end;
  return true;
}

bool PeekKeyword(const std::string &sql, size_t pos,
                 const std::string &keyword) {
  SkipSpaces(sql, &pos);
  if (!match_ci(sql, pos, keyword)) return false;
  const size_t end = pos + keyword.size();
  if (end < sql.size() &&
      !std::isspace(static_cast<unsigned char>(sql[end]))) {
    return false;
  }
  return true;
}

bool ConsumeChar(const std::string &sql, size_t *pos, char ch) {
  SkipSpaces(sql, pos);
  if (*pos >= sql.size() || sql[*pos] != ch) return false;
  ++(*pos);
  return true;
}

bool ParseIdentifier(const std::string &sql, size_t *pos, std::string *out) {
  SkipSpaces(sql, pos);
  if (*pos >= sql.size()) return false;
  out->clear();
  if (sql[*pos] == '"') {
    ++(*pos);
    std::string value;
    while (*pos < sql.size()) {
      char ch = sql[*pos];
      if (ch == '"') {
        if (*pos + 1 < sql.size() && sql[*pos + 1] == '"') {
          value.push_back('"');
          *pos += 2;
          continue;
        }
        ++(*pos);
        break;
      }
      value.push_back(ch);
      ++(*pos);
    }
    *out = std::move(value);
    return !out->empty();
  }

  size_t start = *pos;
  while (*pos < sql.size()) {
    char ch = sql[*pos];
    if (std::isspace(static_cast<unsigned char>(ch)) || ch == '.' ||
        ch == '=' || ch == ',' || ch == ')') {
      break;
    }
    ++(*pos);
  }
  if (*pos == start) return false;
  *out = sql.substr(start, *pos - start);
  return true;
}

bool ParseQualifiedIdentifier(const std::string &sql, size_t *pos,
                              TableId *table) {
  if (!table) return false;
  table->schema.clear();
  table->table.clear();

  std::string first;
  if (!ParseIdentifier(sql, pos, &first)) return false;
  SkipSpaces(sql, pos);
  if (*pos < sql.size() && sql[*pos] == '.') {
    ++(*pos);
    std::string second;
    if (!ParseIdentifier(sql, pos, &second)) return false;
    table->schema = std::move(first);
    table->table = std::move(second);
    return true;
  }
  table->table = std::move(first);
  return true;
}

bool ParseValue(const std::string &sql, size_t *pos, Cell *out) {
  SkipSpaces(sql, pos);
  if (*pos >= sql.size()) return false;

  if (match_ci(sql, *pos, "NULL")) {
    const size_t end = *pos + 4;
    if (end == sql.size() || sql[end] == ',' ||
        std::isspace(static_cast<unsigned char>(sql[end]))) {
      out->is_null = true;
      out->is_blob = false;
      out->value.clear();
      *pos = end;
      return true;
    }
  }

  if (sql[*pos] == '\'') {
    ++(*pos);
    std::string value;
    while (*pos < sql.size()) {
      char ch = sql[*pos];
      if (ch == '\'') {
        if (*pos + 1 < sql.size() && sql[*pos + 1] == '\'') {
          value.push_back('\'');
          *pos += 2;
          continue;
        }
        ++(*pos);
        break;
      }
      value.push_back(ch);
      ++(*pos);
    }
    out->is_null = false;
    out->is_blob = false;
    out->value = std::move(value);
    return true;
  }

  size_t start = *pos;
  while (*pos < sql.size()) {
    char ch = sql[*pos];
    if (ch == ',' || std::isspace(static_cast<unsigned char>(ch))) {
      break;
    }
    ++(*pos);
  }
  if (*pos == start) return false;
  out->is_null = false;
  out->is_blob = false;
  out->value = sql.substr(start, *pos - start);
  return true;
}

bool ParseAssignments(const std::string &sql, size_t *pos,
                      std::map<std::string, Cell> *out) {
  if (!out) return false;
  out->clear();
  while (true) {
    std::string column;
    if (!ParseIdentifier(sql, pos, &column)) return false;
    if (!ConsumeChar(sql, pos, '=')) return false;
    Cell value;
    if (!ParseValue(sql, pos, &value)) return false;
    (*out)[column] = value;

    SkipSpaces(sql, pos);
    if (*pos < sql.size() && sql[*pos] == ',') {
      ++(*pos);
      continue;
    }
    return true;
  }
}

bool ParseWhereClause(const std::string &sql, size_t *pos,
                      std::map<std::string, Cell> *out) {
  if (!out) return false;
  out->clear();
  while (true) {
    std::string column;
    if (!ParseIdentifier(sql, pos, &column)) return false;
    if (!ConsumeKeyword(sql, pos, "IS")) return false;
    if (!ConsumeKeyword(sql, pos, "NOT")) return false;
    if (!ConsumeKeyword(sql, pos, "DISTINCT")) return false;
    if (!ConsumeKeyword(sql, pos, "FROM")) return false;
    Cell value;
    if (!ParseValue(sql, pos, &value)) return false;
    (*out)[column] = value;

    SkipSpaces(sql, pos);
    if (PeekKeyword(sql, *pos, "AND")) {
      ConsumeKeyword(sql, pos, "AND");
      continue;
    }
    return true;
  }
}

bool ParseUpdateStatement(const std::string &sql, ParsedUpdate *parsed) {
  if (!parsed) return false;
  parsed->set_values.clear();
  parsed->where_values.clear();

  size_t pos = 0;
  if (!ConsumeKeyword(sql, &pos, "UPDATE")) return false;

  TableId table;
  if (!ParseQualifiedIdentifier(sql, &pos, &table)) return false;

  if (!ConsumeKeyword(sql, &pos, "SET")) return false;
  if (!ParseAssignments(sql, &pos, &parsed->set_values)) return false;
  if (!ConsumeKeyword(sql, &pos, "WHERE")) return false;
  if (!ParseWhereClause(sql, &pos, &parsed->where_values)) return false;
  return true;
}

bool ParseDeleteStatement(const std::string &sql,
                          std::map<std::string, Cell> *where_values) {
  if (!where_values) return false;
  where_values->clear();

  size_t pos = 0;
  if (!ConsumeKeyword(sql, &pos, "DELETE")) return false;
  if (!ConsumeKeyword(sql, &pos, "FROM")) return false;

  TableId table;
  if (!ParseQualifiedIdentifier(sql, &pos, &table)) return false;

  if (!ConsumeKeyword(sql, &pos, "WHERE")) return false;
  if (!ParseWhereClause(sql, &pos, where_values)) return false;
  return true;
}

bool BuildUpdateRows(const std::vector<std::string> &columns,
                     const std::map<std::string, size_t> &column_index,
                     const ParsedUpdate &parsed, Row *old_row, Row *new_row) {
  if (!old_row || !new_row) return false;
  if (columns.empty()) return false;

  old_row->assign(columns.size(), Cell{});
  new_row->assign(columns.size(), Cell{});
  std::vector<bool> old_set(columns.size(), false);
  std::vector<bool> new_set(columns.size(), false);

  for (const auto &entry : parsed.where_values) {
    auto it = column_index.find(entry.first);
    if (it == column_index.end()) return false;
    old_row->at(it->second) = entry.second;
    old_set[it->second] = true;
  }
  for (const auto &entry : parsed.set_values) {
    auto it = column_index.find(entry.first);
    if (it == column_index.end()) return false;
    new_row->at(it->second) = entry.second;
    new_set[it->second] = true;
  }

  for (size_t i = 0; i < columns.size(); ++i) {
    if (!old_set[i]) return false;
    if (!new_set[i]) {
      new_row->at(i) = old_row->at(i);
    }
  }

  return true;
}

bool BuildDeleteRow(const std::vector<std::string> &columns,
                    const std::map<std::string, size_t> &column_index,
                    const std::map<std::string, Cell> &where_values,
                    Row *old_row) {
  if (!old_row) return false;
  if (columns.empty()) return false;

  old_row->assign(columns.size(), Cell{});
  std::vector<bool> old_set(columns.size(), false);
  for (const auto &entry : where_values) {
    auto it = column_index.find(entry.first);
    if (it == column_index.end()) return false;
    old_row->at(it->second) = entry.second;
    old_set[it->second] = true;
  }
  for (bool set : old_set) {
    if (!set) return false;
  }
  return true;
}

}  // namespace

BinlogApplyControls GetBinlogApplyControls() {
  auto &state = GetApplyState();
  BinlogApplyControls controls;
  controls.paused = state.paused.load();
  controls.throttle_rows_per_sec = state.throttle_rows_per_sec.load();
  controls.throttle_bytes_per_sec = state.throttle_bytes_per_sec.load();
  controls.lag_alert_threshold_ms = state.lag_alert_threshold_ms.load();
  {
    std::lock_guard<std::mutex> guard(state.gtid_mutex);
    controls.stop_at_gtid = state.stop_at_gtid;
  }
  return controls;
}

BinlogApplyMetrics GetBinlogApplyMetrics() {
  auto &state = GetApplyState();
  BinlogApplyMetrics metrics;
  metrics.applied_transactions = state.applied_transactions.load();
  metrics.applied_rows = state.applied_rows.load();
  metrics.applied_bytes = state.applied_bytes.load();
  metrics.last_flush_rows = state.last_flush_rows.load();
  metrics.last_flush_bytes = state.last_flush_bytes.load();
  metrics.last_flush_ms = state.last_flush_ms.load();
  metrics.last_throttle_ms = state.last_throttle_ms.load();
  metrics.total_throttle_ms = state.total_throttle_ms.load();
  metrics.last_commit_epoch_ms = state.last_commit_epoch_ms.load();
  metrics.last_commit_ms = state.last_commit_ms.load();
  if (metrics.last_commit_epoch_ms > 0) {
    const uint64_t now_ms = NowEpochMs();
    metrics.lag_ms =
        now_ms >= metrics.last_commit_epoch_ms
            ? now_ms - metrics.last_commit_epoch_ms
            : 0;
  }
  metrics.gtid_lag = state.gtid_lag.load();
  {
    std::lock_guard<std::mutex> guard(state.gtid_mutex);
    metrics.last_gtid = state.last_gtid;
    metrics.source_gtid_set = state.source_gtid_set;
  }
  const uint64_t lag_threshold = state.lag_alert_threshold_ms.load();
  metrics.lag_alert =
      lag_threshold > 0 && metrics.lag_ms >= lag_threshold;
  return metrics;
}

void SetBinlogApplyPaused(bool paused) {
  auto &state = GetApplyState();
  state.paused.store(paused);
  if (!paused) {
    state.pause_cv.notify_all();
  }
}

void SetBinlogApplyThrottleRowsPerSec(uint64_t rows_per_sec) {
  auto &state = GetApplyState();
  state.throttle_rows_per_sec.store(rows_per_sec);
}

void SetBinlogApplyThrottleBytesPerSec(uint64_t bytes_per_sec) {
  auto &state = GetApplyState();
  state.throttle_bytes_per_sec.store(bytes_per_sec);
}

void SetBinlogApplyLagAlertThresholdMs(uint64_t threshold_ms) {
  auto &state = GetApplyState();
  state.lag_alert_threshold_ms.store(threshold_ms);
}

void SetBinlogApplyStopAtGtid(const std::string &gtid) {
  auto &state = GetApplyState();
  std::lock_guard<std::mutex> guard(state.gtid_mutex);
  state.stop_at_gtid = gtid;
}

void SetBinlogApplySourceGtid(const std::string &gtid_set, uint64_t lag) {
  auto &state = GetApplyState();
  {
    std::lock_guard<std::mutex> guard(state.gtid_mutex);
    state.source_gtid_set = gtid_set;
  }
  state.gtid_lag.store(lag);
}

DuckDBBinlogApplier::DuckDBBinlogApplier(DuckDBAdapter *adapter,
                                         Options options)
    : adapter_(adapter), options_(options) {}

Status DuckDBBinlogApplier::BeginTransaction(Gtid gtid) {
  if (!adapter_) {
    return Status::Error(StatusCode::kInvalid, "DuckDBAdapter is null");
  }
  WaitIfPaused();
  if (in_txn_) {
    return Status::Error(StatusCode::kInvalid,
                         "Binlog transaction already active");
  }
  if (apply_txn_.active && !BatchingEnabled()) {
    return Status::Error(StatusCode::kInvalid,
                         "Apply transaction still active");
  }

  current_gtid_ = std::move(gtid);
  in_txn_ = true;
  skip_txn_ = false;
  if (!BatchingEnabled() || !apply_txn_.active) {
    ResetBuffers();
    apply_txn_ = ApplyTxn{};
  }
  if (!options_.use_gtid) {
    return Status::Ok();
  }
  bool already_applied = false;
  Status st = adapter_->IsGtidApplied(current_gtid_, &already_applied);
  if (!st.ok()) {
    in_txn_ = false;
    return st;
  }
  if (already_applied) {
    skip_txn_ = true;
  }
  return Status::Ok();
}

void DuckDBBinlogApplier::SetBinlogPosition(const std::string &file,
                                            uint64_t pos) {
  if (file.empty() || pos == 0) {
    return;
  }
  current_binlog_file_ = file;
  current_binlog_pos_ = pos;
  has_binlog_pos_ = true;
}

Status DuckDBBinlogApplier::AppendInsert(TableId table, Row row) {
  if (skip_txn_) {
    return Status::Ok();
  }
  RowBatch batch;
  batch.table = table;  // Copy table ID to batch
  batch.rows.emplace_back(std::move(row));
  return AppendInsertBatch(std::move(table), std::move(batch));
}

Status DuckDBBinlogApplier::AppendInsertBatch(TableId table, RowBatch batch) {
  return AppendInsertRows(std::move(table), std::move(batch));
}

Status DuckDBBinlogApplier::AppendInsertRows(TableId table, RowBatch batch) {
  if (!in_txn_) {
    return Status::Error(StatusCode::kInvalid, "No active binlog transaction");
  }
  WaitIfPaused();
  if (skip_txn_) {
    return Status::Ok();
  }
  if (batch.rows.empty()) {
    return Status::Ok();
  }

  TableKey key{table.schema, table.table};
  auto &buffer = buffers_[key];
  if (buffer.inserts.table.table.empty()) {
    buffer.inserts.table = table;
  }
  if (!have_buffered_) {
    have_buffered_ = true;
    first_event_time_ = std::chrono::steady_clock::now();
  }

  size_t rows_added = 0;
  size_t bytes_added = 0;
  for (const auto &row : batch.rows) {
    ++rows_added;
    bytes_added += EstimateRowBytes(row);
  }

  buffer.inserts.rows.reserve(buffer.inserts.rows.size() + batch.rows.size());
  for (auto &row : batch.rows) {
    buffer.inserts.rows.push_back(std::move(row));
  }

  buffer.row_count += rows_added;
  buffer.byte_count += bytes_added;
  buffered_rows_ += rows_added;
  buffered_bytes_ += bytes_added;

  return FlushBuffered(false);
}

Status DuckDBBinlogApplier::AppendUpdateRows(TableId table,
                                             BulkUpdateBatch batch) {
  if (!in_txn_) {
    return Status::Error(StatusCode::kInvalid, "No active binlog transaction");
  }
  WaitIfPaused();
  if (skip_txn_) {
    return Status::Ok();
  }
  if (batch.old_rows.empty()) {
    return Status::Ok();
  }
  if (batch.old_rows.size() != batch.new_rows.size()) {
    return Status::Error(StatusCode::kInvalid,
                         "Update row counts do not match");
  }

  const TableKey key{table.schema, table.table};
  auto &buffer = buffers_[key];
  if (!buffer.updates.statements.empty()) {
    Status st = FlushBuffered(true);
    if (!st.ok()) return st;
  }

  if (!have_buffered_) {
    have_buffered_ = true;
    first_event_time_ = std::chrono::steady_clock::now();
  }

  if (buffer.bulk_updates.table.table.empty()) {
    buffer.bulk_updates.table = table;
  }

  size_t rows_added = 0;
  size_t bytes_added = 0;
  for (size_t i = 0; i < batch.old_rows.size(); ++i) {
    const auto &old_row = batch.old_rows[i];
    const auto &new_row = batch.new_rows[i];
    ++rows_added;
    bytes_added += EstimateRowBytes(old_row);
    bytes_added += EstimateRowBytes(new_row);
    const uint64_t old_hash = HashRow(old_row);
    const uint64_t new_hash = HashRow(new_row);

    if (HasBulkUpdateConflict(buffer.bulk_updates.old_rows,
                              buffer.bulk_updates.new_rows,
                              buffer.bulk_update_old_hashes,
                              buffer.bulk_update_new_hashes, old_hash, old_row,
                              new_hash, new_row)) {
      Status st = FlushBuffered(true);
      if (!st.ok()) return st;
    }

    const size_t row_index = buffer.bulk_updates.old_rows.size();
    buffer.bulk_updates.old_rows.push_back(std::move(batch.old_rows[i]));
    buffer.bulk_updates.new_rows.push_back(std::move(batch.new_rows[i]));
    buffer.bulk_update_old_hashes[old_hash].push_back(row_index);
    buffer.bulk_update_new_hashes[new_hash].push_back(row_index);
  }

  buffer.row_count += rows_added;
  buffer.byte_count += bytes_added;
  buffered_rows_ += rows_added;
  buffered_bytes_ += bytes_added;

  return FlushBuffered(false);
}

Status DuckDBBinlogApplier::AppendDeleteRows(TableId table,
                                             BulkDeleteBatch batch) {
  if (!in_txn_) {
    return Status::Error(StatusCode::kInvalid, "No active binlog transaction");
  }
  WaitIfPaused();
  if (skip_txn_) {
    return Status::Ok();
  }
  if (batch.old_rows.empty()) {
    return Status::Ok();
  }

  const TableKey key{table.schema, table.table};
  auto &buffer = buffers_[key];
  if (!buffer.deletes.statements.empty()) {
    Status st = FlushBuffered(true);
    if (!st.ok()) return st;
  }

  if (!have_buffered_) {
    have_buffered_ = true;
    first_event_time_ = std::chrono::steady_clock::now();
  }

  if (buffer.bulk_deletes.table.table.empty()) {
    buffer.bulk_deletes.table = table;
  }

  size_t rows_added = 0;
  size_t bytes_added = 0;
  for (size_t i = 0; i < batch.old_rows.size(); ++i) {
    const auto &row = batch.old_rows[i];
    ++rows_added;
    bytes_added += EstimateRowBytes(row);
    buffer.bulk_deletes.old_rows.push_back(std::move(batch.old_rows[i]));
  }

  buffer.row_count += rows_added;
  buffer.byte_count += bytes_added;
  buffered_rows_ += rows_added;
  buffered_bytes_ += bytes_added;

  return FlushBuffered(false);
}

Status DuckDBBinlogApplier::AddUpdateStatement(TableId table, std::string sql) {
  if (!in_txn_) {
    return Status::Error(StatusCode::kInvalid, "No active binlog transaction");
  }
  WaitIfPaused();
  if (skip_txn_) {
    return Status::Ok();
  }
  if (sql.empty()) {
    return Status::Error(StatusCode::kInvalid, "Empty UPDATE statement");
  }

  const size_t bytes_added = sql.size();
  ParsedUpdate parsed;
  Row old_row;
  Row new_row;
  bool parsed_ok = ParseUpdateStatement(sql, &parsed);
  bool use_bulk = false;
  bool columns_ready = false;
  std::vector<std::string> columns;
  std::map<std::string, size_t> column_index;
  uint64_t old_hash = 0;
  uint64_t new_hash = 0;

  if (parsed_ok) {
    const TableKey key{table.schema, table.table};
    auto &buffer = buffers_[key];
    if (buffer.columns_loaded) {
      columns = buffer.columns;
      column_index = buffer.column_index;
      columns_ready = true;
    } else {
      Status st = adapter_->GetTableColumns(table, &columns);
      if (st.ok()) {
        columns_ready = true;
        for (size_t i = 0; i < columns.size(); ++i) {
          column_index.emplace(columns[i], i);
        }
      }
    }
    if (columns_ready &&
        BuildUpdateRows(columns, column_index, parsed, &old_row, &new_row)) {
      use_bulk = true;
      old_hash = HashRow(old_row);
      new_hash = HashRow(new_row);
    }
  }

  const TableKey key{table.schema, table.table};
  if (use_bulk) {
    const auto &buffer = buffers_[key];
    if (!buffer.updates.statements.empty() ||
        HasBulkUpdateConflict(buffer.bulk_updates.old_rows,
                              buffer.bulk_updates.new_rows,
                              buffer.bulk_update_old_hashes,
                              buffer.bulk_update_new_hashes, old_hash,
                              old_row, new_hash, new_row)) {
      Status st = FlushBuffered(true);
      if (!st.ok()) return st;
    }
  } else {
    if (!buffers_[key].bulk_updates.old_rows.empty()) {
      Status st = FlushBuffered(true);
      if (!st.ok()) return st;
    }
  }

  auto &buffer = buffers_[key];
  if (!have_buffered_) {
    have_buffered_ = true;
    first_event_time_ = std::chrono::steady_clock::now();
  }
  if (use_bulk) {
    if (!buffer.columns_loaded && columns_ready) {
      buffer.columns = std::move(columns);
      buffer.column_index = std::move(column_index);
      buffer.columns_loaded = true;
    }
    if (buffer.bulk_updates.table.table.empty()) {
      buffer.bulk_updates.table = table;
    }
    const size_t row_index = buffer.bulk_updates.old_rows.size();
    buffer.bulk_updates.old_rows.push_back(std::move(old_row));
    buffer.bulk_updates.new_rows.push_back(std::move(new_row));
    buffer.bulk_update_old_hashes[old_hash].push_back(row_index);
    buffer.bulk_update_new_hashes[new_hash].push_back(row_index);
  } else {
    if (buffer.updates.table.table.empty()) {
      buffer.updates.table = table;
    }
    buffer.updates.statements.push_back(std::move(sql));
  }
  buffer.row_count += 1;
  buffer.byte_count += bytes_added;
  buffered_rows_ += 1;
  buffered_bytes_ += bytes_added;

  return FlushBuffered(false);
}

Status DuckDBBinlogApplier::AddDeleteStatement(TableId table, std::string sql) {
  if (!in_txn_) {
    return Status::Error(StatusCode::kInvalid, "No active binlog transaction");
  }
  WaitIfPaused();
  if (skip_txn_) {
    return Status::Ok();
  }
  if (sql.empty()) {
    return Status::Error(StatusCode::kInvalid, "Empty DELETE statement");
  }

  const size_t bytes_added = sql.size();
  std::map<std::string, Cell> where_values;
  Row old_row;
  bool parsed_ok = ParseDeleteStatement(sql, &where_values);
  bool use_bulk = false;
  bool columns_ready = false;
  std::vector<std::string> columns;
  std::map<std::string, size_t> column_index;

  if (parsed_ok) {
    const TableKey key{table.schema, table.table};
    auto &buffer = buffers_[key];
    if (buffer.columns_loaded) {
      columns = buffer.columns;
      column_index = buffer.column_index;
      columns_ready = true;
    } else {
      Status st = adapter_->GetTableColumns(table, &columns);
      if (st.ok()) {
        columns_ready = true;
        for (size_t i = 0; i < columns.size(); ++i) {
          column_index.emplace(columns[i], i);
        }
      }
    }
    if (columns_ready &&
        BuildDeleteRow(columns, column_index, where_values, &old_row)) {
      use_bulk = true;
    }
  }

  const TableKey key{table.schema, table.table};
  if (use_bulk && !buffers_[key].deletes.statements.empty()) {
    Status st = FlushBuffered(true);
    if (!st.ok()) return st;
  }
  if (!use_bulk && !buffers_[key].bulk_deletes.old_rows.empty()) {
    Status st = FlushBuffered(true);
    if (!st.ok()) return st;
  }

  auto &buffer = buffers_[key];
  if (!have_buffered_) {
    have_buffered_ = true;
    first_event_time_ = std::chrono::steady_clock::now();
  }
  if (use_bulk) {
    if (!buffer.columns_loaded && columns_ready) {
      buffer.columns = std::move(columns);
      buffer.column_index = std::move(column_index);
      buffer.columns_loaded = true;
    }
    if (buffer.bulk_deletes.table.table.empty()) {
      buffer.bulk_deletes.table = table;
    }
    buffer.bulk_deletes.old_rows.push_back(std::move(old_row));
  } else {
    if (buffer.deletes.table.table.empty()) {
      buffer.deletes.table = table;
    }
    buffer.deletes.statements.push_back(std::move(sql));
  }
  buffer.row_count += 1;
  buffer.byte_count += bytes_added;
  buffered_rows_ += 1;
  buffered_bytes_ += bytes_added;

  return FlushBuffered(false);
}

Status DuckDBBinlogApplier::ApplyDDL(DDLChange change) {
  if (!in_txn_) {
    return Status::Error(StatusCode::kInvalid, "No active binlog transaction");
  }
  WaitIfPaused();
  if (skip_txn_) {
    return Status::Ok();
  }

  txn_has_ddl_ = true;
  Status st = FlushBuffered(true);
  if (!st.ok()) {
    return st;
  }

  st = EnsureApplyTxn();
  if (!st.ok()) {
    return st;
  }

  st = adapter_->CloseAppenders(apply_txn_);
  if (!st.ok()) return st;

  st = adapter_->ApplyDDLInTxn(apply_txn_, std::move(change));
  if (!st.ok()) return st;
  return UpdateSchemaVersion(*apply_txn_.conn);
}

Status DuckDBBinlogApplier::CommitTransaction() {
  DUCKDB_APPLY_VERBOSE("DuckDB CommitTransaction: starting");
  if (!in_txn_) {
    return Status::Error(StatusCode::kInvalid, "No active binlog transaction");
  }
  WaitIfPaused();
  if (skip_txn_) {
    in_txn_ = false;
    skip_txn_ = false;
    ResetBuffers();
    txn_has_ddl_ = false;
    return Status::Ok();
  }

  DUCKDB_APPLY_VERBOSE("DuckDB CommitTransaction: calling FlushBuffered");
  Status st = FlushBuffered(true);
  if (!st.ok()) {
    sql_print_warning("DuckDB CommitTransaction: FlushBuffered failed: %s",
                      st.message.c_str());
    RollbackTransaction();
    return st;
  }

  DUCKDB_APPLY_VERBOSE("DuckDB CommitTransaction: calling EnsureApplyTxn");
  st = EnsureApplyTxn();
  if (!st.ok()) {
    sql_print_warning("DuckDB CommitTransaction: EnsureApplyTxn failed: %s",
                      st.message.c_str());
    RollbackTransaction();
    return st;
  }

  bool force_commit = !options_.use_gtid || txn_has_ddl_;
  bool should_pause = false;
  auto &state = GetApplyState();
  {
    std::lock_guard<std::mutex> guard(state.gtid_mutex);
    if (!current_gtid_.value.empty()) {
      batch_gtids_.push_back(current_gtid_.value);
      ++batch_gtid_count_;
      if (batch_start_time_ == std::chrono::steady_clock::time_point{}) {
        batch_start_time_ = std::chrono::steady_clock::now();
      }
      if (!state.stop_at_gtid.empty() &&
          state.stop_at_gtid == current_gtid_.value) {
        force_commit = true;
        should_pause = true;
      }
    }
  }

  if (!ShouldCommitBatch(force_commit)) {
    in_txn_ = false;
    txn_has_ddl_ = false;
    ResetBuffers();
    return Status::Ok();
  }

  const auto commit_start = std::chrono::steady_clock::now();
  DUCKDB_APPLY_VERBOSE("DuckDB CommitTransaction: calling ApplyWatermark");
  st = ApplyWatermark();
  if (!st.ok()) {
    sql_print_warning("DuckDB CommitTransaction: ApplyWatermark failed: %s",
                      st.message.c_str());
    RollbackTransaction();
    return st;
  }

  DUCKDB_APPLY_VERBOSE("DuckDB CommitTransaction: calling CommitApplyTxn");
  st = adapter_->CommitApplyTxn(apply_txn_);
  if (!st.ok()) {
    RollbackTransaction();
    return st;
  }
  const auto commit_end = std::chrono::steady_clock::now();
  const auto commit_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(commit_end -
                                                            commit_start);
  state.applied_transactions.fetch_add(1);
  state.last_commit_epoch_ms.store(NowEpochMs());
  state.last_commit_ms.store(static_cast<uint64_t>(commit_ms.count()));
  {
    std::lock_guard<std::mutex> guard(state.gtid_mutex);
    state.last_gtid = current_gtid_.value;
  }
  if (should_pause) {
    SetBinlogApplyPaused(true);
  }

  apply_txn_ = ApplyTxn{};
  in_txn_ = false;
  txn_has_ddl_ = false;
  ResetBatchState();
  ResetBuffers();
  return Status::Ok();
}

Status DuckDBBinlogApplier::RollbackTransaction() {
  if (!in_txn_) {
    return Status::Error(StatusCode::kInvalid, "No active binlog transaction");
  }

  Status st = Status::Ok();
  if (skip_txn_) {
    st = Status::Ok();
  } else if (apply_txn_.active) {
    st = adapter_->RollbackApplyTxn(apply_txn_);
  }

  apply_txn_ = ApplyTxn{};
  in_txn_ = false;
  skip_txn_ = false;
  txn_has_ddl_ = false;
  ResetBatchState();
  ResetBuffers();
  return st;
}

Status DuckDBBinlogApplier::EnsureApplyTxn() {
  if (apply_txn_.active) {
    return Status::Ok();
  }
  if (!adapter_) {
    return Status::Error(StatusCode::kInvalid, "DuckDBAdapter is null");
  }

  apply_txn_ = adapter_->BeginApplyTxn(current_gtid_);
  if (!apply_txn_.status.ok()) {
    return apply_txn_.status;
  }
  if (!apply_txn_.active) {
    return Status::Error(StatusCode::kDuckDBError,
                         "DuckDB apply transaction not active");
  }
  return Status::Ok();
}

Status DuckDBBinlogApplier::FlushBuffered(bool force) {
  if (!have_buffered_) {
    return Status::Ok();
  }
  if (!force && !ShouldFlush()) {
    return Status::Ok();
  }

  WaitIfPaused();
  const auto apply_start = std::chrono::steady_clock::now();
  Status st = EnsureApplyTxn();
  if (!st.ok()) {
    return st;
  }

  for (auto &entry : buffers_) {
    auto &buffer = entry.second;
    if (!buffer.inserts.rows.empty()) {
      RowBatch batch = std::move(buffer.inserts);
      TableId table = batch.table;  // Copy before move to avoid UB
      st = adapter_->AppendRows(apply_txn_, std::move(table), std::move(batch));
      if (!st.ok()) return st;
    }
    if (!buffer.bulk_updates.old_rows.empty()) {
      BulkUpdateBatch batch = std::move(buffer.bulk_updates);
      TableId table = batch.table;  // Copy before move to avoid UB
      st = adapter_->ApplyBulkUpdates(apply_txn_, std::move(table),
                                      std::move(batch));
      if (!st.ok()) return st;
    }
    if (!buffer.updates.statements.empty()) {
      UpdateBatch batch = std::move(buffer.updates);
      TableId table = batch.table;  // Copy before move to avoid UB
      st = adapter_->ApplyUpdates(apply_txn_, std::move(table), std::move(batch));
      if (!st.ok()) return st;
    }
    if (!buffer.bulk_deletes.old_rows.empty()) {
      BulkDeleteBatch batch = std::move(buffer.bulk_deletes);
      TableId table = batch.table;  // Copy before move to avoid UB
      st = adapter_->ApplyBulkDeletes(apply_txn_, std::move(table),
                                      std::move(batch));
      if (!st.ok()) return st;
    }
    if (!buffer.deletes.statements.empty()) {
      DeleteBatch batch = std::move(buffer.deletes);
      TableId table = batch.table;  // Copy before move to avoid UB
      st = adapter_->ApplyDeletes(apply_txn_, std::move(table), std::move(batch));
      if (!st.ok()) return st;
    }
  }

  const auto apply_end = std::chrono::steady_clock::now();
  const auto apply_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(apply_end -
                                                            apply_start);
  const size_t flush_rows = buffered_rows_;
  const size_t flush_bytes = buffered_bytes_;
  UpdateMetrics(flush_rows, flush_bytes, apply_ms);
  MaybeThrottle(flush_rows, flush_bytes, apply_ms);
  ResetBuffers();
  return Status::Ok();
}

bool DuckDBBinlogApplier::ShouldFlush() const {
  if (!have_buffered_) {
    return false;
  }
  if (options_.max_rows > 0 && buffered_rows_ >= options_.max_rows) {
    return true;
  }
  if (options_.max_bytes > 0 && buffered_bytes_ >= options_.max_bytes) {
    return true;
  }
  if (options_.max_delay.count() > 0) {
    auto now = std::chrono::steady_clock::now();
    if (now - first_event_time_ >= options_.max_delay) {
      return true;
    }
  }
  return false;
}

void DuckDBBinlogApplier::WaitIfPaused() const {
  auto &state = GetApplyState();
  if (!state.paused.load()) {
    return;
  }
  std::unique_lock<std::mutex> lock(state.pause_mutex);
  state.pause_cv.wait(lock, [&state]() { return !state.paused.load(); });
}

void DuckDBBinlogApplier::UpdateMetrics(size_t rows, size_t bytes,
                                        std::chrono::milliseconds apply_ms) {
  auto &state = GetApplyState();
  state.last_flush_rows.store(rows);
  state.last_flush_bytes.store(bytes);
  state.last_flush_ms.store(static_cast<uint64_t>(apply_ms.count()));
  state.applied_rows.fetch_add(rows);
  state.applied_bytes.fetch_add(bytes);
  batch_rows_ += rows;
  batch_bytes_ += bytes;
}

void DuckDBBinlogApplier::MaybeThrottle(size_t rows, size_t bytes,
                                        std::chrono::milliseconds apply_ms) {
  auto &state = GetApplyState();
  const uint64_t rows_per_sec = state.throttle_rows_per_sec.load();
  const uint64_t bytes_per_sec = state.throttle_bytes_per_sec.load();
  if (rows_per_sec == 0 && bytes_per_sec == 0) {
    state.last_throttle_ms.store(0);
    return;
  }

  double required_seconds = 0.0;
  if (rows_per_sec > 0 && rows > 0) {
    required_seconds = std::max(
        required_seconds, static_cast<double>(rows) / rows_per_sec);
  }
  if (bytes_per_sec > 0 && bytes > 0) {
    required_seconds = std::max(
        required_seconds, static_cast<double>(bytes) / bytes_per_sec);
  }
  if (required_seconds <= 0.0) {
    state.last_throttle_ms.store(0);
    return;
  }

  const uint64_t required_ms =
      static_cast<uint64_t>(std::ceil(required_seconds * 1000.0));
  const uint64_t apply_ms_value =
      static_cast<uint64_t>(apply_ms.count());
  if (required_ms > apply_ms_value) {
    const uint64_t sleep_ms = required_ms - apply_ms_value;
    state.last_throttle_ms.store(sleep_ms);
    state.total_throttle_ms.fetch_add(sleep_ms);
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  } else {
    state.last_throttle_ms.store(0);
  }
}

void DuckDBBinlogApplier::ResetBuffers() {
  buffers_.clear();
  buffered_rows_ = 0;
  buffered_bytes_ = 0;
  have_buffered_ = false;
  first_event_time_ = std::chrono::steady_clock::time_point{};
}

Status DuckDBBinlogApplier::ApplyWatermark() {
  DUCKDB_APPLY_VERBOSE("DuckDB ApplyWatermark: starting, gtid=%s",
                       current_gtid_.value.c_str());
  if (!apply_txn_.active || !apply_txn_.conn) {
    return Status::Error(StatusCode::kInvalid, "Apply transaction not active");
  }
  std::string error;
  DUCKDB_APPLY_VERBOSE("DuckDB ApplyWatermark: ensuring repl state table");
  if (!EnsureReplStateTable(*apply_txn_.conn, &error)) {
    sql_print_warning("DuckDB ApplyWatermark: EnsureReplStateTable failed: %s",
                      error.c_str());
    return Status::Error(StatusCode::kDuckDBError,
                         error.empty() ? "Failed to ensure __repl_state"
                                       : error);
  }
  DUCKDB_APPLY_VERBOSE("DuckDB ApplyWatermark: repl state table ensured");

  if (!repl_state_loaded_) {
    bool found = false;
    std::string snapshot;
    std::string applied_set;
    Status st =
        ReadReplStateRow(*apply_txn_.conn, &snapshot, &applied_set, &found);
    if (!st.ok()) return st;
    repl_state_loaded_ = true;
    repl_state_found_ = found;
    repl_snapshot_set_ = std::move(snapshot);
    repl_applied_set_ = std::move(applied_set);
  }

  bool found = repl_state_found_;
  std::string snapshot = repl_snapshot_set_;
  std::string applied_set = repl_applied_set_;

  const std::string commit_ts = GetCurrentTimestampString();
  std::string sql;
  const bool update_binlog = has_binlog_pos_;

  if (options_.use_gtid) {
    if (applied_set.empty() && !snapshot.empty()) {
      applied_set = snapshot;
    }
    if (applied_set.empty()) {
      std::string watermark_set;
      Status st = LoadWatermarkGtidSet(*apply_txn_.conn, &watermark_set);
      if (!st.ok()) return st;
      if (!watermark_set.empty()) {
        applied_set = std::move(watermark_set);
      }
    }
    if (!applied_set.empty() && repl_applied_set_.empty()) {
      repl_applied_set_ = applied_set;
    }

    std::string merged_set = applied_set;
    const bool use_batch = !batch_gtids_.empty();
    const auto &gtids = use_batch ? batch_gtids_
                                  : std::vector<std::string>{current_gtid_.value};
    for (const auto &gtid : gtids) {
      if (gtid.empty()) continue;
      DUCKDB_APPLY_VERBOSE(
          "DuckDB ApplyWatermark: calling MergeGtidIntoSet, applied_set='%s', "
          "gtid='%s'",
          merged_set.c_str(), gtid.c_str());
      std::string next_set;
      if (!MergeGtidIntoSet(merged_set, gtid, &next_set, &error)) {
        sql_print_warning("DuckDB ApplyWatermark: MergeGtidIntoSet failed: %s",
                          error.c_str());
        return Status::Error(StatusCode::kInvalid,
                             error.empty() ? "Failed to merge GTID" : error);
      }
      merged_set = std::move(next_set);
    }
    DUCKDB_APPLY_VERBOSE(
        "DuckDB ApplyWatermark: MergeGtidIntoSet succeeded, merged_set='%s'",
        merged_set.c_str());

    if (found) {
      // Use DELETE + INSERT instead of UPDATE to work around DuckDB UTF-8
      // validation issue that can occur during UPDATE processing.
      DUCKDB_APPLY_VERBOSE(
          "DuckDB ApplyWatermark: building DELETE+INSERT SQL (found=true)");
      std::string delete_sql = "DELETE FROM __repl_state WHERE channel = '" +
                               std::string(kReplChannel) + "'";
      auto del_result = apply_txn_.conn->Query(delete_sql);
      if (del_result->HasError()) {
        sql_print_warning("DuckDB ApplyWatermark: DELETE failed: %s",
                          del_result->GetError().c_str());
        return Status::Error(StatusCode::kDuckDBError, del_result->GetError());
      }
      DUCKDB_APPLY_VERBOSE("DuckDB ApplyWatermark: DELETE succeeded");
      // Fall through to INSERT path below
    }
    // Always INSERT (either fresh row or after DELETE)
    {
      DUCKDB_APPLY_VERBOSE(
          "DuckDB ApplyWatermark: building INSERT SQL, binlog_file='%s' len=%zu",
          current_binlog_file_.c_str(), current_binlog_file_.size());
      const std::string snapshot_sql =
          snapshot.empty() ? "''" : "'" + EscapeLiteral(snapshot) + "'";
      std::string insert_cols =
          "channel, snapshot_gtid_set, applied_gtid_set, last_commit_ts";
      std::string insert_vals = "'" + std::string(kReplChannel) + "', " +
                                snapshot_sql + ", '" +
                                EscapeLiteral(merged_set) + "', '" +
                                EscapeLiteral(commit_ts) + "'";
      if (update_binlog) {
        insert_cols += ", binlog_file, binlog_pos";
        insert_vals += ", '" + EscapeLiteral(current_binlog_file_) + "', " +
                       std::to_string(current_binlog_pos_);
      }
      sql = "INSERT INTO __repl_state (" + insert_cols + ") VALUES (" +
            insert_vals + ")";
    }
    repl_applied_set_ = merged_set;
  } else {
    if (!update_binlog) {
      return Status::Error(StatusCode::kInvalid,
                           "Binlog position not set for watermark");
    }
    if (found) {
      sql = "UPDATE __repl_state SET binlog_file = '" +
            EscapeLiteral(current_binlog_file_) + "', binlog_pos = " +
            std::to_string(current_binlog_pos_) + ", last_commit_ts = '" +
            EscapeLiteral(commit_ts) + "' WHERE channel = '" +
            std::string(kReplChannel) + "'";
    } else {
      std::string snapshot_sql =
          snapshot.empty() ? "NULL" : "'" + EscapeLiteral(snapshot) + "'";
      std::string applied_sql =
          applied_set.empty()
              ? (snapshot.empty() ? "NULL" : snapshot_sql)
              : "'" + EscapeLiteral(applied_set) + "'";
      sql =
          "INSERT INTO __repl_state (channel, snapshot_gtid_set, "
          "applied_gtid_set, last_commit_ts, binlog_file, binlog_pos) VALUES "
          "('" +
          std::string(kReplChannel) + "', " + snapshot_sql + ", " +
          applied_sql + ", '" + EscapeLiteral(commit_ts) + "', '" +
          EscapeLiteral(current_binlog_file_) + "', " +
          std::to_string(current_binlog_pos_) + ")";
    }
  }

  DUCKDB_APPLY_VERBOSE("DuckDB ApplyWatermark: executing SQL: %s", sql.c_str());
  try {
    auto result = apply_txn_.conn->Query(sql);
    bool has_error = result->HasError();
    if (has_error) {
      std::string err = result->GetError();
      sql_print_warning("DuckDB ApplyWatermark: Query error: %s", err.c_str());
      return Status::Error(StatusCode::kDuckDBError, err);
    }
  } catch (const std::exception &ex) {
    sql_print_warning("DuckDB ApplyWatermark: Query exception: %s", ex.what());
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  repl_state_found_ = true;
  DUCKDB_APPLY_VERBOSE("DuckDB ApplyWatermark: done");
  return Status::Ok();
}

std::string DuckDBBinlogApplier::EscapeLiteral(const std::string &value) const {
  std::string out;
  out.reserve(value.size() + 4);
  for (char ch : value) {
    if (ch == '\'') out.push_back('\'');
    out.push_back(ch);
  }
  return out;
}

bool DuckDBBinlogApplier::BatchingEnabled() const {
  if (!options_.use_gtid) return false;
  return options_.batch_max_gtids > 1 || options_.batch_max_rows > 0 ||
         options_.batch_max_bytes > 0 ||
         options_.batch_max_delay.count() > 0;
}

bool DuckDBBinlogApplier::ShouldCommitBatch(bool force_commit) const {
  if (!BatchingEnabled()) return true;
  if (force_commit) return true;
  if (options_.batch_max_gtids > 0 &&
      batch_gtid_count_ >= options_.batch_max_gtids) {
    return true;
  }
  if (options_.batch_max_rows > 0 && batch_rows_ >= options_.batch_max_rows) {
    return true;
  }
  if (options_.batch_max_bytes > 0 && batch_bytes_ >= options_.batch_max_bytes) {
    return true;
  }
  if (options_.batch_max_delay.count() > 0 &&
      batch_start_time_ != std::chrono::steady_clock::time_point{}) {
    auto now = std::chrono::steady_clock::now();
    if (now - batch_start_time_ >= options_.batch_max_delay) {
      return true;
    }
  }
  return false;
}

void DuckDBBinlogApplier::ResetBatchState() {
  batch_gtids_.clear();
  batch_gtid_count_ = 0;
  batch_rows_ = 0;
  batch_bytes_ = 0;
  batch_start_time_ = std::chrono::steady_clock::time_point{};
}

}  // namespace duckdb_se
