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

#include "storage/duckdb/duckdb_adapter.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <unordered_set>

#include "sql/log.h"
#include "storage/duckdb/duckdb_engine_utils.h"
#include "storage/duckdb/duckdb_gtid_utils.h"
#include "storage/duckdb/duckdb_repl_state.h"

#define DUCKDB_ADAPTER_VERBOSE(...)                                          \
  do {                                                                       \
    if (duckdb_se::DuckdbBinlogApplyVerbose())                               \
      sql_print_information(__VA_ARGS__);                                    \
  } while (0)

namespace duckdb_se {
namespace {

duckdb::Appender *GetOrCreateAppender(ApplyTxn &txn,
                                      const std::string &table_name) {
  if (!txn.conn) return nullptr;
  auto &slot = txn.appenders[table_name];
  if (!slot) {
    slot = std::make_unique<duckdb::Appender>(*txn.conn, table_name);
  }
  return slot.get();
}

// Generate a DuckDB-compatible timestamp literal for the current time.
// Uses C++ chrono to avoid reliance on DuckDB's now()/CURRENT_TIMESTAMP
// functions which may require core_functions extension.
std::string CurrentTimestampLiteral() {
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

bool IsValidUtf8(const std::string &value) {
  const unsigned char *data =
      reinterpret_cast<const unsigned char *>(value.data());
  size_t i = 0;
  const size_t len = value.size();
  while (i < len) {
    const unsigned char c = data[i];
    if (c <= 0x7F) {
      ++i;
      continue;
    }
    if (c >= 0xC2 && c <= 0xDF) {
      if (i + 1 >= len) return false;
      if ((data[i + 1] & 0xC0) != 0x80) return false;
      i += 2;
      continue;
    }
    if (c == 0xE0) {
      if (i + 2 >= len) return false;
      if (data[i + 1] < 0xA0 || data[i + 1] > 0xBF) return false;
      if ((data[i + 2] & 0xC0) != 0x80) return false;
      i += 3;
      continue;
    }
    if (c >= 0xE1 && c <= 0xEC) {
      if (i + 2 >= len) return false;
      if ((data[i + 1] & 0xC0) != 0x80) return false;
      if ((data[i + 2] & 0xC0) != 0x80) return false;
      i += 3;
      continue;
    }
    if (c == 0xED) {
      if (i + 2 >= len) return false;
      if (data[i + 1] < 0x80 || data[i + 1] > 0x9F) return false;
      if ((data[i + 2] & 0xC0) != 0x80) return false;
      i += 3;
      continue;
    }
    if (c >= 0xEE && c <= 0xEF) {
      if (i + 2 >= len) return false;
      if ((data[i + 1] & 0xC0) != 0x80) return false;
      if ((data[i + 2] & 0xC0) != 0x80) return false;
      i += 3;
      continue;
    }
    if (c == 0xF0) {
      if (i + 3 >= len) return false;
      if (data[i + 1] < 0x90 || data[i + 1] > 0xBF) return false;
      if ((data[i + 2] & 0xC0) != 0x80) return false;
      if ((data[i + 3] & 0xC0) != 0x80) return false;
      i += 4;
      continue;
    }
    if (c >= 0xF1 && c <= 0xF3) {
      if (i + 3 >= len) return false;
      if ((data[i + 1] & 0xC0) != 0x80) return false;
      if ((data[i + 2] & 0xC0) != 0x80) return false;
      if ((data[i + 3] & 0xC0) != 0x80) return false;
      i += 4;
      continue;
    }
    if (c == 0xF4) {
      if (i + 3 >= len) return false;
      if (data[i + 1] < 0x80 || data[i + 1] > 0x8F) return false;
      if ((data[i + 2] & 0xC0) != 0x80) return false;
      if ((data[i + 3] & 0xC0) != 0x80) return false;
      i += 4;
      continue;
    }
    return false;
  }
  return true;
}

std::string Latin1ToUtf8(const std::string &value) {
  std::string out;
  out.reserve(value.size() * 2);
  for (unsigned char ch : value) {
    if (ch < 0x80) {
      out.push_back(static_cast<char>(ch));
    } else {
      out.push_back(static_cast<char>(0xC0 | (ch >> 6)));
      out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    }
  }
  return out;
}

// Sanitize invalid UTF-8 sequences by replacing them with U+FFFD (replacement
// character). This ensures the output is always valid UTF-8.
std::string SanitizeUtf8(const std::string &value) {
  std::string out;
  out.reserve(value.size());
  const unsigned char *data =
      reinterpret_cast<const unsigned char *>(value.data());
  size_t i = 0;
  const size_t len = value.size();
  while (i < len) {
    const unsigned char c = data[i];
    // Check for valid UTF-8 byte sequences
    if (c <= 0x7F) {
      // ASCII character
      out.push_back(static_cast<char>(c));
      ++i;
    } else if (c >= 0xC2 && c <= 0xDF) {
      // 2-byte sequence
      if (i + 1 < len && (data[i + 1] & 0xC0) == 0x80) {
        out.push_back(static_cast<char>(c));
        out.push_back(static_cast<char>(data[i + 1]));
        i += 2;
      } else {
        // Invalid - replace with U+FFFD
        out.append("\xEF\xBF\xBD");
        ++i;
      }
    } else if (c == 0xE0) {
      // 3-byte sequence starting with E0
      if (i + 2 < len && data[i + 1] >= 0xA0 && data[i + 1] <= 0xBF &&
          (data[i + 2] & 0xC0) == 0x80) {
        out.push_back(static_cast<char>(c));
        out.push_back(static_cast<char>(data[i + 1]));
        out.push_back(static_cast<char>(data[i + 2]));
        i += 3;
      } else {
        out.append("\xEF\xBF\xBD");
        ++i;
      }
    } else if (c >= 0xE1 && c <= 0xEC) {
      // 3-byte sequence
      if (i + 2 < len && (data[i + 1] & 0xC0) == 0x80 &&
          (data[i + 2] & 0xC0) == 0x80) {
        out.push_back(static_cast<char>(c));
        out.push_back(static_cast<char>(data[i + 1]));
        out.push_back(static_cast<char>(data[i + 2]));
        i += 3;
      } else {
        out.append("\xEF\xBF\xBD");
        ++i;
      }
    } else if (c == 0xED) {
      // 3-byte sequence starting with ED (avoid surrogates)
      if (i + 2 < len && data[i + 1] >= 0x80 && data[i + 1] <= 0x9F &&
          (data[i + 2] & 0xC0) == 0x80) {
        out.push_back(static_cast<char>(c));
        out.push_back(static_cast<char>(data[i + 1]));
        out.push_back(static_cast<char>(data[i + 2]));
        i += 3;
      } else {
        out.append("\xEF\xBF\xBD");
        ++i;
      }
    } else if (c >= 0xEE && c <= 0xEF) {
      // 3-byte sequence
      if (i + 2 < len && (data[i + 1] & 0xC0) == 0x80 &&
          (data[i + 2] & 0xC0) == 0x80) {
        out.push_back(static_cast<char>(c));
        out.push_back(static_cast<char>(data[i + 1]));
        out.push_back(static_cast<char>(data[i + 2]));
        i += 3;
      } else {
        out.append("\xEF\xBF\xBD");
        ++i;
      }
    } else if (c == 0xF0) {
      // 4-byte sequence starting with F0
      if (i + 3 < len && data[i + 1] >= 0x90 && data[i + 1] <= 0xBF &&
          (data[i + 2] & 0xC0) == 0x80 && (data[i + 3] & 0xC0) == 0x80) {
        out.push_back(static_cast<char>(c));
        out.push_back(static_cast<char>(data[i + 1]));
        out.push_back(static_cast<char>(data[i + 2]));
        out.push_back(static_cast<char>(data[i + 3]));
        i += 4;
      } else {
        out.append("\xEF\xBF\xBD");
        ++i;
      }
    } else if (c >= 0xF1 && c <= 0xF3) {
      // 4-byte sequence
      if (i + 3 < len && (data[i + 1] & 0xC0) == 0x80 &&
          (data[i + 2] & 0xC0) == 0x80 && (data[i + 3] & 0xC0) == 0x80) {
        out.push_back(static_cast<char>(c));
        out.push_back(static_cast<char>(data[i + 1]));
        out.push_back(static_cast<char>(data[i + 2]));
        out.push_back(static_cast<char>(data[i + 3]));
        i += 4;
      } else {
        out.append("\xEF\xBF\xBD");
        ++i;
      }
    } else if (c == 0xF4) {
      // 4-byte sequence starting with F4
      if (i + 3 < len && data[i + 1] >= 0x80 && data[i + 1] <= 0x8F &&
          (data[i + 2] & 0xC0) == 0x80 && (data[i + 3] & 0xC0) == 0x80) {
        out.push_back(static_cast<char>(c));
        out.push_back(static_cast<char>(data[i + 1]));
        out.push_back(static_cast<char>(data[i + 2]));
        out.push_back(static_cast<char>(data[i + 3]));
        i += 4;
      } else {
        out.append("\xEF\xBF\xBD");
        ++i;
      }
    } else {
      // Invalid leading byte - replace with U+FFFD
      out.append("\xEF\xBF\xBD");
      ++i;
    }
  }
  return out;
}

Status AppendCellValue(duckdb::Appender &appender, const Cell &cell) {
  if (cell.is_null) {
    appender.Append(duckdb::Value());
    return Status::Ok();
  }
  if (cell.is_blob) {
    // For BLOB columns, use Value::BLOB_RAW to pass raw bytes without
    // interpreting escape sequences. Value::BLOB(string) expects hex-escaped
    // non-ASCII bytes (e.g. \xAA), but we have raw binary data.
    try {
      appender.Append(duckdb::Value::BLOB_RAW(cell.value));
    } catch (const std::exception &ex) {
      sql_print_warning("DuckDB AppendCellValue: BLOB_RAW append failed: %s", ex.what());
      return Status::Error(StatusCode::kDuckDBError,
                           std::string("BLOB encoding error: ") + ex.what());
    }
    return Status::Ok();
  }
  // For non-blob values, ensure the string is valid UTF-8.
  // If not valid, try Latin1→UTF-8 conversion first.
  // If DuckDB still rejects it, sanitize by replacing invalid sequences
  // with U+FFFD (replacement character).
  const std::string *value_ptr = &cell.value;
  std::string converted;
  if (!IsValidUtf8(cell.value)) {
    converted = Latin1ToUtf8(cell.value);
    value_ptr = &converted;
  }
  try {
    appender.Append(value_ptr->c_str(),
                    static_cast<uint32_t>(value_ptr->size()));
  } catch (const std::exception &ex) {
    // DuckDB's internal UTF-8 validation may be stricter than ours.
    // Try sanitizing the string by replacing invalid sequences.
    std::string sanitized = SanitizeUtf8(cell.value);
    try {
      appender.Append(sanitized.c_str(),
                      static_cast<uint32_t>(sanitized.size()));
    } catch (const std::exception &ex2) {
      // If sanitizing still fails, return an error instead of silently
      // corrupting data by using BLOB for a VARCHAR column.
      return Status::Error(StatusCode::kDuckDBError,
                           std::string("UTF-8 encoding error: ") + ex2.what());
    }
  }
  return Status::Ok();
}

bool IsMissingTableError(const std::string &error, const std::string &table) {
  return error.find("does not exist") != std::string::npos &&
         error.find(table) != std::string::npos;
}

constexpr const char *kReplChannel = "default";

Status ReadReplStateRow(duckdb::Connection &conn, std::string *snapshot,
                        std::string *applied, std::string *last_commit_ts,
                        bool *found) {
  if (snapshot) snapshot->clear();
  if (applied) applied->clear();
  if (last_commit_ts) last_commit_ts->clear();
  if (found) *found = false;
  auto result = conn.Query(
      "SELECT snapshot_gtid_set, applied_gtid_set, last_commit_ts "
      "FROM __repl_state WHERE channel = '" +
      std::string(kReplChannel) + "' LIMIT 1");
  if (result->HasError()) {
    return Status::Error(StatusCode::kDuckDBError, result->GetError());
  }
  auto chunk = result->Fetch();
  if (!chunk || chunk->size() == 0) {
    return Status::Ok();
  }
  if (found) *found = true;
  auto snap_val = chunk->GetValue(0, 0);
  if (snapshot && !snap_val.IsNull()) {
    *snapshot = snap_val.ToString();
  }
  auto applied_val = chunk->GetValue(1, 0);
  if (applied && !applied_val.IsNull()) {
    *applied = applied_val.ToString();
  }
  auto ts_val = chunk->GetValue(2, 0);
  if (last_commit_ts && !ts_val.IsNull()) {
    *last_commit_ts = ts_val.ToString();
  }
  return Status::Ok();
}

Status LoadWatermarkGtidSet(duckdb::Connection &conn, std::string *out_set) {
  if (!out_set) {
    return Status::Error(StatusCode::kInvalid, "GTID set output is null");
  }
  out_set->clear();
  auto result =
      conn.Query("SELECT gtid, commit_ts FROM __repl_watermark ORDER BY commit_ts");
  if (result->HasError()) {
    if (IsMissingTableError(result->GetError(), "__repl_watermark")) {
      return Status::Ok();
    }
    return Status::Error(StatusCode::kDuckDBError, result->GetError());
  }
  std::vector<std::string> gtids;
  while (true) {
    auto chunk = result->Fetch();
    if (!chunk || chunk->size() == 0) break;
    for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
      auto gtid_val = chunk->GetValue(0, row);
      if (!gtid_val.IsNull()) {
        gtids.push_back(gtid_val.ToString());
      }
    }
  }
  if (gtids.empty()) {
    return Status::Ok();
  }
  std::string error;
  if (!BuildGtidSetFromList(gtids, out_set, &error)) {
    return Status::Error(StatusCode::kInvalid,
                         error.empty() ? "Failed to build GTID set"
                                       : error);
  }
  return Status::Ok();
}

Status PersistReplStateFromWatermark(duckdb::Connection &conn,
                                     const std::string &applied_set) {
  if (applied_set.empty()) return Status::Ok();
  bool found = false;
  Status st = ReadReplStateRow(conn, nullptr, nullptr, nullptr, &found);
  if (!st.ok()) return st;
  if (found) return Status::Ok();
  const std::string ts_sql = CurrentTimestampLiteral();
  const std::string sql =
      "INSERT INTO __repl_state (channel, snapshot_gtid_set, "
      "applied_gtid_set, last_commit_ts) VALUES ('" +
      std::string(kReplChannel) + "', NULL, '" + EscapeReplStateLiteral(applied_set) +
      "', " + ts_sql + ")";
  auto result = conn.Query(sql);
  if (result->HasError()) {
    return Status::Error(StatusCode::kDuckDBError, result->GetError());
  }
  return Status::Ok();
}

bool StartsWithCI(const std::string &input, const char *prefix) {
  size_t i = 0;
  while (i < input.size() && std::isspace(static_cast<unsigned char>(input[i]))) {
    ++i;
  }
  size_t j = 0;
  while (prefix[j] != '\0') {
    if (i >= input.size()) return false;
    const char lhs = static_cast<char>(
        std::toupper(static_cast<unsigned char>(input[i++])));
    const char rhs = static_cast<char>(
        std::toupper(static_cast<unsigned char>(prefix[j++])));
    if (lhs != rhs) return false;
  }
  return true;
}

enum class TypeSeverity {
  kOk,
  kWarning,
  kLossy
};

struct TypeMapping {
  std::string type;
  TypeSeverity severity{TypeSeverity::kOk};
  std::string reason;
};

std::string Trim(const std::string &input) {
  size_t start = 0;
  while (start < input.size() &&
         std::isspace(static_cast<unsigned char>(input[start]))) {
    ++start;
  }
  size_t end = input.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(input[end - 1]))) {
    --end;
  }
  return input.substr(start, end - start);
}

std::string ToUpperASCII(std::string input) {
  for (char &ch : input) {
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }
  return input;
}

bool ParsePrecisionScale(const std::string &input, uint *precision,
                         uint *scale) {
  if (!precision || !scale) return false;
  *precision = 0;
  *scale = 0;

  size_t pos = 0;
  while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) {
    ++pos;
  }
  if (pos >= input.size() || !std::isdigit(static_cast<unsigned char>(input[pos]))) {
    return false;
  }
  while (pos < input.size() &&
         std::isdigit(static_cast<unsigned char>(input[pos]))) {
    *precision = *precision * 10 + (input[pos] - '0');
    ++pos;
  }
  while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) {
    ++pos;
  }
  if (pos >= input.size() || input[pos] != ',') {
    return true;
  }
  ++pos;
  while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) {
    ++pos;
  }
  if (pos >= input.size() || !std::isdigit(static_cast<unsigned char>(input[pos]))) {
    return false;
  }
  while (pos < input.size() &&
         std::isdigit(static_cast<unsigned char>(input[pos]))) {
    *scale = *scale * 10 + (input[pos] - '0');
    ++pos;
  }
  return true;
}

TypeMapping MapMySQLTypeString(const std::string &mysql_type) {
  TypeMapping mapping;
  const std::string trimmed = Trim(mysql_type);
  if (trimmed.empty()) {
    mapping.type = "VARCHAR";
    mapping.severity = TypeSeverity::kLossy;
    mapping.reason = "empty type";
    return mapping;
  }

  std::string upper = ToUpperASCII(trimmed);
  const bool unsigned_flag = upper.find("UNSIGNED") != std::string::npos;
  const size_t base_end = upper.find_first_of(" (");
  const std::string base =
      base_end == std::string::npos ? upper : upper.substr(0, base_end);
  uint precision = 0;
  uint scale = 0;
  const size_t paren = upper.find('(');
  if (paren != std::string::npos) {
    const size_t end = upper.find(')', paren);
    if (end != std::string::npos) {
      (void)ParsePrecisionScale(upper.substr(paren + 1, end - paren - 1),
                                &precision, &scale);
    }
  }

  if (base == "TINYINT") {
    mapping.type = unsigned_flag ? "UTINYINT" : "TINYINT";
    return mapping;
  }
  if (base == "SMALLINT") {
    mapping.type = unsigned_flag ? "USMALLINT" : "SMALLINT";
    return mapping;
  }
  if (base == "MEDIUMINT" || base == "INT" || base == "INTEGER") {
    mapping.type = unsigned_flag ? "UINTEGER" : "INTEGER";
    return mapping;
  }
  if (base == "BIGINT") {
    mapping.type = unsigned_flag ? "UBIGINT" : "BIGINT";
    return mapping;
  }
  if (base == "FLOAT" || base == "REAL") {
    mapping.type = "FLOAT";
    return mapping;
  }
  if (base == "DOUBLE" || base == "DOUBLE PRECISION") {
    mapping.type = "DOUBLE";
    return mapping;
  }
  if (base == "DECIMAL" || base == "NUMERIC") {
    if (precision == 0) precision = 10;
    if (precision > 38) {
      mapping.type = "VARCHAR";
      mapping.severity = TypeSeverity::kLossy;
      mapping.reason = "DECIMAL precision > 38 stored as VARCHAR";
      return mapping;
    }
    mapping.type = "DECIMAL(" + std::to_string(precision) + "," +
                   std::to_string(scale) + ")";
    return mapping;
  }
  if (base == "DATE") {
    mapping.type = "DATE";
    return mapping;
  }
  if (base == "TIME") {
    mapping.type = "TIME";
    return mapping;
  }
  if (base == "DATETIME") {
    mapping.type = "TIMESTAMP";
    return mapping;
  }
  if (base == "TIMESTAMP") {
    mapping.type = "TIMESTAMP";
    mapping.severity = TypeSeverity::kWarning;
    mapping.reason = "TIMESTAMP timezone semantics may differ";
    return mapping;
  }
  if (base == "YEAR") {
    mapping.type = "SMALLINT";
    mapping.severity = TypeSeverity::kWarning;
    mapping.reason = "YEAR stored as SMALLINT";
    return mapping;
  }
  if (base == "JSON") {
    mapping.type = "VARCHAR";
    mapping.severity = TypeSeverity::kLossy;
    mapping.reason = "JSON stored as VARCHAR";
    return mapping;
  }
  if (base == "ENUM" || base == "SET") {
    mapping.type = "VARCHAR";
    mapping.severity = TypeSeverity::kLossy;
    mapping.reason = "ENUM/SET stored as VARCHAR";
    return mapping;
  }
  if (base == "BIT") {
    mapping.type = "BLOB";
    mapping.severity = TypeSeverity::kLossy;
    mapping.reason = "BIT stored as BLOB";
    return mapping;
  }
  if (base == "BLOB" || base == "TINYBLOB" || base == "MEDIUMBLOB" ||
      base == "LONGBLOB") {
    mapping.type = "BLOB";
    return mapping;
  }
  if (base == "TEXT" || base == "TINYTEXT" || base == "MEDIUMTEXT" ||
      base == "LONGTEXT") {
    mapping.type = "VARCHAR";
    return mapping;
  }
  if (base == "CHAR" || base == "VARCHAR") {
    mapping.type = "VARCHAR";
    return mapping;
  }
  if (base == "BOOLEAN" || base == "BOOL") {
    mapping.type = "BOOLEAN";
    return mapping;
  }
  if (base == "BLOB" || base == "VARCHAR" || base == "INTEGER" ||
      base == "BIGINT" || base == "DOUBLE" || base == "FLOAT" ||
      base == "TIMESTAMP") {
    mapping.type = base;
    return mapping;
  }

  mapping.type = "VARCHAR";
  mapping.severity = TypeSeverity::kLossy;
  mapping.reason = "unrecognized type";
  return mapping;
}

bool IsBoundary(char ch) {
  return !std::isalnum(static_cast<unsigned char>(ch)) && ch != '_';
}

bool ContainsToken(const std::string &upper, const std::string &token) {
  size_t pos = upper.find(token);
  while (pos != std::string::npos) {
    const char before = pos == 0 ? ' ' : upper[pos - 1];
    const size_t end = pos + token.size();
    const char after = end >= upper.size() ? ' ' : upper[end];
    if (IsBoundary(before) && IsBoundary(after)) return true;
    pos = upper.find(token, pos + 1);
  }
  return false;
}

std::string UnsupportedDDLReason(const std::string &sql,
                                 duckdb_se::DDLChange::Type type) {
  if (sql.empty()) return "";
  std::string upper = ToUpperASCII(sql);

  if (ContainsToken(upper, "CHARACTER SET") || ContainsToken(upper, "CHARSET") ||
      ContainsToken(upper, "COLLATE")) {
    return "character set/collation";
  }
  if (ContainsToken(upper, "GENERATED") || ContainsToken(upper, "VIRTUAL") ||
      ContainsToken(upper, "STORED")) {
    return "generated columns";
  }
  if (ContainsToken(upper, "PARTITION")) {
    return "partitioning";
  }
  if (ContainsToken(upper, "FOREIGN KEY") || ContainsToken(upper, "REFERENCES")) {
    return "foreign keys";
  }
  if (ContainsToken(upper, "FULLTEXT") || ContainsToken(upper, "SPATIAL") ||
      ContainsToken(upper, "INDEX") || ContainsToken(upper, "UNIQUE") ||
      (ContainsToken(upper, "KEY") && !ContainsToken(upper, "PRIMARY KEY") &&
       !ContainsToken(upper, "FOREIGN KEY"))) {
    return "secondary indexes";
  }
  if (type == duckdb_se::DDLChange::Type::kAlter) {
    if (ContainsToken(upper, "FIRST") || ContainsToken(upper, "AFTER")) {
      return "column reordering";
    }
  }

  return "";
}

}  // namespace

TypeCheck CheckMySQLTypeString(const std::string &mysql_type) {
  const auto mapping = MapMySQLTypeString(mysql_type);
  TypeCheck out;
  out.type = mapping.type;
  out.lossy = mapping.severity == TypeSeverity::kLossy;
  out.reason = mapping.reason;
  return out;
}

Status DuckDBAdapter::Init(std::string db_path, DuckDBConfig cfg) {
  if (initialized_) {
    return Status::Error(StatusCode::kAlreadyInitialized,
                         "DuckDBAdapter already initialized");
  }

  try {
    duckdb::DBConfig config(cfg.read_only);
    db_ = std::make_unique<duckdb::DuckDB>(db_path, &config);
    conn_ = std::make_unique<duckdb::Connection>(*db_);
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  db_path_ = std::move(db_path);
  cfg_ = cfg;
  initialized_ = true;
  return Status::Ok();
}

void DuckDBAdapter::Shutdown() {
  conn_.reset();
  db_.reset();
  initialized_ = false;
  db_path_.clear();
}

Status DuckDBAdapter::EnsureInitialized() const {
  if (!initialized_) {
    return Status::Error(StatusCode::kNotInitialized,
                         "DuckDBAdapter not initialized");
  }
  return Status::Ok();
}

Status DuckDBAdapter::ExecuteDDLOn(duckdb::Connection &conn,
                                   const std::string &sql) {
  if (sql.empty()) {
    return Status::Error(StatusCode::kInvalid, "Empty DDL statement");
  }
  try {
    auto result = conn.Query(sql);
    if (result->HasError()) {
      return Status::Error(StatusCode::kDuckDBError, result->GetError());
    }
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }
  return Status::Ok();
}

std::string DuckDBAdapter::DeltaTableName(const TableId &table) const {
  return "__delta_" + table.table;
}

std::string DuckDBAdapter::InsertDeltaTableName(const TableId &table) const {
  return "__delta_insert_" + table.table;
}

Status DuckDBAdapter::EnsureDeltaTable(duckdb::Connection &conn,
                                       TableId table) {
  if (table.table.empty()) {
    return Status::Error(StatusCode::kInvalid, "Missing table name");
  }

  std::vector<ColumnInfo> columns;
  Status st = GetTableInfoOn(conn, table, &columns);
  if (!st.ok()) return st;
  if (columns.empty()) {
    return Status::Error(StatusCode::kInvalid, "No columns for delta table");
  }

  std::string sql = "CREATE TABLE IF NOT EXISTS ";
  TableId delta{table.schema, DeltaTableName(table)};
  sql += QualifiedName(delta);
  sql += " (";
  for (size_t i = 0; i < columns.size(); ++i) {
    const std::string type =
        columns[i].type.empty() ? "VARCHAR" : columns[i].type;
    sql += QuoteIdent(columns[i].name) + " " + type;
    if (i + 1 < columns.size()) sql += ", ";
  }
  sql += ")";

  return ExecuteDDLOn(conn, sql);
}

Status DuckDBAdapter::EnsureInsertDeltaTable(duckdb::Connection &conn,
                                             TableId table) {
  if (table.table.empty()) {
    return Status::Error(StatusCode::kInvalid, "Missing table name");
  }

  std::vector<ColumnInfo> columns;
  Status st = GetTableInfoOn(conn, table, &columns);
  if (!st.ok()) return st;
  if (columns.empty()) {
    return Status::Error(StatusCode::kInvalid, "No columns for insert delta");
  }

  std::string sql = "CREATE TABLE IF NOT EXISTS ";
  TableId delta{table.schema, InsertDeltaTableName(table)};
  sql += QualifiedName(delta);
  sql += " (";
  for (size_t i = 0; i < columns.size(); ++i) {
    const std::string type =
        columns[i].type.empty() ? "VARCHAR" : columns[i].type;
    sql += QuoteIdent(columns[i].name) + " " + type;
    if (i + 1 < columns.size()) sql += ", ";
  }
  sql += ")";

  return ExecuteDDLOn(conn, sql);
}

Status DuckDBAdapter::ExecuteDDL(const std::string &sql) {
  auto st = EnsureInitialized();
  if (!st.ok()) return st;
  return ExecuteDDLOn(*conn_, sql);
}

std::string DuckDBAdapter::QuoteIdent(const std::string &name) const {
  std::string out;
  out.reserve(name.size() + 2);
  out.push_back('"');
  for (char ch : name) {
    if (ch == '"') out.push_back('"');
    out.push_back(ch);
  }
  out.push_back('"');
  return out;
}

std::string DuckDBAdapter::QualifiedName(const TableId &table) const {
  // Always use just table name (main schema) since:
  // 1. MySQL ha_duckdb creates tables without schema prefix
  // 2. Schema separation is handled at DuckDB file level (<schema>.duckdb)
  // 3. Binlog applier creates per-schema DuckDB instances
  return QuoteIdent(table.table);
}

std::string DuckDBAdapter::EscapeLiteral(const std::string &value) const {
  std::string out;
  out.reserve(value.size() + 4);
  for (char ch : value) {
    if (ch == '\'') out.push_back('\'');
    out.push_back(ch);
  }
  return out;
}

std::string DuckDBAdapter::NormalizeDDL(const std::string &sql) const {
  std::string out;
  out.reserve(sql.size());
  for (char ch : sql) {
    out.push_back(ch == '`' ? '"' : ch);
  }
  if (StartsWithCI(out, "CREATE TABLE")) {
    const auto pos = out.rfind(')');
    if (pos != std::string::npos) {
      out.resize(pos + 1);
    }
  }
  return out;
}

DDLChange::Type DuckDBAdapter::InferDDLType(const std::string &sql) const {
  if (StartsWithCI(sql, "CREATE TABLE")) return DDLChange::Type::kCreate;
  if (StartsWithCI(sql, "DROP TABLE")) return DDLChange::Type::kDrop;
  if (StartsWithCI(sql, "ALTER TABLE")) return DDLChange::Type::kAlter;
  if (StartsWithCI(sql, "RENAME TABLE")) return DDLChange::Type::kRename;
  if (StartsWithCI(sql, "TRUNCATE TABLE")) return DDLChange::Type::kTruncate;
  return DDLChange::Type::kUnknown;
}

Status DuckDBAdapter::CreateTable(MySQLTableDef def) {
  auto st = EnsureInitialized();
  if (!st.ok()) return st;
  return CreateTableOn(*conn_, std::move(def));
}

Status DuckDBAdapter::CreateTableOn(duckdb::Connection &conn,
                                    MySQLTableDef def) {
  std::string sql = def.ddl_sql;
  if (sql.empty()) {
    TableId id{def.schema, def.name};
    std::ostringstream ddl;
    ddl << "CREATE TABLE " << QualifiedName(id) << " (";
    for (size_t i = 0; i < def.columns.size(); ++i) {
      const auto &col = def.columns[i];
      const auto mapping = MapMySQLTypeString(col.type);
      if (mapping.severity == TypeSeverity::kLossy && !def.allow_lossy) {
        return Status::Error(
            StatusCode::kInvalid,
            "Unsupported or lossy column type for " + col.name + ": " +
                col.type + " (" + mapping.reason + ")");
      }
      ddl << QuoteIdent(col.name) << " " << mapping.type;
      if (col.not_null) ddl << " NOT NULL";
      if (i + 1 < def.columns.size()) ddl << ", ";
    }
    if (!def.primary_key.empty()) {
      ddl << ", PRIMARY KEY (";
      for (size_t i = 0; i < def.primary_key.size(); ++i) {
        ddl << QuoteIdent(def.primary_key[i]);
        if (i + 1 < def.primary_key.size()) ddl << ", ";
      }
      ddl << ")";
    }
    ddl << ")";
    sql = ddl.str();
  }

  DUCKDB_ADAPTER_VERBOSE("DuckDB create table DDL: %s", sql.c_str());
  try {
    DUCKDB_ADAPTER_VERBOSE("DuckDB create table: executing query...");
    auto result = conn.Query(sql);
    DUCKDB_ADAPTER_VERBOSE(
        "DuckDB create table: query returned, checking error...");
    if (result->HasError()) {
      sql_print_warning("DuckDB create table: query error: %s",
                        result->GetError().c_str());
      return Status::Error(StatusCode::kDuckDBError, result->GetError());
    }
    DUCKDB_ADAPTER_VERBOSE("DuckDB create table: success");
  } catch (const std::exception &ex) {
    sql_print_warning("DuckDB create table: exception: %s", ex.what());
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return Status::Ok();
}

Status DuckDBAdapter::AlterTable(DDLChange change) {
  change.type = DDLChange::Type::kAlter;
  return ApplyDDL(std::move(change));
}

Status DuckDBAdapter::DropTable(std::string schema, std::string table) {
  auto st = EnsureInitialized();
  if (!st.ok()) return st;
  return DropTableOn(*conn_, std::move(schema), std::move(table));
}

Status DuckDBAdapter::DropTableOn(duckdb::Connection &conn, std::string schema,
                                  std::string table) {
  TableId id{std::move(schema), std::move(table)};
  const std::string sql = "DROP TABLE IF EXISTS " + QualifiedName(id);
  return ExecuteDDLOn(conn, sql);
}

Status DuckDBAdapter::RenameTable(TableId from, TableId to) {
  auto st = EnsureInitialized();
  if (!st.ok()) return st;
  return RenameTableOn(*conn_, std::move(from), std::move(to));
}

Status DuckDBAdapter::TruncateTable(TableId table) {
  auto st = EnsureInitialized();
  if (!st.ok()) return st;
  return TruncateTableOn(*conn_, std::move(table));
}

Status DuckDBAdapter::RenameTableOn(duckdb::Connection &conn, TableId from,
                                    TableId to) {
  auto st = EnsureInitialized();
  if (!st.ok()) return st;
  if (from.table.empty() || to.table.empty()) {
    return Status::Error(StatusCode::kInvalid, "Missing table name for rename");
  }
  if (!from.schema.empty() && !to.schema.empty() && from.schema != to.schema) {
    return Status::Error(StatusCode::kInvalid,
                         "Cross-schema rename not supported");
  }
  if (from.schema.empty()) {
    from.schema = to.schema;
  }
  const std::string sql = "ALTER TABLE " + QualifiedName(from) +
                          " RENAME TO " + QuoteIdent(to.table);
  return ExecuteDDLOn(conn, sql);
}

Status DuckDBAdapter::TruncateTableOn(duckdb::Connection &conn, TableId table) {
  auto st = EnsureInitialized();
  if (!st.ok()) return st;
  if (table.table.empty()) {
    return Status::Error(StatusCode::kInvalid, "Missing table name to truncate");
  }
  const std::string sql = "DELETE FROM " + QualifiedName(table);
  return ExecuteDDLOn(conn, sql);
}

Status DuckDBAdapter::GetTableColumns(TableId table,
                                      std::vector<std::string> *columns) {
  if (!columns) {
    return Status::Error(StatusCode::kInvalid, "Columns output is null");
  }
  columns->clear();

  std::vector<ColumnInfo> info;
  auto st = GetTableInfo(table, &info);
  if (!st.ok()) return st;
  for (const auto &col : info) {
    columns->push_back(col.name);
  }
  return Status::Ok();
}

Status DuckDBAdapter::GetTableInfo(TableId table,
                                   std::vector<ColumnInfo> *columns) {
  auto st = EnsureInitialized();
  if (!st.ok()) return st;
  return GetTableInfoOn(*conn_, std::move(table), columns);
}

Status DuckDBAdapter::GetTableInfoOn(duckdb::Connection &conn, TableId table,
                                     std::vector<ColumnInfo> *columns) {
  if (!columns) {
    return Status::Error(StatusCode::kInvalid, "Table info output is null");
  }
  columns->clear();
  if (table.table.empty()) {
    return Status::Error(StatusCode::kInvalid, "Missing table name");
  }

  auto st = EnsureInitialized();
  if (!st.ok()) return st;

  const std::string sql =
      "PRAGMA table_info('" + EscapeLiteral(table.table) + "')";

  try {
    auto result = conn.Query(sql);
    if (result->HasError()) {
      return Status::Error(StatusCode::kDuckDBError, result->GetError());
    }
    while (true) {
      auto chunk = result->Fetch();
      if (!chunk || chunk->size() == 0) {
        break;
      }
      for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
        auto name_val = chunk->GetValue(1, row);
        if (name_val.IsNull()) continue;
        ColumnInfo info;
        info.name = name_val.ToString();
        auto type_val = chunk->GetValue(2, row);
        if (!type_val.IsNull()) {
          info.type = type_val.ToString();
        }
        auto pk_val = chunk->GetValue(5, row);
        if (!pk_val.IsNull()) {
          try {
            info.pk = pk_val.GetValue<int32_t>();
          } catch (...) {
            const std::string pk_str = pk_val.ToString();
            if (!pk_str.empty()) {
              info.pk = std::stoi(pk_str);
            }
          }
        }
        columns->push_back(std::move(info));
      }
    }
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return Status::Ok();
}

Status DuckDBAdapter::GetTableColumnsOn(duckdb::Connection &conn, TableId table,
                                        std::vector<std::string> *columns) {
  if (!columns) {
    return Status::Error(StatusCode::kInvalid, "Columns output is null");
  }
  columns->clear();

  std::vector<ColumnInfo> info;
  auto st = GetTableInfoOn(conn, std::move(table), &info);
  if (!st.ok()) return st;
  for (const auto &col : info) {
    columns->push_back(col.name);
  }
  return Status::Ok();
}

Status DuckDBAdapter::GetPrimaryKeyColumns(
    TableId table, std::vector<std::string> *columns) {
  if (!columns) {
    return Status::Error(StatusCode::kInvalid, "PK output is null");
  }
  columns->clear();
  std::vector<ColumnInfo> info;
  Status st = GetTableInfo(table, &info);
  if (!st.ok()) return st;

  std::vector<std::pair<int, std::string>> pk;
  pk.reserve(info.size());
  for (const auto &col : info) {
    if (col.pk > 0) {
      pk.emplace_back(col.pk, col.name);
    }
  }
  if (pk.empty()) return Status::Ok();
  std::sort(pk.begin(), pk.end(),
            [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });
  for (const auto &entry : pk) {
    columns->push_back(entry.second);
  }
  return Status::Ok();
}

Status DuckDBAdapter::GetPrimaryKeyColumnsOn(
    duckdb::Connection &conn, TableId table,
    std::vector<std::string> *columns) {
  if (!columns) {
    return Status::Error(StatusCode::kInvalid, "PK output is null");
  }
  columns->clear();
  std::vector<ColumnInfo> info;
  Status st = GetTableInfoOn(conn, std::move(table), &info);
  if (!st.ok()) return st;

  std::vector<std::pair<int, std::string>> pk;
  pk.reserve(info.size());
  for (const auto &col : info) {
    if (col.pk > 0) {
      pk.emplace_back(col.pk, col.name);
    }
  }
  if (pk.empty()) return Status::Ok();
  std::sort(pk.begin(), pk.end(),
            [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });
  for (const auto &entry : pk) {
    columns->push_back(entry.second);
  }
  return Status::Ok();
}

Status DuckDBAdapter::GetSchemaVersion(int64_t *version, bool *found) {
  if (!version || !found) {
    return Status::Error(StatusCode::kInvalid, "Schema version output is null");
  }
  *version = 0;
  *found = false;

  auto st = EnsureInitialized();
  if (!st.ok()) return st;

  std::string error;
  if (!EnsureReplStateTable(*conn_, &error)) {
    return Status::Error(StatusCode::kDuckDBError,
                         error.empty() ? "Failed to ensure __repl_state"
                                       : error);
  }

  try {
    auto result = conn_->Query(
        "SELECT schema_version FROM __repl_state WHERE channel = '" +
        std::string(kReplChannel) + "' LIMIT 1");
    if (result->HasError()) {
      return Status::Error(StatusCode::kDuckDBError, result->GetError());
    }
    auto chunk = result->Fetch();
    if (!chunk || chunk->size() == 0) {
      return Status::Ok();
    }
    *found = true;
    auto val = chunk->GetValue(0, 0);
    if (val.IsNull()) {
      return Status::Ok();
    }
    const std::string value = val.ToString();
    if (value.empty()) {
      return Status::Ok();
    }
    try {
      *version = std::stoll(value);
    } catch (const std::exception &ex) {
      return Status::Error(StatusCode::kInvalid, ex.what());
    }
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return Status::Ok();
}

Status DuckDBAdapter::CopyTable(TableId source,
                                const MySQLTableDef &target_def) {
  auto st = EnsureInitialized();
  if (!st.ok()) return st;
  return CopyTableOn(*conn_, std::move(source), target_def, true);
}

Status DuckDBAdapter::CopyTableOn(duckdb::Connection &conn, TableId source,
                                  const MySQLTableDef &target_def,
                                  bool manage_txn) {
  auto st = EnsureInitialized();
  if (!st.ok()) return st;
  if (source.table.empty()) {
    return Status::Error(StatusCode::kInvalid, "Missing source table");
  }
  if (target_def.name.empty()) {
    return Status::Error(StatusCode::kInvalid, "Missing target table name");
  }

  std::string target_schema = target_def.schema.empty() ? source.schema
                                                        : target_def.schema;
  if (!target_schema.empty() && !source.schema.empty() &&
      target_schema != source.schema) {
    return Status::Error(StatusCode::kInvalid,
                         "Copy DDL across schemas not supported");
  }

  const auto suffix = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const std::string temp_name = target_def.name + "__duckdb_tmp_" + suffix;
  const std::string backup_name = source.table + "__duckdb_old_" + suffix;

  if (manage_txn) {
    st = ExecuteDDLOn(conn, "BEGIN TRANSACTION");
    if (!st.ok()) return st;
  }
  bool committed = false;

  auto rollback = [&]() {
    if (manage_txn && !committed) {
      ExecuteDDLOn(conn, "ROLLBACK");
    }
  };

  MySQLTableDef temp_def = target_def;
  temp_def.schema = target_schema;
  temp_def.name = temp_name;

  st = CreateTableOn(conn, std::move(temp_def));
  if (!st.ok()) {
    rollback();
    return st;
  }

  std::vector<std::string> old_cols;
  st = GetTableColumns(source, &old_cols);
  if (!st.ok()) {
    rollback();
    return st;
  }

  std::unordered_set<std::string> old_set(old_cols.begin(), old_cols.end());
  std::vector<std::string> copy_cols;
  if (!target_def.columns.empty()) {
    for (const auto &col : target_def.columns) {
      if (old_set.find(col.name) != old_set.end()) {
        copy_cols.push_back(col.name);
      }
    }
  } else {
    copy_cols = old_cols;
  }

  if (!copy_cols.empty()) {
    std::string cols_sql;
    for (size_t i = 0; i < copy_cols.size(); ++i) {
      cols_sql += QuoteIdent(copy_cols[i]);
      if (i + 1 < copy_cols.size()) cols_sql += ", ";
    }
    TableId temp_table{target_schema, temp_name};
    const std::string insert_sql =
        "INSERT INTO " + QualifiedName(temp_table) + " (" + cols_sql + ") " +
        "SELECT " + cols_sql + " FROM " + QualifiedName(source);
    st = ExecuteDDLOn(conn, insert_sql);
    if (!st.ok()) {
      rollback();
      return st;
    }
  }

  TableId backup{source.schema, backup_name};
  st = RenameTableOn(conn, source, backup);
  if (!st.ok()) {
    rollback();
    return st;
  }

  TableId temp_table{target_schema, temp_name};
  TableId final_table{target_schema, target_def.name};
  st = RenameTableOn(conn, temp_table, final_table);
  if (!st.ok()) {
    rollback();
    return st;
  }

  st = DropTableOn(conn, backup.schema, backup.table);
  if (!st.ok()) {
    rollback();
    return st;
  }

  if (manage_txn) {
    st = ExecuteDDLOn(conn, "COMMIT");
    if (!st.ok()) {
      rollback();
      return st;
    }
    committed = true;
  }
  return Status::Ok();
}

Status DuckDBAdapter::ApplyDDL(DDLChange change) {
  auto st = EnsureInitialized();
  if (!st.ok()) return st;
  return ApplyDDLOn(*conn_, std::move(change), true);
}

Status DuckDBAdapter::ApplyDDLInTxn(ApplyTxn &txn, DDLChange change) {
  if (!txn.active || !txn.conn) {
    return Status::Error(StatusCode::kInvalid, "Apply transaction not active");
  }
  return ApplyDDLOn(*txn.conn, std::move(change), false);
}

Status DuckDBAdapter::ApplyDDLOn(duckdb::Connection &conn, DDLChange change,
                                 bool manage_copy_txn) {
  auto st = EnsureInitialized();
  if (!st.ok()) return st;

  auto type = change.type;
  if (type == DDLChange::Type::kUnknown && !change.sql.empty()) {
    type = InferDDLType(change.sql);
  }
  const std::string unsupported_reason = UnsupportedDDLReason(change.sql, type);

  switch (type) {
    case DDLChange::Type::kCreate:
      if (!change.new_def.name.empty()) {
        return CreateTableOn(conn, std::move(change.new_def));
      }
      if (!unsupported_reason.empty()) {
        return Status::Error(StatusCode::kInvalid,
                             "Unsupported DuckDB DDL (" + unsupported_reason +
                                 "); use copy DDL fallback with new_def");
      }
      if (change.sql.empty()) {
        return Status::Error(StatusCode::kInvalid, "Missing CREATE TABLE SQL");
      }
      return ExecuteDDLOn(conn, NormalizeDDL(change.sql));
    case DDLChange::Type::kDrop:
      if (!change.table.table.empty()) {
        return DropTableOn(conn, change.table.schema, change.table.table);
      }
      if (change.sql.empty()) {
        return Status::Error(StatusCode::kInvalid, "Missing DROP TABLE SQL");
      }
      return ExecuteDDLOn(conn, NormalizeDDL(change.sql));
    case DDLChange::Type::kAlter:
      if (!unsupported_reason.empty()) {
        if (!change.new_def.name.empty()) {
          return CopyTableOn(conn, change.table, change.new_def,
                             manage_copy_txn);
        }
        return Status::Error(StatusCode::kInvalid,
                             "Unsupported DuckDB DDL (" + unsupported_reason +
                                 "); use copy DDL fallback with new_def");
      }
      if (change.copy_ddl) {
        return CopyTableOn(conn, change.table, change.new_def,
                           manage_copy_txn);
      }
      if (change.sql.empty()) {
        return Status::Error(StatusCode::kInvalid, "Missing ALTER TABLE SQL");
      }
      return ExecuteDDLOn(conn, NormalizeDDL(change.sql));
    case DDLChange::Type::kRename:
      if (!change.table.table.empty() && !change.new_table.table.empty()) {
        return RenameTableOn(conn, change.table, change.new_table);
      }
      if (change.sql.empty()) {
        return Status::Error(StatusCode::kInvalid, "Missing RENAME TABLE SQL");
      }
      return ExecuteDDLOn(conn, NormalizeDDL(change.sql));
    case DDLChange::Type::kTruncate:
      if (!change.table.table.empty()) {
        return TruncateTableOn(conn, change.table);
      }
      if (change.sql.empty()) {
        return Status::Error(StatusCode::kInvalid, "Missing TRUNCATE TABLE SQL");
      }
      return ExecuteDDLOn(conn, NormalizeDDL(change.sql));
    case DDLChange::Type::kUnknown:
    default:
      if (!unsupported_reason.empty()) {
        return Status::Error(StatusCode::kInvalid,
                             "Unsupported DuckDB DDL (" + unsupported_reason +
                                 "); use copy DDL fallback with new_def");
      }
      if (change.sql.empty()) {
        return Status::Error(StatusCode::kInvalid, "Missing DDL SQL");
      }
      return ExecuteDDLOn(conn, NormalizeDDL(change.sql));
  }
}

ApplyTxn DuckDBAdapter::BeginApplyTxn(Gtid gtid) {
  ApplyTxn txn;
  txn.gtid = std::move(gtid);

  if (!db_) {
    txn.status = Status::Error(StatusCode::kNotInitialized,
                               "DuckDBAdapter not initialized");
    return txn;
  }

  try {
    // Use existing database instance instead of creating a new one.
    // DuckDB instances are isolated and don't share catalog changes.
    txn.conn = std::make_unique<duckdb::Connection>(*db_);
    auto result = txn.conn->Query("BEGIN TRANSACTION");
    if (result->HasError()) {
      txn.status = Status::Error(StatusCode::kDuckDBError, result->GetError());
      return txn;
    }
    txn.active = true;
    txn.status = Status::Ok();
  } catch (const std::exception &ex) {
    txn.status = Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return txn;
}

Status DuckDBAdapter::AppendRows(ApplyTxn &txn, TableId table, RowBatch batch) {
  if (!txn.active || !txn.conn) {
    return Status::Error(StatusCode::kInvalid, "Apply transaction not active");
  }

  try {
    // Always use main schema; schema separation is at the DuckDB file level.
    if (table.table.empty()) {
      return Status::Error(StatusCode::kInvalid, "Missing table name");
    }
    DUCKDB_ADAPTER_VERBOSE(
        "DuckDB AppendRows: using appender for %s, rows=%zu",
        table.table.c_str(), batch.rows.size());
    duckdb::Appender *appender = GetOrCreateAppender(txn, table.table);
    if (!appender) {
      return Status::Error(StatusCode::kInvalid, "Appender init failed");
    }

    size_t row_idx = 0;
    for (const auto &row : batch.rows) {
      appender->BeginRow();
      size_t col_idx = 0;
      for (const auto &cell : row) {
        Status st = AppendCellValue(*appender, cell);
        if (!st.ok()) {
          sql_print_warning(
              "DuckDB AppendRows: AppendCellValue failed row=%zu col=%zu",
              row_idx, col_idx);
          return st;
        }
        ++col_idx;
      }
      appender->EndRow();
      ++row_idx;
    }
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return Status::Ok();
}

Status DuckDBAdapter::ApplyInsertDelta(ApplyTxn &txn, TableId table,
                                       RowBatch batch) {
  if (!txn.active || !txn.conn) {
    return Status::Error(StatusCode::kInvalid, "Apply transaction not active");
  }
  if (batch.rows.empty()) {
    return Status::Ok();
  }

  Status st = EnsureInsertDeltaTable(*txn.conn, table);
  if (!st.ok()) return st;

  TableId delta{table.schema, InsertDeltaTableName(table)};
  // Append into insert-delta table for idempotent merge.
  st = AppendRows(txn, delta, std::move(batch));
  if (!st.ok()) return st;

  std::vector<std::string> columns;
  st = GetTableColumnsOn(*txn.conn, table, &columns);
  if (!st.ok()) return st;
  if (columns.empty()) {
    return Status::Error(StatusCode::kInvalid, "No columns for insert merge");
  }

  std::vector<std::string> pk_columns;
  st = GetPrimaryKeyColumnsOn(*txn.conn, table, &pk_columns);
  if (!st.ok()) return st;

  const std::string target = QualifiedName(table);
  const std::string delta_name = QualifiedName(delta);

  std::ostringstream sql;
  sql << "INSERT INTO " << target << " (";
  for (size_t i = 0; i < columns.size(); ++i) {
    sql << QuoteIdent(columns[i]);
    if (i + 1 < columns.size()) sql << ", ";
  }
  sql << ") SELECT ";
  for (size_t i = 0; i < columns.size(); ++i) {
    sql << "d." << QuoteIdent(columns[i]);
    if (i + 1 < columns.size()) sql << ", ";
  }
  sql << " FROM " << delta_name << " d";
  if (!pk_columns.empty()) {
    sql << " WHERE NOT EXISTS (SELECT 1 FROM " << target << " t WHERE ";
    for (size_t i = 0; i < pk_columns.size(); ++i) {
      if (i > 0) sql << " AND ";
      sql << "t." << QuoteIdent(pk_columns[i]) << " IS NOT DISTINCT FROM "
          << "d." << QuoteIdent(pk_columns[i]);
    }
    sql << ")";
  }

  auto result = txn.conn->Query(sql.str());
  if (result->HasError()) {
    return Status::Error(StatusCode::kDuckDBError, result->GetError());
  }

  const std::string cleanup_sql = "DELETE FROM " + delta_name;
  st = ExecuteDDLOn(*txn.conn, cleanup_sql);
  if (!st.ok()) return st;
  return Status::Ok();
}

Status DuckDBAdapter::ApplyUpdates(ApplyTxn &txn, TableId,
                                  UpdateBatch batch) {
  if (!txn.active || !txn.conn) {
    return Status::Error(StatusCode::kInvalid, "Apply transaction not active");
  }

  try {
    for (const auto &sql : batch.statements) {
      auto result = txn.conn->Query(sql);
      if (result->HasError()) {
        return Status::Error(StatusCode::kDuckDBError, result->GetError());
      }
    }
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return Status::Ok();
}

Status DuckDBAdapter::ApplyBulkUpdates(ApplyTxn &txn, TableId table,
                                       BulkUpdateBatch batch) {
  if (!txn.active || !txn.conn) {
    return Status::Error(StatusCode::kInvalid, "Apply transaction not active");
  }
  if (batch.new_rows.empty()) {
    return Status::Ok();
  }
  if (batch.old_rows.size() != batch.new_rows.size()) {
    return Status::Error(StatusCode::kInvalid,
                         "Bulk update row counts do not match");
  }

  Status st = EnsureDeltaTable(*txn.conn, table);
  if (!st.ok()) return st;

  try {
    // Use main schema only - schema separation is at DuckDB file level
    const std::string delta_name = DeltaTableName(table);
    DUCKDB_ADAPTER_VERBOSE(
        "DuckDB ApplyBulkUpdates: using appender for %s",
        delta_name.c_str());
    duckdb::Appender *delta_appender = GetOrCreateAppender(txn, delta_name);
    if (!delta_appender) {
      return Status::Error(StatusCode::kInvalid, "Appender init failed");
    }

    DUCKDB_ADAPTER_VERBOSE(
        "DuckDB ApplyBulkUpdates: appending %zu old rows",
        batch.old_rows.size());
    size_t row_idx = 0;
    for (const auto &row : batch.old_rows) {
      delta_appender->BeginRow();
      size_t col_idx = 0;
      for (const auto &cell : row) {
        Status st = AppendCellValue(*delta_appender, cell);
        if (!st.ok()) {
          sql_print_warning(
              "DuckDB ApplyBulkUpdates: AppendCellValue failed row=%zu col=%zu",
              row_idx, col_idx);
          return st;
        }
        ++col_idx;
      }
      delta_appender->EndRow();
      ++row_idx;
    }

    std::vector<std::string> columns;
    st = GetTableColumnsOn(*txn.conn, table, &columns);
    if (!st.ok()) return st;
    if (columns.empty()) {
      return Status::Error(StatusCode::kInvalid,
                           "No columns available for bulk update");
    }
    std::vector<std::string> pk_columns;
    st = GetPrimaryKeyColumnsOn(*txn.conn, table, &pk_columns);
    if (!st.ok()) return st;

    for (const auto &row : batch.old_rows) {
      if (row.size() != columns.size()) {
        return Status::Error(StatusCode::kInvalid,
                             "Bulk update row does not match column count");
      }
    }
    for (const auto &row : batch.new_rows) {
      if (row.size() != columns.size()) {
        return Status::Error(StatusCode::kInvalid,
                             "Bulk update row does not match column count");
      }
    }

    const std::string target = QualifiedName(table);
    const std::string delta_quoted = QuoteIdent(delta_name);

    const std::vector<std::string> &join_columns =
        pk_columns.empty() ? columns : pk_columns;
    std::string join_sql;
    for (size_t i = 0; i < join_columns.size(); ++i) {
      if (i > 0) join_sql += " AND ";
      const std::string col = QuoteIdent(join_columns[i]);
      join_sql += target + "." + col + " IS NOT DISTINCT FROM " + delta_quoted +
                  "." + col;
    }

    const std::string delete_sql =
        "DELETE FROM " + target + " USING " + delta_quoted + " WHERE " + join_sql;
    st = ExecuteDDLOn(*txn.conn, delete_sql);
    if (!st.ok()) return st;

    const std::string cleanup_sql = "DELETE FROM " + delta_quoted;
    st = ExecuteDDLOn(*txn.conn, cleanup_sql);
    if (!st.ok()) return st;

    RowBatch insert_batch;
    insert_batch.table = table;
    insert_batch.rows = std::move(batch.new_rows);
    st = AppendRows(txn, std::move(table), std::move(insert_batch));
    if (!st.ok()) return st;
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return Status::Ok();
}

Status DuckDBAdapter::ApplyBulkDeletes(ApplyTxn &txn, TableId table,
                                       BulkDeleteBatch batch) {
  if (!txn.active || !txn.conn) {
    return Status::Error(StatusCode::kInvalid, "Apply transaction not active");
  }
  if (batch.old_rows.empty()) {
    return Status::Ok();
  }

  Status st = EnsureDeltaTable(*txn.conn, table);
  if (!st.ok()) return st;

  try {
    // Use main schema only - schema separation is at DuckDB file level
    const std::string delta_name = DeltaTableName(table);
    duckdb::Appender *delta_appender = GetOrCreateAppender(txn, delta_name);
    if (!delta_appender) {
      return Status::Error(StatusCode::kInvalid, "Appender init failed");
    }

    for (const auto &row : batch.old_rows) {
      delta_appender->BeginRow();
      for (const auto &cell : row) {
        Status st = AppendCellValue(*delta_appender, cell);
        if (!st.ok()) return st;
      }
      delta_appender->EndRow();
    }

    std::vector<std::string> columns;
    st = GetTableColumnsOn(*txn.conn, table, &columns);
    if (!st.ok()) return st;
    if (columns.empty()) {
      return Status::Error(StatusCode::kInvalid,
                           "No columns available for bulk delete");
    }
    std::vector<std::string> pk_columns;
    st = GetPrimaryKeyColumnsOn(*txn.conn, table, &pk_columns);
    if (!st.ok()) return st;

    for (const auto &row : batch.old_rows) {
      if (row.size() != columns.size()) {
        return Status::Error(StatusCode::kInvalid,
                             "Bulk delete row does not match column count");
      }
    }

    const std::string target = QualifiedName(table);
    const std::string delta_quoted = QuoteIdent(delta_name);

    const std::vector<std::string> &join_columns =
        pk_columns.empty() ? columns : pk_columns;
    std::string join_sql;
    for (size_t i = 0; i < join_columns.size(); ++i) {
      if (i > 0) join_sql += " AND ";
      const std::string col = QuoteIdent(join_columns[i]);
      join_sql += target + "." + col + " IS NOT DISTINCT FROM " + delta_quoted +
                  "." + col;
    }

    const std::string delete_sql =
        "DELETE FROM " + target + " USING " + delta_quoted + " WHERE " + join_sql;
    st = ExecuteDDLOn(*txn.conn, delete_sql);
    if (!st.ok()) return st;

    const std::string cleanup_sql = "DELETE FROM " + delta_quoted;
    st = ExecuteDDLOn(*txn.conn, cleanup_sql);
    if (!st.ok()) return st;
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return Status::Ok();
}

Status DuckDBAdapter::ApplyDeletes(ApplyTxn &txn, TableId,
                                  DeleteBatch batch) {
  if (!txn.active || !txn.conn) {
    return Status::Error(StatusCode::kInvalid, "Apply transaction not active");
  }

  try {
    for (const auto &sql : batch.statements) {
      auto result = txn.conn->Query(sql);
      if (result->HasError()) {
        return Status::Error(StatusCode::kDuckDBError, result->GetError());
      }
    }
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return Status::Ok();
}

Status DuckDBAdapter::CloseAppenders(ApplyTxn &txn) {
  if (txn.appenders.empty()) {
    return Status::Ok();
  }
  try {
    for (auto &entry : txn.appenders) {
      if (entry.second) {
        entry.second->Close();
      }
    }
    txn.appenders.clear();
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }
  return Status::Ok();
}

Status DuckDBAdapter::CleanupInsertDeltaTables(
    const std::vector<TableId> &tables) {
  if (tables.empty()) {
    return Status::Ok();
  }
  if (!db_) {
    return Status::Error(StatusCode::kNotInitialized,
                         "DuckDBAdapter not initialized");
  }
  try {
    duckdb::Connection conn(*db_);
    for (const auto &table : tables) {
      TableId delta{table.schema, InsertDeltaTableName(table)};
      const std::string sql =
          "DROP TABLE IF EXISTS " + QualifiedName(delta);
      auto result = conn.Query(sql);
      if (result->HasError()) {
        return Status::Error(StatusCode::kDuckDBError, result->GetError());
      }
    }
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }
  return Status::Ok();
}

Status DuckDBAdapter::CommitApplyTxn(ApplyTxn &txn) {
  if (!txn.active || !txn.conn) {
    return Status::Error(StatusCode::kInvalid, "Apply transaction not active");
  }

  try {
    Status st = CloseAppenders(txn);
    if (!st.ok()) return st;
    auto result = txn.conn->Query("COMMIT");
    if (result->HasError()) {
      return Status::Error(StatusCode::kDuckDBError, result->GetError());
    }
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  txn.conn.reset();
  txn.db.reset();
  txn.active = false;
  return Status::Ok();
}

Status DuckDBAdapter::RollbackApplyTxn(ApplyTxn &txn) {
  if (!txn.active || !txn.conn) {
    return Status::Error(StatusCode::kInvalid, "Apply transaction not active");
  }

  try {
    Status st = CloseAppenders(txn);
    if (!st.ok()) return st;
    auto result = txn.conn->Query("ROLLBACK");
    if (result->HasError()) {
      return Status::Error(StatusCode::kDuckDBError, result->GetError());
    }
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  txn.conn.reset();
  txn.db.reset();
  txn.active = false;
  return Status::Ok();
}

QueryResult DuckDBAdapter::ExecuteQuery(std::string mysql_sql,
                                       SessionCtx) {
  QueryResult result;
  auto st = EnsureInitialized();
  if (!st.ok()) {
    result.ok = false;
    result.error = st.message;
    return result;
  }

  try {
    result.result = conn_->SendQuery(mysql_sql);
    if (!result.result || result.result->HasError()) {
      result.ok = false;
      result.error = result.result ? result.result->GetError()
                                   : "DuckDB query failed";
      return result;
    }
  } catch (const std::exception &ex) {
    result.ok = false;
    result.error = ex.what();
    return result;
  }

  result.ok = true;
  return result;
}

Status DuckDBAdapter::GetLatestWatermark(Gtid *gtid) {
  if (!gtid) {
    return Status::Error(StatusCode::kInvalid, "GTID output is null");
  }
  gtid->value.clear();

  auto st = EnsureInitialized();
  if (!st.ok()) return st;

  try {
    std::string error;
    if (!EnsureReplStateTable(*conn_, &error)) {
      return Status::Error(StatusCode::kDuckDBError,
                           error.empty() ? "Failed to ensure __repl_state"
                                         : error);
    }
    bool found = false;
    std::string applied;
    st = ReadReplStateRow(*conn_, nullptr, &applied, nullptr, &found);
    if (!st.ok()) return st;
    if (!found || applied.empty()) {
      std::string fallback;
      st = LoadWatermarkGtidSet(*conn_, &fallback);
      if (!st.ok()) return st;
      if (!fallback.empty() && !found) {
        Status persist = PersistReplStateFromWatermark(*conn_, fallback);
        if (!persist.ok()) return persist;
      }
      applied = std::move(fallback);
    }
    gtid->value = applied;
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return Status::Ok();
}

Status DuckDBAdapter::GetAppliedGtids(std::vector<Gtid> *gtids) {
  if (!gtids) {
    return Status::Error(StatusCode::kInvalid, "GTID list output is null");
  }
  gtids->clear();

  auto st = EnsureInitialized();
  if (!st.ok()) return st;

  try {
    std::string error;
    if (!EnsureReplStateTable(*conn_, &error)) {
      return Status::Error(StatusCode::kDuckDBError,
                           error.empty() ? "Failed to ensure __repl_state"
                                         : error);
    }
    bool found = false;
    std::string applied;
    st = ReadReplStateRow(*conn_, nullptr, &applied, nullptr, &found);
    if (!st.ok()) return st;
    if (!found || applied.empty()) {
      std::string fallback;
      st = LoadWatermarkGtidSet(*conn_, &fallback);
      if (!st.ok()) return st;
      if (!fallback.empty() && !found) {
        Status persist = PersistReplStateFromWatermark(*conn_, fallback);
        if (!persist.ok()) return persist;
      }
      applied = std::move(fallback);
    }
    if (!applied.empty()) {
      Gtid entry;
      entry.value = applied;
      gtids->push_back(std::move(entry));
    }
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return Status::Ok();
}

Status DuckDBAdapter::IsGtidApplied(const Gtid &gtid, bool *applied) {
  if (!applied) {
    return Status::Error(StatusCode::kInvalid, "Applied flag is null");
  }
  *applied = false;

  auto st = EnsureInitialized();
  if (!st.ok()) return st;

  try {
    std::string error;
    if (!EnsureReplStateTable(*conn_, &error)) {
      return Status::Error(StatusCode::kDuckDBError,
                           error.empty() ? "Failed to ensure __repl_state"
                                         : error);
    }
    bool found = false;
    std::string snapshot;
    std::string applied_set;
    st = ReadReplStateRow(*conn_, &snapshot, &applied_set, nullptr, &found);
    if (!st.ok()) return st;
    if (applied_set.empty() && !snapshot.empty()) {
      applied_set = snapshot;
    }
    if (!found || applied_set.empty()) {
      std::string fallback;
      st = LoadWatermarkGtidSet(*conn_, &fallback);
      if (!st.ok()) return st;
      if (!fallback.empty() && !found) {
        Status persist = PersistReplStateFromWatermark(*conn_, fallback);
        if (!persist.ok()) return persist;
      }
      applied_set = std::move(fallback);
    }
    if (applied_set.empty()) return Status::Ok();
    bool contains = false;
    if (!GtidSetContains(applied_set, gtid.value, &contains, &error)) {
      return Status::Error(StatusCode::kInvalid,
                           error.empty() ? "Failed to parse GTID set"
                                         : error);
    }
    *applied = contains;
  } catch (const std::exception &ex) {
    return Status::Error(StatusCode::kDuckDBError, ex.what());
  }

  return Status::Ok();
}

}  // namespace duckdb_se
