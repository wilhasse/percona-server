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

#include "storage/duckdb/ha_duckdb.h"

#include <fcntl.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/common/types/value.hpp"
#include "my_bitmap.h"
#include "my_dbug.h"
#include "my_systime.h"
#include "my_sys.h"
#include "mysqld_error.h"
#include "sql/log.h"
#include "sql/sql_error.h"
#include "sql/field.h"
#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "mysql/plugin.h"
#include "sql/dd/types/table.h"
#include "sql/my_decimal.h"
#include "sql/mysqld.h"
#include "sql/key.h"
#include "sql/psi_memory_key.h"
#include "sql/sql_const.h"
#include "sql/query_result.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"
#include "sql/sql_thd_internal_api.h"
#include "sql/sql_time.h"
#include "sql/rpl_gtid.h"
#include "sql/tztime.h"
#include "sql/visible_fields.h"
#include "scope_guard.h"
#include "storage/duckdb/duckdb_binlog_applier.h"
#include "storage/duckdb/duckdb_binlog_apply_thread.h"
#include "storage/duckdb/duckdb_compat.h"
#include "storage/duckdb/duckdb_engine_utils.h"
#include "storage/duckdb/duckdb_repl_state.h"
#include "sql/field.h"
#include "sql/table.h"
#include "sql_string.h"
#include "template_utils.h"
#include "thr_lock.h"

static char *duckdb_db_dir = nullptr;

namespace {

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

struct DuckdbTableState {
  DuckdbTableState(std::string path_in, bool replicated_in)
      : path(std::move(path_in)), replicated(replicated_in) {
    thr_lock_init(&lock);
  }

  ~DuckdbTableState() { thr_lock_delete(&lock); }

  DuckdbTableState(const DuckdbTableState &) = delete;
  DuckdbTableState &operator=(const DuckdbTableState &) = delete;

  THR_LOCK lock;
  std::string path;
  bool replicated{false};
};

class LoadedTables {
  std::map<std::pair<std::string, std::string>,
           std::unique_ptr<DuckdbTableState>>
      m_tables;
  std::mutex m_mutex;

 public:
  void add(const std::string &db, const std::string &table,
           const std::string &path, bool replicated = false) {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_tables[{db, table}] =
        std::make_unique<DuckdbTableState>(path, replicated);
  }

  DuckdbTableState *get(const std::string &db, const std::string &table) {
    std::lock_guard<std::mutex> guard(m_mutex);
    auto it = m_tables.find(std::make_pair(db, table));
    return it == m_tables.end() ? nullptr : it->second.get();
  }

  bool is_replicated(const std::string &db, const std::string &table) {
    std::lock_guard<std::mutex> guard(m_mutex);
    auto it = m_tables.find(std::make_pair(db, table));
    if (it == m_tables.end()) return false;
    return it->second->replicated;
  }

  void erase(const std::string &db, const std::string &table) {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_tables.erase(std::make_pair(db, table));
  }
};

LoadedTables *loaded_tables{nullptr};

static std::atomic<ulonglong> duckdb_instance_pool_hits{0};
static std::atomic<ulonglong> duckdb_instance_pool_misses{0};
static std::atomic<ulonglong> duckdb_writer_lock_acquires{0};
static std::atomic<ulonglong> duckdb_writer_lock_waits{0};
static std::atomic<ulonglong> duckdb_writer_lock_wait_ns{0};

class DuckdbInstancePool {
  struct Entry {
    std::weak_ptr<duckdb::DuckDB> db;
    std::shared_ptr<std::timed_mutex> writer_mutex;
  };

  std::map<std::string, Entry> m_instances;
  std::mutex m_mutex;

 public:
  bool acquire(const std::string &path, std::shared_ptr<duckdb::DuckDB> *db,
               std::shared_ptr<std::timed_mutex> *writer_mutex,
               std::string *error) {
    if (db == nullptr || writer_mutex == nullptr) return false;

    std::lock_guard<std::mutex> guard(m_mutex);
    Entry &entry = m_instances[path];
    if (!entry.writer_mutex) {
      entry.writer_mutex = std::make_shared<std::timed_mutex>();
    }

    std::shared_ptr<duckdb::DuckDB> shared_db = entry.db.lock();
    if (shared_db) {
      duckdb_instance_pool_hits.fetch_add(1, std::memory_order_relaxed);
    } else {
      try {
        duckdb::DBConfig config(false);
        shared_db = std::make_shared<duckdb::DuckDB>(path, &config);
      } catch (const std::exception &ex) {
        if (error != nullptr) *error = ex.what();
        return false;
      }
      entry.db = shared_db;
      duckdb_instance_pool_misses.fetch_add(1, std::memory_order_relaxed);
    }

    *db = std::move(shared_db);
    *writer_mutex = entry.writer_mutex;
    return true;
  }
};

DuckdbInstancePool *duckdb_instance_pool{nullptr};

class Duckdb_execution_context : public Secondary_engine_execution_context {
 public:
  std::string db;
  std::string table;
  std::string db_path;
  std::string sql;
  std::string fail_reason;
  TABLE *base_table{nullptr};
  bool eligible{false};
};

std::string quote_ident(const char *name, size_t length) {
  std::string out;
  out.reserve(length + 2);
  out.push_back('"');
  for (size_t i = 0; i < length; ++i) {
    const char ch = name[i];
    if (ch == '"') out.push_back('"');
    out.push_back(ch);
  }
  out.push_back('"');
  return out;
}

std::string qualified_table_name(const std::string &schema,
                                 const std::string &table) {
  if (schema.empty()) {
    return quote_ident(table.c_str(), table.size());
  }
  std::string qualified;
  qualified.reserve(schema.size() + table.size() + 8);
  qualified.append(quote_ident(schema.c_str(), schema.size()));
  qualified.push_back('.');
  qualified.append(quote_ident("main", 4));
  qualified.push_back('.');
  qualified.append(quote_ident(table.c_str(), table.size()));
  return qualified;
}

std::string value_to_sql(const duckdb::Value &val);
std::string build_where_clause(const std::vector<Field *> &fields,
                               const std::vector<duckdb::Value> &values);
void store_duckdb_value(Field *field, const duckdb::Value &value);
int copy_chunk_row_to_table(TABLE *table, const duckdb::DataChunk &chunk,
                            duckdb::idx_t row_idx,
                            const std::vector<Field *> &projected_fields,
                            duckdb::idx_t col_offset);

static std::atomic<ulonglong> duckdb_bulk_insert_rows{0};
static std::atomic<ulonglong> duckdb_conversion_rows{0};
static std::atomic<ulonglong> duckdb_conversion_values{0};
static std::atomic<ulonglong> duckdb_conversion_ns{0};

class DuckdbWriterGuard {
 public:
  explicit DuckdbWriterGuard(
      const std::shared_ptr<std::timed_mutex> &writer_mutex)
      : m_writer_mutex(writer_mutex) {
    if (!m_writer_mutex) return;
    const auto wait_start = std::chrono::steady_clock::now();
    if (!m_writer_mutex->try_lock()) {
      duckdb_writer_lock_waits.fetch_add(1, std::memory_order_relaxed);
      m_writer_mutex->lock();
      const auto waited_ns = static_cast<ulonglong>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - wait_start)
              .count());
      duckdb_writer_lock_wait_ns.fetch_add(waited_ns,
                                           std::memory_order_relaxed);
    }
    duckdb_writer_lock_acquires.fetch_add(1, std::memory_order_relaxed);
    m_locked = true;
  }

  ~DuckdbWriterGuard() {
    if (m_locked && m_writer_mutex) m_writer_mutex->unlock();
  }

  bool locked() const { return m_locked; }

 private:
  std::shared_ptr<std::timed_mutex> m_writer_mutex;
  bool m_locked{false};
};

bool DuckdbTableExistsInFile(const std::string &path,
                             const std::string &schema,
                             const std::string &table, bool *exists) {
  if (!exists) return false;
  *exists = false;
  try {
    duckdb::DBConfig config(true);
    duckdb::DuckDB db(path, &config);
    duckdb::Connection conn(db);
    const std::string sql =
        "SELECT 1 FROM information_schema.tables WHERE table_schema = 'main' "
        "AND table_name = " +
        value_to_sql(duckdb::Value(table)) +
        (schema.empty() ? std::string()
                        : " AND table_catalog = " +
                              value_to_sql(duckdb::Value(schema))) +
        " LIMIT 1";
    auto result = conn.Query(sql);
    if (result->HasError()) {
      sql_print_warning("DuckDB table check failed: %s",
                        result->GetError().c_str());
      return false;
    }
    auto chunk = result->Fetch();
    if (chunk && chunk->size() > 0) {
      *exists = true;
      return true;
    }
    if (!schema.empty()) {
      const std::string fallback_sql =
          "SELECT 1 FROM information_schema.tables WHERE table_schema = " +
          value_to_sql(duckdb::Value(schema)) + " AND table_name = " +
          value_to_sql(duckdb::Value(table)) + " LIMIT 1";
      result = conn.Query(fallback_sql);
      if (result->HasError()) {
        sql_print_warning("DuckDB table check failed: %s",
                          result->GetError().c_str());
        return false;
      }
      chunk = result->Fetch();
      if (chunk && chunk->size() > 0) {
        *exists = true;
        return true;
      }
    }
  } catch (const std::exception &ex) {
    sql_print_warning("DuckDB table existence check failed: %s", ex.what());
    return false;
  }
  return true;
}

bool is_binary_field(const Field *field) {
  switch (field->type()) {
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_GEOMETRY:
    case MYSQL_TYPE_BIT:
    case MYSQL_TYPE_JSON:
      return true;
    // Numeric types are never binary even if field->binary() returns true
    // (which can happen during table scans due to MySQL's internal state)
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP:
      return false;
    default:
      break;
  }
  return field->binary();
}

bool is_integer_field_type(enum_field_types type) {
  switch (type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_YEAR:
      return true;
    default:
      return false;
  }
}

bool is_numeric_field_type(enum_field_types type) {
  switch (type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
      return true;
    default:
      return false;
  }
}

bool duckdb_value_to_ha_rows(const duckdb::Value &value, ha_rows *out) {
  if (out == nullptr || value.IsNull()) return false;
  const std::string text = value.ToString();
  if (text.empty()) return false;
  char *end = nullptr;
  const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return false;
  *out = static_cast<ha_rows>(parsed);
  return true;
}

bool duckdb_value_to_long_double(const duckdb::Value &value, long double *out) {
  if (out == nullptr || value.IsNull()) return false;
  const std::string text = value.ToString();
  if (text.empty()) return false;
  char *end = nullptr;
  const long double parsed = std::strtold(text.c_str(), &end);
  if (end == nullptr || *end != '\0' || !std::isfinite(parsed)) return false;
  *out = parsed;
  return true;
}

enum class MappingSeverity {
  kOk,
  kWarning,
  kLossy
};

struct DuckDBTypeMapping {
  std::string type;
  MappingSeverity severity{MappingSeverity::kOk};
  std::string reason;
};

DuckDBTypeMapping duckdb_type_mapping_for_field(const Field *field) {
  DuckDBTypeMapping mapping;
  if (field == nullptr) {
    mapping.type = "VARCHAR";
    mapping.severity = MappingSeverity::kLossy;
    mapping.reason = "unknown field type";
    return mapping;
  }

  const bool binary = is_binary_field(field);
  const bool unsigned_flag = field->is_unsigned();
  switch (field->type()) {
    case MYSQL_TYPE_TINY:
      mapping.type = unsigned_flag ? "UTINYINT" : "TINYINT";
      break;
    case MYSQL_TYPE_SHORT:
      mapping.type = unsigned_flag ? "USMALLINT" : "SMALLINT";
      break;
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
      mapping.type = unsigned_flag ? "UINTEGER" : "INTEGER";
      break;
    case MYSQL_TYPE_LONGLONG:
      mapping.type = unsigned_flag ? "UBIGINT" : "BIGINT";
      break;
    case MYSQL_TYPE_FLOAT:
      mapping.type = "FLOAT";
      break;
    case MYSQL_TYPE_DOUBLE:
      mapping.type = "DOUBLE";
      break;
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL: {
      uint precision = 0;
      uint scale = field->decimals();
      if (field->type() == MYSQL_TYPE_NEWDECIMAL) {
        const auto *dec = static_cast<const Field_new_decimal *>(field);
        precision = dec->precision;
      }
      if (precision == 0) {
        precision = field->field_length;
      }
      if (precision > 38) {
        mapping.type = "VARCHAR";
        mapping.severity = MappingSeverity::kLossy;
        mapping.reason = "DECIMAL precision > 38 stored as VARCHAR";
      } else {
        mapping.type = "DECIMAL(" + std::to_string(precision) + "," +
                       std::to_string(scale) + ")";
      }
      break;
    }
    case MYSQL_TYPE_DATE:
      mapping.type = "DATE";
      break;
    case MYSQL_TYPE_TIME:
      mapping.type = "TIME";
      break;
    case MYSQL_TYPE_DATETIME:
      mapping.type = "TIMESTAMP";
      break;
    case MYSQL_TYPE_TIMESTAMP:
      mapping.type = "TIMESTAMP";
      mapping.severity = MappingSeverity::kWarning;
      mapping.reason = "TIMESTAMP timezone semantics may differ";
      break;
    case MYSQL_TYPE_YEAR:
      mapping.type = "SMALLINT";
      mapping.severity = MappingSeverity::kWarning;
      mapping.reason = "YEAR stored as SMALLINT";
      break;
    case MYSQL_TYPE_BIT:
      mapping.type = "BLOB";
      mapping.severity = MappingSeverity::kLossy;
      mapping.reason = "BIT stored as BLOB";
      break;
    case MYSQL_TYPE_JSON:
      mapping.type = "BLOB";
      mapping.severity = MappingSeverity::kLossy;
      mapping.reason = "JSON stored as BLOB";
      break;
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
      mapping.type = "VARCHAR";
      mapping.severity = MappingSeverity::kLossy;
      mapping.reason = "ENUM/SET stored as VARCHAR";
      break;
    case MYSQL_TYPE_GEOMETRY:
      mapping.type = "BLOB";
      mapping.severity = MappingSeverity::kLossy;
      mapping.reason = "GEOMETRY stored as BLOB";
      break;
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_TINY_BLOB:
      mapping.type = binary ? "BLOB" : "VARCHAR";
      break;
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_VARCHAR:
      mapping.type = binary ? "BLOB" : "VARCHAR";
      break;
    default:
      mapping.type = binary ? "BLOB" : "VARCHAR";
      mapping.severity = MappingSeverity::kLossy;
      mapping.reason = "fallback to string storage";
      break;
  }

  return mapping;
}

void EmitTypeMappingWarning(THD *thd, const char *table, const char *column,
                            const DuckDBTypeMapping &mapping) {
  if (thd == nullptr) return;
  if (mapping.severity == MappingSeverity::kOk) return;
  const char *reason = mapping.reason.empty() ? "type mapping warning"
                                              : mapping.reason.c_str();
  push_warning_printf(
      thd, Sql_condition::SL_WARNING, ER_UNKNOWN_ERROR,
      "DuckDB type mapping for %s.%s uses %s (%s)",
      table != nullptr ? table : "unknown",
      column != nullptr ? column : "unknown", mapping.type.c_str(), reason);
}

class FieldOffsetGuard {
 public:
  FieldOffsetGuard(TABLE *table, const uchar *record)
      : table_(table),
        diff_(record != nullptr && table_ != nullptr
                  ? record - table_->record[0]
                  : 0) {
    if (diff_ == 0 || table_ == nullptr) return;
    const uint field_count = table_->s->fields;
    for (uint i = 0; i < field_count; ++i) {
      table_->field[i]->move_field_offset(diff_);
    }
  }

  ~FieldOffsetGuard() {
    if (diff_ == 0 || table_ == nullptr) return;
    const uint field_count = table_->s->fields;
    for (uint i = 0; i < field_count; ++i) {
      table_->field[i]->move_field_offset(-diff_);
    }
  }

 private:
  TABLE *table_;
  ptrdiff_t diff_;
};

duckdb::Value field_value(Field *field) {
  if (field->is_null()) return duckdb::Value();

  if (is_binary_field(field)) {
    String tmp;
    field->val_str(&tmp);
    return duckdb::Value::BLOB_RAW(std::string(tmp.ptr(), tmp.length()));
  }

  const bool unsigned_flag = field->is_unsigned();
  switch (field->type()) {
    case MYSQL_TYPE_TINY: {
      const longlong v = field->val_int();
      return unsigned_flag ? duckdb::Value::UTINYINT(static_cast<uint8_t>(v))
                           : duckdb::Value::TINYINT(static_cast<int8_t>(v));
    }
    case MYSQL_TYPE_SHORT: {
      const longlong v = field->val_int();
      return unsigned_flag ? duckdb::Value::USMALLINT(static_cast<uint16_t>(v))
                           : duckdb::Value::SMALLINT(static_cast<int16_t>(v));
    }
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG: {
      const longlong v = field->val_int();
      return unsigned_flag ? duckdb::Value::UINTEGER(static_cast<uint32_t>(v))
                           : duckdb::Value::INTEGER(static_cast<int32_t>(v));
    }
    case MYSQL_TYPE_LONGLONG: {
      const longlong v = field->val_int();
      return unsigned_flag ? duckdb::Value::UBIGINT(static_cast<uint64_t>(v))
                           : duckdb::Value::BIGINT(static_cast<int64_t>(v));
    }
    case MYSQL_TYPE_FLOAT: {
      const double v = field->val_real();
      return duckdb::Value::FLOAT(static_cast<float>(v));
    }
    case MYSQL_TYPE_DOUBLE: {
      const double v = field->val_real();
      return duckdb::Value::DOUBLE(v);
    }
    case MYSQL_TYPE_YEAR: {
      const longlong v = field->val_int();
      return duckdb::Value::SMALLINT(static_cast<int16_t>(v));
    }
    default:
      break;
  }

  String tmp;
  field->val_str(&tmp);
  return duckdb::Value(std::string(tmp.ptr(), tmp.length()));
}

void store_duckdb_value(Field *field, const duckdb::Value &value) {
  if (value.IsNull()) {
    field->set_null();
    return;
  }
  field->set_notnull();

  if (is_binary_field(field)) {
    const auto &str = duckdb::StringValue::Get(value);
    field->store(str.data(), str.size(), field->charset());
    return;
  }

  const bool unsigned_flag = field->is_unsigned();
  switch (field->type()) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_YEAR: {
      // Use DefaultCastAs to handle all DuckDB integer widths (INT32,
      // INT64, etc.) instead of assuming BIGINT.
      if (unsigned_flag) {
        auto casted = value.DefaultCastAs(duckdb::LogicalType::UBIGINT);
        const uint64_t v = duckdb::UBigIntValue::Get(casted);
        field->store(static_cast<longlong>(v), true);
      } else {
        auto casted = value.DefaultCastAs(duckdb::LogicalType::BIGINT);
        const int64_t v = duckdb::BigIntValue::Get(casted);
        field->store(static_cast<longlong>(v), false);
      }
      return;
    }
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE: {
      auto casted = value.DefaultCastAs(duckdb::LogicalType::DOUBLE);
      const double v = duckdb::DoubleValue::Get(casted);
      field->store(v);
      return;
    }
    default:
      break;
  }

  std::string text;
  const auto type_id = value.type().id();
  if (type_id == duckdb::LogicalTypeId::VARCHAR ||
      type_id == duckdb::LogicalTypeId::BLOB) {
    text = duckdb::StringValue::Get(value);
  } else {
    text = value.ToString();
  }
  field->store(text.data(), text.size(), field->charset());
}

std::string value_to_sql(const duckdb::Value &val) {
  if (val.IsNull()) return "NULL";
  // Use DuckDB's ToString which properly escapes values
  const auto &type = val.type();
  if (type.id() == duckdb::LogicalTypeId::VARCHAR ||
      type.id() == duckdb::LogicalTypeId::BLOB) {
    std::string str = val.ToString();
    std::string escaped;
    escaped.reserve(str.size() + 4);
    escaped.push_back('\'');
    for (char ch : str) {
      if (ch == '\'') escaped.push_back('\'');
      escaped.push_back(ch);
    }
    escaped.push_back('\'');
    return escaped;
  }
  return val.ToString();
}

std::vector<Field *> collect_fields(TABLE *table, const MY_BITMAP *bitmap) {
  std::vector<Field *> fields;
  if (table == nullptr || table->s == nullptr) return fields;

  const uint field_count = table->s->fields;
  fields.reserve(field_count);
  for (uint i = 0; i < field_count; ++i) {
    Field *field = table->field[i];
    if (bitmap == nullptr ||
        bitmap_is_set(bitmap, field->field_index())) {
      fields.push_back(field);
    }
  }

  if (fields.empty() && bitmap != nullptr) {
    for (uint i = 0; i < field_count; ++i) {
      fields.push_back(table->field[i]);
    }
  }

  return fields;
}

bool should_use_projection_pushdown(TABLE *table) {
  if (table == nullptr) return false;
  THD *thd = current_thd;
  if (thd == nullptr || thd->lex == nullptr) return true;
  switch (thd->lex->sql_command) {
    case SQLCOM_DELETE:
    case SQLCOM_DELETE_MULTI:
    case SQLCOM_UPDATE:
    case SQLCOM_UPDATE_MULTI:
      return false;
    default:
      return true;
  }
}

std::vector<Field *> collect_projection_fields(TABLE *table) {
  if (!should_use_projection_pushdown(table)) {
    return collect_fields(table, nullptr);
  }
  return collect_fields(table, table->read_set);
}

std::string build_select_list(const std::vector<Field *> &fields) {
  if (fields.empty()) return "*";

  std::string sql;
  bool first = true;
  for (const Field *field : fields) {
    if (field == nullptr) continue;
    if (!first) sql.append(", ");
    sql.append(
        quote_ident(field->field_name, std::strlen(field->field_name)));
    first = false;
  }
  return sql.empty() ? "*" : sql;
}

std::vector<duckdb::Value> collect_values(TABLE *table, const uchar *record,
                                          const std::vector<Field *> &fields) {
  FieldOffsetGuard guard(table, record);
  std::vector<duckdb::Value> values;
  values.reserve(fields.size());
  for (Field *field : fields) {
    values.push_back(field_value(field));
  }
  return values;
}

const KEY *duckdb_primary_key_info(const TABLE *table,
                                   const TABLE_SHARE *table_share) {
  if (table == nullptr || table_share == nullptr) return nullptr;
  if (table_share->primary_key == MAX_KEY) return nullptr;
  if (table->key_info == nullptr) return nullptr;
  return table->key_info + table_share->primary_key;
}

std::vector<Field *> collect_primary_key_fields(TABLE *table,
                                                const TABLE_SHARE *table_share) {
  std::vector<Field *> fields;
  const KEY *primary_key = duckdb_primary_key_info(table, table_share);
  if (primary_key == nullptr) return fields;
  fields.reserve(primary_key->user_defined_key_parts);
  for (uint i = 0; i < primary_key->user_defined_key_parts; ++i) {
    Field *field = primary_key->key_part[i].field;
    if (field == nullptr) continue;
    if (std::find(fields.begin(), fields.end(), field) != fields.end()) continue;
    fields.push_back(field);
  }
  return fields;
}

std::vector<Field *> collect_key_prefix_fields(const KEY *key_info,
                                               uint key_parts) {
  std::vector<Field *> fields;
  if (key_info == nullptr || key_parts == 0) return fields;
  fields.reserve(key_parts);
  for (uint i = 0; i < key_parts; ++i) {
    Field *field = key_info->key_part[i].field;
    if (field == nullptr) continue;
    if (std::find(fields.begin(), fields.end(), field) != fields.end()) continue;
    fields.push_back(field);
  }
  return fields;
}

std::string build_primary_key_order_by(const KEY *primary_key, bool descending) {
  if (primary_key == nullptr || primary_key->user_defined_key_parts == 0) {
    return std::string();
  }
  std::string sql = " ORDER BY ";
  bool first = true;
  for (uint i = 0; i < primary_key->user_defined_key_parts; ++i) {
    const Field *field = primary_key->key_part[i].field;
    if (field == nullptr) continue;
    if (!first) sql.append(", ");
    sql.append(quote_ident(field->field_name, std::strlen(field->field_name)));
    sql.append(descending ? " DESC" : " ASC");
    first = false;
  }
  return sql;
}

std::string build_lexicographic_predicate(const std::vector<Field *> &fields,
                                          const std::vector<duckdb::Value> &values,
                                          bool greater_than, bool inclusive) {
  if (fields.empty() || fields.size() != values.size()) return std::string();

  const std::string equal_sql = build_where_clause(fields, values);
  std::ostringstream strict;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i > 0) strict << " OR ";
    strict << "(";
    for (size_t j = 0; j < i; ++j) {
      if (j > 0) strict << " AND ";
      strict << quote_ident(fields[j]->field_name, std::strlen(fields[j]->field_name))
             << " IS NOT DISTINCT FROM " << value_to_sql(values[j]);
    }
    if (i > 0) strict << " AND ";
    strict << quote_ident(fields[i]->field_name, std::strlen(fields[i]->field_name))
           << (greater_than ? " > " : " < ") << value_to_sql(values[i]) << ")";
  }

  if (!inclusive) return strict.str();

  std::string sql("(");
  sql += equal_sql;
  sql += ") OR (";
  sql += strict.str();
  sql += ")";
  return sql;
}

int copy_chunk_row_to_table(TABLE *table, const duckdb::DataChunk &chunk,
                            duckdb::idx_t row_idx,
                            const std::vector<Field *> &projected_fields) {
  if (table == nullptr || table->s == nullptr) return HA_ERR_GENERIC;
  if (chunk.ColumnCount() != projected_fields.size()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "DuckDB column count mismatch");
    return HA_ERR_GENERIC;
  }
  return copy_chunk_row_to_table(table, chunk, row_idx, projected_fields, 0);
}

int copy_chunk_row_to_table(TABLE *table, const duckdb::DataChunk &chunk,
                            duckdb::idx_t row_idx,
                            const std::vector<Field *> &projected_fields,
                            duckdb::idx_t col_offset) {
  if (table == nullptr || table->s == nullptr) return HA_ERR_GENERIC;
  if (chunk.ColumnCount() < projected_fields.size() + col_offset) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "DuckDB column count mismatch");
    return HA_ERR_GENERIC;
  }

  my_bitmap_map *old_map = dbug_tmp_use_all_columns(table, table->write_set);
  const auto conversion_start = std::chrono::steady_clock::now();
  ulonglong converted_values = 0;

  for (duckdb::idx_t col_idx = 0; col_idx < projected_fields.size(); ++col_idx) {
    Field *field = projected_fields[col_idx];
    if (field == nullptr) continue;
    const duckdb::Value value = chunk.GetValue(col_idx + col_offset, row_idx);
    store_duckdb_value(field, value);
    ++converted_values;
  }

  const auto elapsed_ns = static_cast<ulonglong>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - conversion_start)
          .count());
  duckdb_conversion_rows.fetch_add(1, std::memory_order_relaxed);
  duckdb_conversion_values.fetch_add(converted_values,
                                     std::memory_order_relaxed);
  duckdb_conversion_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
  dbug_tmp_restore_column_map(table->write_set, old_map);
  return 0;
}

bool parse_probe_index(const duckdb::Value &value, size_t *probe_idx) {
  if (probe_idx == nullptr || value.IsNull()) return false;
  const std::string probe_text = duckdb::StringValue::Get(value);
  if (probe_text.empty()) return false;
  char *end = nullptr;
  const unsigned long long parsed = std::strtoull(probe_text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') return false;
  *probe_idx = static_cast<size_t>(parsed);
  return true;
}

std::string build_set_clause(const std::vector<Field *> &fields,
                             const std::vector<duckdb::Value> &values) {
  std::string sql;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i > 0) sql.append(", ");
    const char *name = fields[i]->field_name;
    sql.append(quote_ident(name, std::strlen(name)));
    sql.append(" = ");
    sql.append(value_to_sql(values[i]));
  }
  return sql;
}

std::string build_where_clause(const std::vector<Field *> &fields,
                               const std::vector<duckdb::Value> &values) {
  std::string sql;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i > 0) sql.append(" AND ");
    const char *name = fields[i]->field_name;
    sql.append(quote_ident(name, std::strlen(name)));
    sql.append(" IS NOT DISTINCT FROM ");
    sql.append(value_to_sql(values[i]));
  }
  return sql;
}

bool is_string_like_mysql_type(enum_field_types type) {
  switch (type) {
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_JSON:
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
      return true;
    default:
      return false;
  }
}

bool is_temporal_mysql_type(enum_field_types type) {
  switch (type) {
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_DATETIME2:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_TIME2:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIMESTAMP2:
    case MYSQL_TYPE_NEWDATE:
      return true;
    default:
      return false;
  }
}

struct DuckdbScalarExpression {
  std::string sql;
  bool references_target_table{false};
  bool string_like{false};
  bool null_literal{false};
};

std::string combine_predicates(const std::string &left,
                               const std::string &right) {
  if (left.empty()) return right;
  if (right.empty()) return left;
  return "(" + left + ") AND (" + right + ")";
}

bool build_pushdown_scalar_expr(const Item *item, TABLE *table,
                                DuckdbScalarExpression *out);

bool build_constant_sql(const Item *item, DuckdbScalarExpression *out) {
  if (item == nullptr || out == nullptr) return false;
  if (!item->const_item() || item->is_non_deterministic()) return false;

  const Item *real = item->real_item();
  if (real == nullptr) return false;

  if (real->type() == Item::NULL_ITEM) {
    out->sql = "NULL";
    out->null_literal = true;
    out->references_target_table = false;
    out->string_like = false;
    return true;
  }

  String tmp;
  Item *mutable_item = const_cast<Item *>(real);
  String *value = mutable_item->val_str(&tmp);
  if (mutable_item->null_value || value == nullptr) {
    out->sql = "NULL";
    out->null_literal = true;
    out->references_target_table = false;
    out->string_like = false;
    return true;
  }

  const Item_result result = real->result_type();
  const enum_field_types data_type = real->data_type();
  const std::string literal(value->ptr(), value->length());

  if (result == STRING_RESULT) {
    out->string_like = !is_temporal_mysql_type(data_type);
    out->sql = value_to_sql(duckdb::Value(literal));
  } else if (result == INT_RESULT || result == REAL_RESULT ||
             result == DECIMAL_RESULT) {
    out->string_like = false;
    out->sql = literal;
  } else {
    return false;
  }

  out->references_target_table = false;
  out->null_literal = false;
  return true;
}

bool build_pushdown_field_expr(const Item *item, TABLE *table,
                               DuckdbScalarExpression *out) {
  if (item == nullptr || table == nullptr || out == nullptr) return false;
  if (item->type() != Item::FIELD_ITEM) return false;

  const Item_field *field_item = down_cast<const Item_field *>(item);
  Field *field = field_item->field;
  if (field == nullptr || field->table != table) return false;
  if (field->type() == MYSQL_TYPE_GEOMETRY) return false;

  out->sql = quote_ident(field->field_name, std::strlen(field->field_name));
  out->references_target_table = true;
  out->string_like = is_string_like_mysql_type(field->type());
  out->null_literal = false;
  return true;
}

bool build_pushdown_numeric_function(const Item_func *func, TABLE *table,
                                     const char *op,
                                     DuckdbScalarExpression *out) {
  if (func == nullptr || out == nullptr || func->arg_count != 2) return false;
  Item **args = func->arguments();
  if (args == nullptr) return false;

  DuckdbScalarExpression left;
  DuckdbScalarExpression right;
  if (!build_pushdown_scalar_expr(args[0], table, &left) ||
      !build_pushdown_scalar_expr(args[1], table, &right)) {
    return false;
  }

  if (left.string_like || right.string_like) return false;
  if (left.null_literal || right.null_literal) return false;

  out->sql = "(" + left.sql + " " + op + " " + right.sql + ")";
  out->references_target_table =
      left.references_target_table || right.references_target_table;
  out->string_like = false;
  out->null_literal = false;
  return true;
}

bool build_pushdown_scalar_expr(const Item *item, TABLE *table,
                                DuckdbScalarExpression *out) {
  if (item == nullptr || table == nullptr || out == nullptr) return false;

  const Item *real = item->real_item();
  if (real == nullptr) return false;

  if (build_pushdown_field_expr(real, table, out)) return true;
  if (build_constant_sql(real, out)) return true;

  if (real->type() != Item::FUNC_ITEM) return false;
  const Item_func *func = down_cast<const Item_func *>(real);

  switch (func->functype()) {
    case Item_func::PLUS_FUNC:
      return build_pushdown_numeric_function(func, table, "+", out);
    case Item_func::MINUS_FUNC:
      return build_pushdown_numeric_function(func, table, "-", out);
    case Item_func::MUL_FUNC:
      return build_pushdown_numeric_function(func, table, "*", out);
    case Item_func::MOD_FUNC:
      return build_pushdown_numeric_function(func, table, "%", out);
    case Item_func::NEG_FUNC: {
      if (func->arg_count != 1) return false;
      Item **args = func->arguments();
      if (args == nullptr) return false;
      DuckdbScalarExpression arg;
      if (!build_pushdown_scalar_expr(args[0], table, &arg)) return false;
      if (arg.string_like || arg.null_literal) return false;
      out->sql = "(-" + arg.sql + ")";
      out->references_target_table = arg.references_target_table;
      out->string_like = false;
      out->null_literal = false;
      return true;
    }
    case Item_func::ABS_FUNC: {
      if (func->arg_count != 1) return false;
      Item **args = func->arguments();
      if (args == nullptr) return false;
      DuckdbScalarExpression arg;
      if (!build_pushdown_scalar_expr(args[0], table, &arg)) return false;
      if (arg.string_like || arg.null_literal) return false;
      out->sql = "ABS(" + arg.sql + ")";
      out->references_target_table = arg.references_target_table;
      out->string_like = false;
      out->null_literal = false;
      return true;
    }
    default:
      return false;
  }
}

bool build_pushdown_predicate(const Item *cond, TABLE *table, std::string *out);

bool build_binary_comparison_predicate(const Item_func *func, TABLE *table,
                                       const char *op, std::string *out) {
  if (func == nullptr || out == nullptr || func->arg_count != 2) return false;
  Item **args = func->arguments();
  if (args == nullptr) return false;

  DuckdbScalarExpression left;
  DuckdbScalarExpression right;
  if (!build_pushdown_scalar_expr(args[0], table, &left) ||
      !build_pushdown_scalar_expr(args[1], table, &right)) {
    return false;
  }
  if (!(left.references_target_table || right.references_target_table)) {
    return false;
  }
  if (left.string_like || right.string_like) return false;

  if (left.null_literal || right.null_literal) {
    if (func->functype() == Item_func::EQUAL_FUNC) {
      return false;
    }
    if (func->functype() == Item_func::EQ_FUNC && left.references_target_table &&
        right.null_literal) {
      *out = "(" + left.sql + " IS NULL)";
      return true;
    }
    if (func->functype() == Item_func::EQ_FUNC && right.references_target_table &&
        left.null_literal) {
      *out = "(" + right.sql + " IS NULL)";
      return true;
    }
    if (func->functype() == Item_func::NE_FUNC && left.references_target_table &&
        right.null_literal) {
      *out = "(" + left.sql + " IS NOT NULL)";
      return true;
    }
    if (func->functype() == Item_func::NE_FUNC && right.references_target_table &&
        left.null_literal) {
      *out = "(" + right.sql + " IS NOT NULL)";
      return true;
    }
    return false;
  }

  if (func->functype() == Item_func::EQUAL_FUNC) {
    *out = "(" + left.sql + " IS NOT DISTINCT FROM " + right.sql + ")";
  } else {
    *out = "(" + left.sql + " " + std::string(op) + " " + right.sql + ")";
  }
  return true;
}

bool build_in_predicate(const Item_func *func, TABLE *table, std::string *out) {
  if (func == nullptr || out == nullptr || func->arg_count < 2) return false;
  Item **args = func->arguments();
  if (args == nullptr) return false;

  DuckdbScalarExpression lhs;
  if (!build_pushdown_scalar_expr(args[0], table, &lhs)) return false;
  if (!lhs.references_target_table || lhs.string_like) return false;

  std::string list_sql;
  for (uint i = 1; i < func->arg_count; ++i) {
    DuckdbScalarExpression rhs;
    if (!build_pushdown_scalar_expr(args[i], table, &rhs)) return false;
    if (rhs.references_target_table || rhs.string_like) return false;
    if (i > 1) list_sql.append(", ");
    list_sql.append(rhs.sql);
  }
  if (list_sql.empty()) return false;

  *out = "(" + lhs.sql + " IN (" + list_sql + "))";
  return true;
}

bool build_pushdown_predicate(const Item *cond, TABLE *table, std::string *out) {
  if (cond == nullptr || table == nullptr || out == nullptr) return false;

  const Item *real = cond->real_item();
  if (real == nullptr) return false;

  if (table->pos_in_table_list != nullptr) {
    const table_map allowed_tables =
        table->pos_in_table_list->map() | PSEUDO_TABLE_BITS;
    if ((real->used_tables() & ~allowed_tables) != 0) return false;
  }

  if (real->type() == Item::COND_ITEM) {
    const Item_cond *cond_item = down_cast<const Item_cond *>(real);
    const Item_func::Functype type = cond_item->functype();
    if (type != Item_func::COND_AND_FUNC && type != Item_func::COND_OR_FUNC) {
      return false;
    }

    std::string joined;
    bool first = true;
    for (const Item &arg : *cond_item->argument_list()) {
      std::string part;
      if (!build_pushdown_predicate(&arg, table, &part)) return false;
      if (!first) {
        joined.append(type == Item_func::COND_AND_FUNC ? " AND " : " OR ");
      }
      joined.append(part);
      first = false;
    }
    if (joined.empty()) return false;
    *out = "(" + joined + ")";
    return true;
  }

  if (real->type() != Item::FUNC_ITEM) return false;
  const Item_func *func = down_cast<const Item_func *>(real);

  switch (func->functype()) {
    case Item_func::NOT_FUNC: {
      if (func->arg_count != 1) return false;
      Item **args = func->arguments();
      if (args == nullptr) return false;
      std::string part;
      if (!build_pushdown_predicate(args[0], table, &part)) return false;
      *out = "(NOT " + part + ")";
      return true;
    }
    case Item_func::EQ_FUNC:
      return build_binary_comparison_predicate(func, table, "=", out);
    case Item_func::EQUAL_FUNC:
      return build_binary_comparison_predicate(func, table, "=", out);
    case Item_func::NE_FUNC:
      return build_binary_comparison_predicate(func, table, "<>", out);
    case Item_func::LT_FUNC:
      return build_binary_comparison_predicate(func, table, "<", out);
    case Item_func::LE_FUNC:
      return build_binary_comparison_predicate(func, table, "<=", out);
    case Item_func::GT_FUNC:
      return build_binary_comparison_predicate(func, table, ">", out);
    case Item_func::GE_FUNC:
      return build_binary_comparison_predicate(func, table, ">=", out);
    case Item_func::ISNULL_FUNC:
    case Item_func::ISNOTNULL_FUNC: {
      if (func->arg_count != 1) return false;
      Item **args = func->arguments();
      if (args == nullptr) return false;
      DuckdbScalarExpression arg;
      if (!build_pushdown_scalar_expr(args[0], table, &arg)) return false;
      if (!arg.references_target_table) return false;
      if (func->functype() == Item_func::ISNULL_FUNC) {
        *out = "(" + arg.sql + " IS NULL)";
      } else {
        *out = "(" + arg.sql + " IS NOT NULL)";
      }
      return true;
    }
    case Item_func::IN_FUNC:
      return build_in_predicate(func, table, out);
    default:
      return false;
  }
}

bool parse_table_path(const char *path, std::string *schema_name,
                      std::string *table_name) {
  if (path == nullptr || schema_name == nullptr || table_name == nullptr) {
    return false;
  }
  const std::string path_str(path);
  const size_t table_sep = path_str.find_last_of(FN_LIBCHAR);
  if (table_sep == std::string::npos || table_sep + 1 >= path_str.size()) {
    return false;
  }
  const size_t schema_sep = path_str.find_last_of(FN_LIBCHAR, table_sep - 1);
  if (schema_sep == std::string::npos || schema_sep + 1 >= table_sep) {
    return false;
  }
  *schema_name = path_str.substr(schema_sep + 1, table_sep - schema_sep - 1);
  *table_name = path_str.substr(table_sep + 1);
  return !schema_name->empty() && !table_name->empty();
}

std::string default_duckdb_path_for_schema(const std::string &schema_name) {
  const char *base_dir =
      (duckdb_db_dir != nullptr && duckdb_db_dir[0] != '\0')
          ? duckdb_db_dir
          : mysql_real_data_home;
  std::string dir = base_dir ? base_dir : "";
  if (!dir.empty() && dir.back() != FN_LIBCHAR) dir.push_back(FN_LIBCHAR);
  return dir + schema_name + ".duckdb";
}

std::string default_duckdb_path(const TABLE_SHARE *share) {
  return default_duckdb_path_for_schema(
      std::string(share->db.str, share->db.length));
}

std::string resolve_duckdb_path(const TABLE_SHARE *share) {
  if (share->secondary_engine_attribute.length > 0) {
    std::string attr(share->secondary_engine_attribute.str,
                     share->secondary_engine_attribute.length);
    if (attr.size() >= 7 &&
        attr.compare(attr.size() - 7, 7, ".duckdb") == 0) {
      return attr;
    }
    if (!attr.empty() && attr.back() != FN_LIBCHAR) attr.push_back(FN_LIBCHAR);
    std::string file;
    file.append(share->db.str, share->db.length);
    file.append(".duckdb");
    return attr + file;
  }
  return default_duckdb_path(share);
}

std::string resolve_duckdb_path(const std::string &schema_name,
                                const dd::Table *table_def) {
  if (table_def != nullptr) {
    const LEX_CSTRING attr = table_def->secondary_engine_attribute();
    if (attr.length > 0) {
      std::string attribute(attr.str, attr.length);
      if (attribute.size() >= 7 &&
          attribute.compare(attribute.size() - 7, 7, ".duckdb") == 0) {
        return attribute;
      }
      if (!attribute.empty() && attribute.back() != FN_LIBCHAR) {
        attribute.push_back(FN_LIBCHAR);
      }
      return attribute + schema_name + ".duckdb";
    }
  }
  return default_duckdb_path_for_schema(schema_name);
}

std::string make_loading_table_name(const std::string &base, THD *thd) {
  std::ostringstream oss;
  oss << "__loading_" << base << "_" << static_cast<ulonglong>(my_micro_time());
  if (thd != nullptr) {
    oss << "_" << static_cast<ulonglong>(thd->thread_id());
  }
  return oss.str();
}

bool backup_stale_wal(const std::string &path) {
  const std::string wal = path + ".wal";
  MY_STAT stat_buf;
  if (my_stat(wal.c_str(), &stat_buf, MYF(0)) == nullptr) return true;
  std::ostringstream oss;
  oss << wal << ".bak." << static_cast<ulonglong>(my_micro_time());
  const std::string backup = oss.str();
  if (my_rename(wal.c_str(), backup.c_str(), MYF(0)) != 0) {
    char errbuf[MYSYS_STRERROR_SIZE];
    sql_print_warning("DuckDB WAL backup failed for %s (%s)", wal.c_str(),
                      my_strerror(errbuf, sizeof(errbuf), my_errno()));
    return false;
  }
  sql_print_warning("DuckDB WAL moved aside to %s before creating %s",
                    backup.c_str(), path.c_str());
  return true;
}

bool ensure_duckdb_file(const std::string &path) {
  // Check if directory exists; DuckDB will create the file itself
  MY_STAT stat_buf;
  const size_t dir_pos = path.rfind(FN_LIBCHAR);
  std::string dir = (dir_pos == std::string::npos)
                        ? std::string(".")
                        : path.substr(0, dir_pos);
  if (my_stat(dir.c_str(), &stat_buf, MYF(0)) == nullptr) {
    return false;  // Directory doesn't exist
  }
  if (my_stat(path.c_str(), &stat_buf, MYF(0)) == nullptr) {
    // If DB doesn't exist but WAL does, preserve it by moving aside.
    if (!backup_stale_wal(path)) return false;
  }
  return true;
}

std::string capture_snapshot_gtid() {
  std::string snapshot;
  if (gtid_state != nullptr) {
    gtid_state->get_snapshot_gtid_executed(snapshot);
  }
  return snapshot;
}

bool store_snapshot_gtid(duckdb::Connection &con,
                         const std::string &snapshot_gtid) {
  std::string error;
  if (!duckdb_se::EnsureReplStateTable(con, &error)) return false;

  const std::string gtid_sql = value_to_sql(duckdb::Value(snapshot_gtid));
  const std::string ts_sql = CurrentTimestampLiteral();
  const std::string sql =
      "INSERT INTO __repl_state (channel, snapshot_gtid_set, applied_gtid_set, "
      "last_commit_ts) "
      "VALUES ('default', " +
      gtid_sql + ", " + gtid_sql + ", " + ts_sql + ") "
      "ON CONFLICT(channel) DO UPDATE SET "
      "snapshot_gtid_set = excluded.snapshot_gtid_set, "
      "applied_gtid_set = excluded.applied_gtid_set, "
      "last_commit_ts = excluded.last_commit_ts";
  auto upsert_result = con.Query(sql);
  return !upsert_result->HasError();
}

bool is_simple_select(LEX *lex, Table_ref **base_table, std::string *reason,
                      std::string *schema_out, std::string *path_out) {
  if (lex == nullptr || base_table == nullptr) return false;
  *base_table = nullptr;
  if (schema_out) schema_out->clear();
  if (path_out) path_out->clear();

  if (lex->sql_command != SQLCOM_SELECT) {
    if (reason) *reason = "Only SELECT statements are supported";
    return false;
  }
  if (!lex->unit->is_simple() || !lex->is_single_level_stmt()) {
    if (reason)
      *reason = "Subqueries, unions, or derived tables are not supported";
    return false;
  }
  if (lex->param_list.elements != 0) {
    if (reason) *reason = "Prepared statements are not supported";
    return false;
  }

  int base_count = 0;
  std::string base_schema;
  std::string base_path;
  for (Table_ref *tl = lex->query_tables; tl != nullptr; tl = tl->next_global) {
    if (tl->is_placeholder()) continue;
    if (tl->is_view_or_derived()) {
      if (reason) *reason = "Views or derived tables are not supported";
      return false;
    }
    if (tl->schema_table != nullptr) {
      if (reason) *reason = "Schema tables are not supported";
      return false;
    }
    if (tl->is_sj_or_aj_nest()) {
      if (reason) *reason = "Semi/anti joins are not supported";
      return false;
    }
    if (tl->is_natural_join || tl->natural_join != nullptr ||
        tl->join_using_fields != nullptr) {
      if (reason) *reason = "NATURAL/USING joins are not supported";
      return false;
    }

    if (tl->table == nullptr || tl->table->s == nullptr) {
      if (reason) *reason = "Table metadata not available for DuckDB offload";
      return false;
    }

    std::string schema;
    if (tl->db != nullptr && tl->db_length > 0) {
      schema.assign(tl->db, tl->db_length);
    } else if (tl->table->s->db.str != nullptr &&
               tl->table->s->db.length > 0) {
      schema.assign(tl->table->s->db.str, tl->table->s->db.length);
    }
    const std::string path = resolve_duckdb_path(tl->table->s);

    if (base_count == 0) {
      *base_table = tl;
      base_schema = schema;
      base_path = path;
    } else {
      if (!base_schema.empty() && !schema.empty() && base_schema != schema) {
        if (reason)
          *reason = "DuckDB offload requires tables in the same schema";
        return false;
      }
      if (!base_path.empty() && !path.empty() && base_path != path) {
        if (reason)
          *reason =
              "DuckDB offload requires tables in the same DuckDB database";
        return false;
      }
    }

    ++base_count;
  }

  if (base_count == 0) {
    if (reason) *reason = "No base tables found for DuckDB offload";
    return false;
  }

  if (schema_out) *schema_out = base_schema;
  if (path_out) *path_out = base_path;

  return true;
}

std::vector<duckdb_se::QualifiedTableRef> collect_query_tables(LEX *lex) {
  std::vector<duckdb_se::QualifiedTableRef> tables;
  if (lex == nullptr) return tables;
  for (Table_ref *tl = lex->query_tables; tl != nullptr; tl = tl->next_global) {
    if (tl->is_placeholder()) continue;
    if (tl->is_view_or_derived()) continue;
    if (tl->schema_table != nullptr) continue;
    if (tl->table == nullptr || tl->table->s == nullptr) continue;

    std::string schema;
    if (tl->table->s->db.str != nullptr && tl->table->s->db.length > 0) {
      schema.assign(tl->table->s->db.str, tl->table->s->db.length);
    } else if (tl->db != nullptr && tl->db_length > 0) {
      schema.assign(tl->db, tl->db_length);
    }

    std::string table_name;
    if (tl->table->s->table_name.str != nullptr &&
        tl->table->s->table_name.length > 0) {
      table_name.assign(tl->table->s->table_name.str,
                        tl->table->s->table_name.length);
    } else if (tl->table_name != nullptr && tl->table_name_length > 0) {
      table_name.assign(tl->table_name, tl->table_name_length);
    }

    if (!schema.empty() && !table_name.empty()) {
      tables.push_back(duckdb_se::QualifiedTableRef{schema, table_name});
    }
  }
  return tables;
}

std::string build_missing_loaded_tables_reason(THD *thd) {
  if (thd == nullptr || thd->lex == nullptr || loaded_tables == nullptr) {
    return "";
  }
  const auto tables = collect_query_tables(thd->lex);
  if (tables.empty()) return "";

  std::vector<std::string> missing;
  missing.reserve(tables.size());
  for (const auto &ref : tables) {
    if (ref.schema.empty() || ref.table.empty()) continue;
    if (loaded_tables->get(ref.schema, ref.table) == nullptr) {
      missing.push_back(ref.schema + "." + ref.table);
    }
  }
  if (missing.empty()) return "";

  std::sort(missing.begin(), missing.end());
  missing.erase(std::unique(missing.begin(), missing.end()), missing.end());

  std::ostringstream oss;
  oss << "DuckDB secondary tables not loaded: ";
  for (size_t i = 0; i < missing.size(); ++i) {
    if (i > 0) oss << ", ";
    oss << missing[i];
  }
  oss << ". Run ALTER TABLE ... SECONDARY_LOAD";
  return oss.str();
}

bool collect_visible_fields(const mem_root_deque<Item *> &fields,
                            std::vector<Item *> *out,
                            std::string *reason) {
  if (!out) return false;
  out->clear();
  for (Item *item : fields) {
    if (item == nullptr) {
      if (reason) *reason = "DuckDB select list contains null item";
      return false;
    }
    if (item->hidden) continue;
    out->push_back(item);
  }
  if (out->empty()) {
    if (reason) *reason = "DuckDB select list is empty";
    return false;
  }
  return true;
}

bool uses_supported_select_items(const mem_root_deque<Item *> &fields,
                                 std::string *reason) {
  std::vector<Item *> visible;
  if (!collect_visible_fields(fields, &visible, reason)) return false;
  for (Item *item : visible) {
    Item *real = item->real_item();
    if (real->result_type() == ROW_RESULT) {
      if (reason) *reason = "Row constructors are not supported";
      return false;
    }
    if (real->type() == Item::SUBSELECT_ITEM) {
      if (reason) *reason = "Subqueries in SELECT list are not supported";
      return false;
    }
  }
  return true;
}

Item_cache *create_duckdb_output_cache(Item *item, std::string *reason) {
  if (item == nullptr) {
    if (reason) *reason = "DuckDB output item missing";
    return nullptr;
  }
  if (item->result_type() == ROW_RESULT) {
    if (reason) *reason = "Row constructors are not supported";
    return nullptr;
  }
  if (item->data_type() == MYSQL_TYPE_JSON) {
    return new Item_cache_str(item);
  }
  return Item_cache::get_cache(item);
}

std::string duckdb_value_to_string(const duckdb::Value &value) {
  const auto type_id = value.type().id();
  if (type_id == duckdb::LogicalTypeId::VARCHAR ||
      type_id == duckdb::LogicalTypeId::BLOB) {
    return duckdb::StringValue::Get(value);
  }
  return value.ToString();
}

bool duckdb_validate_read_only_settings() {
  if (!read_only || !super_read_only) {
    sql_print_warning(
        "DuckDB analytics node should run with read_only=ON and "
        "super_read_only=ON (current: read_only=%s, super_read_only=%s)",
        read_only ? "ON" : "OFF", super_read_only ? "ON" : "OFF");
    return false;
  }
  return true;
}

bool store_duckdb_result_value(THD *thd, Item *item, Item_cache *cache,
                               const duckdb::Value &value,
                               std::string *reason) {
  if (cache == nullptr) {
    if (reason) *reason = "DuckDB output cache missing";
    return true;
  }
  if (value.IsNull()) {
    cache->store_null();
    return false;
  }

  cache->null_value = false;
  switch (cache->result_type()) {
    case INT_RESULT: {
      const longlong v = item->unsigned_flag
                             ? static_cast<longlong>(value.GetValue<uint64_t>())
                             : static_cast<longlong>(value.GetValue<int64_t>());
      down_cast<Item_cache_int *>(cache)->store_value(cache, v);
      return false;
    }
    case REAL_RESULT: {
      const double v = value.GetValue<double>();
      down_cast<Item_cache_real *>(cache)->store_value(cache, v);
      return false;
    }
    case DECIMAL_RESULT: {
      const std::string text = duckdb_value_to_string(value);
      my_decimal dec;
      if (str2my_decimal(E_DEC_FATAL_ERROR, text.c_str(), text.size(),
                         item->collation.collation, &dec)) {
        if (reason) *reason = "DuckDB decimal conversion failed";
        return true;
      }
      down_cast<Item_cache_decimal *>(cache)->store_value(cache, &dec);
      return false;
    }
    case STRING_RESULT: {
      const std::string text = duckdb_value_to_string(value);
      if (item->is_temporal()) {
        String tmp(text.c_str(), text.size(), item->collation.collation);
        MYSQL_TIME ltime;
        MYSQL_TIME_STATUS status{};
        const my_time_flags_t flags = TIME_FUZZY_DATE;
        bool parse_error = false;
        if (item->data_type() == MYSQL_TYPE_TIME) {
          parse_error = str_to_time(&tmp, &ltime, flags, &status);
        } else {
          parse_error = str_to_datetime(&tmp, &ltime, flags, &status);
        }
        if (!parse_error && ltime.time_type == MYSQL_TIMESTAMP_DATETIME_TZ) {
          parse_error =
              convert_time_zone_displacement(thd->time_zone(), &ltime);
        }
        if (parse_error) {
          if (reason) *reason = "DuckDB temporal conversion failed";
          return true;
        }
        const longlong packed =
            TIME_to_longlong_packed(ltime, item->data_type());
        down_cast<Item_cache_datetime *>(cache)->store_value(cache, packed);
        return false;
      }
      String tmp(text.c_str(), text.size(), item->collation.collation);
      down_cast<Item_cache_str *>(cache)->store_value(cache, tmp);
      return false;
    }
    default:
      break;
  }

  if (reason) *reason = "Unsupported DuckDB result type";
  return true;
}

// Thread-local storage for fail reason that persists across context changes
static thread_local std::string tls_fail_reason;

static void DuckdbSetOffloadFailReason(THD *thd, const char *reason);

static bool PrepareSecondaryEngine(THD *thd, LEX *lex) {
  // Clear thread-local fail reason from previous queries
  tls_fail_reason.clear();

  auto *ctx = new (thd->mem_root) Duckdb_execution_context;
  if (ctx == nullptr) return true;
  lex->set_secondary_engine_execution_context(ctx);

  ctx->eligible = true;
  Table_ref *base_table = nullptr;
  for (Table_ref *tl = lex->query_tables; tl != nullptr; tl = tl->next_global) {
    if (tl->is_placeholder()) continue;
    if (tl->is_view_or_derived()) continue;
    if (tl->schema_table != nullptr) continue;
    base_table = tl;
    break;
  }
  if (base_table != nullptr && base_table->table != nullptr &&
      base_table->table->s != nullptr) {
    ctx->base_table = base_table->table;
    if (base_table->table->s->db.str != nullptr &&
        base_table->table->s->db.length > 0) {
      ctx->db.assign(base_table->table->s->db.str,
                     base_table->table->s->db.length);
    } else if (base_table->db != nullptr && base_table->db_length > 0) {
      ctx->db.assign(base_table->db, base_table->db_length);
    }
    if (base_table->table->s->table_name.str != nullptr &&
        base_table->table->s->table_name.length > 0) {
      ctx->table.assign(base_table->table->s->table_name.str,
                        base_table->table->s->table_name.length);
    } else if (base_table->table_name != nullptr &&
               base_table->table_name_length > 0) {
      ctx->table.assign(base_table->table_name,
                        base_table->table_name_length);
    }
    ctx->db_path = resolve_duckdb_path(base_table->table->s);
  }

  lex->add_statement_options(OPTION_NO_CONST_TABLES |
                             OPTION_NO_SUBQUERY_DURING_OPTIMIZATION);
  return false;
}

static bool DuckdbExecuteQuery(JOIN *join, Query_result *query_result) {
  if (join == nullptr || query_result == nullptr) return true;
  THD *thd = join->thd;
  if (thd == nullptr || thd->lex == nullptr) return true;

  std::string reason;
  Table_ref *base_table = nullptr;
  std::string schema;
  std::string path;
  if (!is_simple_select(thd->lex, &base_table, &reason, &schema, &path)) {
    DuckdbSetOffloadFailReason(thd, reason.c_str());
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), reason.c_str());
    return true;
  }

  const std::string missing = build_missing_loaded_tables_reason(thd);
  if (!missing.empty()) {
    DuckdbSetOffloadFailReason(thd, missing.c_str());
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), missing.c_str());
    return true;
  }

  const LEX_CSTRING query = thd->query();
  if (query.str == nullptr || query.length == 0) {
    reason = "DuckDB query text is empty";
    DuckdbSetOffloadFailReason(thd, reason.c_str());
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), reason.c_str());
    return true;
  }

  auto rewrite = duckdb_se::RewriteForDuckdb(
      std::string(query.str, query.length));
  if (!rewrite.ok) {
    reason = rewrite.reason.empty() ? "DuckDB SQL rewrite failed"
                                    : rewrite.reason;
    DuckdbSetOffloadFailReason(thd, reason.c_str());
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), reason.c_str());
    return true;
  }
  std::string rewritten = duckdb_se::RewriteQualifiedTables(
      rewrite.sql, collect_query_tables(thd->lex));

  mem_root_deque<Item *> *fields = join->query_expression()->get_field_list();
  std::vector<Item *> visible;
  if (!collect_visible_fields(*fields, &visible, &reason)) {
    DuckdbSetOffloadFailReason(thd, reason.c_str());
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), reason.c_str());
    return true;
  }

  std::vector<Item_cache *> caches;
  caches.reserve(visible.size());
  mem_root_deque<Item *> cache_items(thd->mem_root);
  for (Item *item : visible) {
    Item_cache *cache = create_duckdb_output_cache(item, &reason);
    if (cache == nullptr) {
      if (reason.empty()) reason = "DuckDB output cache setup failed";
      DuckdbSetOffloadFailReason(thd, reason.c_str());
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), reason.c_str());
      return true;
    }
    cache->store(item);
    caches.push_back(cache);
    cache_items.push_back(cache);
  }

  try {
    duckdb::DBConfig config(true);
    duckdb::DuckDB db(path, &config);
    duckdb::Connection conn(db);
    auto result = conn.SendQuery(rewritten);
    if (!result || result->HasError()) {
      const std::string err = result ? result->GetError()
                                     : "DuckDB query failed";
      DuckdbSetOffloadFailReason(thd, err.c_str());
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err.c_str());
      return true;
    }

    join->send_records = 0;
    while (true) {
      auto chunk = result->Fetch();
      if (!chunk || chunk->size() == 0) break;
      if (chunk->ColumnCount() != caches.size()) {
        reason = "DuckDB column count mismatch";
        DuckdbSetOffloadFailReason(thd, reason.c_str());
        my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), reason.c_str());
        return true;
      }
      for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
        for (duckdb::idx_t col = 0; col < chunk->ColumnCount(); ++col) {
          const duckdb::Value value = chunk->GetValue(col, row);
          if (store_duckdb_result_value(thd, visible[col], caches[col], value,
                                        &reason)) {
            if (reason.empty()) reason = "DuckDB value conversion failed";
            DuckdbSetOffloadFailReason(thd, reason.c_str());
            my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), reason.c_str());
            return true;
          }
        }
        if (query_result->send_data(thd, cache_items)) return true;
        join->send_records++;
      }
    }
  } catch (const std::exception &ex) {
    DuckdbSetOffloadFailReason(thd, ex.what());
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ex.what());
    return true;
  }

  return false;
}

static bool OptimizeSecondaryEngine(THD *thd, LEX *lex) {
  auto *ctx = dynamic_cast<Duckdb_execution_context *>(
      lex->secondary_engine_execution_context());
  if (ctx == nullptr) {
    thd->get_stmt_da()->set_error_status(thd, ER_PREPARE_FOR_PRIMARY_ENGINE);
    return true;
  }

  std::string reason;
  Table_ref *base_table = nullptr;
  std::string schema;
  std::string path;
  if (!is_simple_select(lex, &base_table, &reason, &schema, &path)) {
    DuckdbSetOffloadFailReason(thd, reason.c_str());
    if (thd->variables.use_secondary_engine == SECONDARY_ENGINE_FORCED) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), reason.c_str());
      return true;
    }
    thd->get_stmt_da()->set_error_status(thd, ER_PREPARE_FOR_PRIMARY_ENGINE);
    return true;
  }

  const std::string missing = build_missing_loaded_tables_reason(thd);
  if (!missing.empty()) {
    DuckdbSetOffloadFailReason(thd, missing.c_str());
    if (thd->variables.use_secondary_engine == SECONDARY_ENGINE_FORCED) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), missing.c_str());
      return true;
    }
    thd->get_stmt_da()->set_error_status(thd, ER_PREPARE_FOR_PRIMARY_ENGINE);
    return true;
  }

  Query_block *select = lex->unit != nullptr ? lex->unit->first_query_block()
                                             : nullptr;
  if (select == nullptr || select->join == nullptr) {
    reason = "DuckDB query block unavailable";
    DuckdbSetOffloadFailReason(thd, reason.c_str());
    if (thd->variables.use_secondary_engine == SECONDARY_ENGINE_FORCED) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), reason.c_str());
      return true;
    }
    thd->get_stmt_da()->set_error_status(thd, ER_PREPARE_FOR_PRIMARY_ENGINE);
    return true;
  }

  if (!uses_supported_select_items(*select->join->fields, &reason)) {
    DuckdbSetOffloadFailReason(thd, reason.c_str());
    if (thd->variables.use_secondary_engine == SECONDARY_ENGINE_FORCED) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), reason.c_str());
      return true;
    }
    thd->get_stmt_da()->set_error_status(thd, ER_PREPARE_FOR_PRIMARY_ENGINE);
    return true;
  }

  ctx->eligible = true;
  ctx->fail_reason.clear();
  ctx->db_path = path;

  select->join->override_executor_func = DuckdbExecuteQuery;
  return false;
}

static const char *DuckdbGetOffloadFailReason(THD *thd) {
  // Clear stale fail reason only when not in FORCED mode and not using secondary engine.
  // In FORCED mode, we always want to show the specific error message.
  // When using_secondary_storage_engine() is false after a FORCED query, it means
  // offload failed and we need to preserve the error message.
  if (thd != nullptr && thd->lex != nullptr &&
      thd->lex->m_sql_cmd != nullptr &&
      thd->variables.use_secondary_engine != SECONDARY_ENGINE_FORCED &&
      !thd->lex->m_sql_cmd->using_secondary_storage_engine()) {
    tls_fail_reason.clear();
  }
  // First check thread-local storage (set by ha_duckdb::open)
  if (!tls_fail_reason.empty()) {
    return tls_fail_reason.c_str();
  }
  // Fall back to context-based reason
  auto *ctx = dynamic_cast<Duckdb_execution_context *>(
      thd->lex->secondary_engine_execution_context());
  if (ctx == nullptr || ctx->fail_reason.empty()) {
    return "DuckDB secondary engine could not execute query";
  }
  return ctx->fail_reason.c_str();
}

static void DuckdbSetOffloadFailReason(THD *thd, const char *reason) {
  // Store in thread-local for retrieval even if context changes
  tls_fail_reason = reason ? reason : "DuckDB secondary engine failed";

  if (thd == nullptr || thd->lex == nullptr) {
    return;
  }
  auto *raw_ctx = thd->lex->secondary_engine_execution_context();
  if (raw_ctx == nullptr) return;

  auto *ctx = dynamic_cast<Duckdb_execution_context *>(raw_ctx);
  if (ctx != nullptr) {
    ctx->fail_reason = tls_fail_reason;
  }
  // If context belongs to another engine, leave it untouched.
  // tls_fail_reason is used by DuckdbGetOffloadFailReason().
}

}  // namespace

namespace duckdb_se {

void *duckdb_plugin_ptr = nullptr;
static std::atomic<bool> duckdb_binlog_apply_verbose_flag{false};

void SetDuckdbPluginPtr(void *ptr) { duckdb_plugin_ptr = ptr; }

void *GetDuckdbPluginPtr() { return duckdb_plugin_ptr; }

void RegisterLoadedTable(const std::string &schema, const std::string &table,
                         const std::string &path, bool replicated) {
  if (loaded_tables) loaded_tables->add(schema, table, path, replicated);
}

void UnregisterLoadedTable(const std::string &schema, const std::string &table) {
  if (loaded_tables) loaded_tables->erase(schema, table);
}

void SetDuckdbBinlogApplyVerbose(bool enabled) {
  duckdb_binlog_apply_verbose_flag.store(enabled);
}

bool DuckdbBinlogApplyVerbose() {
  return duckdb_binlog_apply_verbose_flag.load();
}

// Forward declaration for sysvar defined later
static char *duckdb_binlog_apply_start_gtid;

std::string GetDuckdbBinlogApplyStartGtid() {
  return duckdb_binlog_apply_start_gtid ? duckdb_binlog_apply_start_gtid : "";
}

ha_duckdb::ha_duckdb(handlerton *hton, TABLE_SHARE *table_share_arg)
    : handler(hton, table_share_arg) {}

int ha_duckdb::create(const char *, TABLE *table_arg, HA_CREATE_INFO *,
                      dd::Table *) {
  if (table_arg == nullptr || table_arg->s == nullptr) {
    return HA_ERR_GENERIC;
  }

  const std::string path = resolve_duckdb_path(table_arg->s);
  const size_t dir_pos = path.rfind(FN_LIBCHAR);
  if (dir_pos == std::string::npos) {
    my_error(ER_CANT_CREATE_TABLE, MYF(0), table_arg->s->table_name.str,
             HA_ERR_GENERIC, "Invalid DuckDB path");
    return HA_ERR_GENERIC;
  }

  const std::string dir = path.substr(0, dir_pos);
  MY_STAT stat_buf;
  if (my_stat(dir.c_str(), &stat_buf, MYF(0)) == nullptr) {
    my_error(ER_CANT_CREATE_TABLE, MYF(0), table_arg->s->table_name.str,
             HA_ERR_GENERIC, "DuckDB directory does not exist");
    return HA_ERR_GENERIC;
  }

  const bool file_exists = (my_stat(path.c_str(), &stat_buf, MYF(0)) != nullptr);
  if (!file_exists) {
    if (!backup_stale_wal(path)) {
      my_error(ER_CANT_CREATE_TABLE, MYF(0), table_arg->s->table_name.str,
               HA_ERR_GENERIC, "Failed to move stale DuckDB WAL");
      return HA_ERR_GENERIC;
    }
  }

  try {
    if (duckdb_instance_pool == nullptr) {
      my_error(ER_CANT_CREATE_TABLE, MYF(0), table_arg->s->table_name.str,
               HA_ERR_GENERIC, "DuckDB instance pool unavailable");
      return HA_ERR_GENERIC;
    }
    std::shared_ptr<duckdb::DuckDB> db;
    std::shared_ptr<std::timed_mutex> writer_mutex;
    std::string pool_error;
    if (!duckdb_instance_pool->acquire(path, &db, &writer_mutex, &pool_error)) {
      my_error(ER_CANT_CREATE_TABLE, MYF(0), table_arg->s->table_name.str,
               HA_ERR_GENERIC,
               pool_error.empty() ? "Failed to acquire DuckDB instance"
                                  : pool_error.c_str());
      return HA_ERR_GENERIC;
    }
    DuckdbWriterGuard writer_guard(writer_mutex);
    if (!writer_guard.locked()) {
      my_error(ER_CANT_CREATE_TABLE, MYF(0), table_arg->s->table_name.str,
               HA_ERR_GENERIC, "DuckDB writer lock unavailable");
      return HA_ERR_GENERIC;
    }
    duckdb::Connection conn(*db);
    std::string schema_name;
    if (table_arg->s->db.str != nullptr && table_arg->s->db.length > 0) {
      schema_name.assign(table_arg->s->db.str, table_arg->s->db.length);
    }
    std::string create_sql = "CREATE TABLE ";
    const std::string table_name(table_arg->s->table_name.str,
                                 table_arg->s->table_name.length);
    create_sql += qualified_table_name(schema_name, table_name);
    create_sql += " (";
    for (uint i = 0; i < table_arg->s->fields; ++i) {
      const Field *field = table_arg->field[i];
      create_sql += quote_ident(field->field_name,
                                std::strlen(field->field_name));
      create_sql += " ";
      const auto mapping = duckdb_type_mapping_for_field(field);
      EmitTypeMappingWarning(table_arg->in_use, table_arg->s->table_name.str,
                             field->field_name, mapping);
      create_sql += mapping.type;
      if (!field->is_nullable()) create_sql += " NOT NULL";
      if (i + 1 < table_arg->s->fields) create_sql += ", ";
    }
    if (table_arg->s->primary_key != MAX_KEY && table_arg->key_info != nullptr) {
      const KEY *primary_key = table_arg->key_info + table_arg->s->primary_key;
      if (primary_key->user_defined_key_parts > 0) {
        create_sql += ", PRIMARY KEY (";
        for (uint i = 0; i < primary_key->user_defined_key_parts; ++i) {
          const Field *key_field = primary_key->key_part[i].field;
          if (key_field == nullptr) continue;
          create_sql +=
              quote_ident(key_field->field_name, std::strlen(key_field->field_name));
          if (i + 1 < primary_key->user_defined_key_parts) create_sql += ", ";
        }
        create_sql += ")";
      }
    }
    create_sql += ")";

    auto result = conn.Query(create_sql);
    if (result->HasError()) {
      const std::string err = result->GetError();
      if (err.find("already exists") != std::string::npos ||
          err.find("ALREADY EXISTS") != std::string::npos) {
        my_error(ER_TABLE_EXISTS_ERROR, MYF(0), table_arg->s->table_name.str);
        return HA_ERR_TABLE_EXIST;
      }
      my_error(ER_CANT_CREATE_TABLE, MYF(0), table_arg->s->table_name.str,
               HA_ERR_GENERIC, err.c_str());
      return HA_ERR_GENERIC;
    }
  } catch (const std::exception &ex) {
    my_error(ER_CANT_CREATE_TABLE, MYF(0), table_arg->s->table_name.str,
             HA_ERR_GENERIC, ex.what());
    return HA_ERR_GENERIC;
  }

  loaded_tables->add(table_arg->s->db.str, table_arg->s->table_name.str, path);
  return 0;
}

int ha_duckdb::rename_table(const char *from, const char *to,
                            const dd::Table *from_table_def,
                            dd::Table *to_table_def) {
  if (from == nullptr || to == nullptr) return HA_ERR_GENERIC;

  std::string from_schema;
  std::string from_table_name;
  std::string to_schema;
  std::string to_table_name;
  if (!parse_table_path(from, &from_schema, &from_table_name) ||
      !parse_table_path(to, &to_schema, &to_table_name)) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Failed to parse table path for DuckDB rename");
    return HA_ERR_GENERIC;
  }
  if (from_schema != to_schema) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "DuckDB rename across schemas is not supported");
    return HA_ERR_WRONG_COMMAND;
  }

  const std::string from_path = resolve_duckdb_path(from_schema, from_table_def);
  const std::string to_path = resolve_duckdb_path(to_schema, to_table_def);
  if (from_path != to_path) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "DuckDB rename across files is not supported");
    return HA_ERR_WRONG_COMMAND;
  }
  if (from_table_name == to_table_name) return 0;

  MY_STAT stat_buf;
  if (my_stat(from_path.c_str(), &stat_buf, MYF(0)) == nullptr) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "DuckDB database file not found");
    return HA_ERR_GENERIC;
  }

  if (duckdb_instance_pool == nullptr) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "DuckDB instance pool unavailable");
    return HA_ERR_GENERIC;
  }

  try {
    std::shared_ptr<duckdb::DuckDB> db;
    std::shared_ptr<std::timed_mutex> writer_mutex;
    std::string pool_error;
    if (!duckdb_instance_pool->acquire(from_path, &db, &writer_mutex,
                                       &pool_error)) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               pool_error.empty() ? "Failed to acquire DuckDB instance"
                                  : pool_error.c_str());
      return HA_ERR_GENERIC;
    }
    DuckdbWriterGuard writer_guard(writer_mutex);
    if (!writer_guard.locked()) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "DuckDB writer lock unavailable");
      return HA_ERR_GENERIC;
    }

    duckdb::Connection conn(*db);
    std::string sql = "ALTER TABLE ";
    sql += qualified_table_name(from_schema, from_table_name);
    sql += " RENAME TO ";
    sql += quote_ident(to_table_name.c_str(), to_table_name.size());
    auto result = conn.Query(sql);
    if (!result || result->HasError()) {
      const std::string err =
          result ? result->GetError() : "DuckDB rename query failed";
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err.c_str());
      return HA_ERR_GENERIC;
    }
  } catch (const std::exception &ex) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ex.what());
    return HA_ERR_GENERIC;
  }

  const bool replicated =
      loaded_tables != nullptr &&
      loaded_tables->is_replicated(from_schema, from_table_name);
  if (loaded_tables != nullptr) {
    loaded_tables->erase(from_schema, from_table_name);
    loaded_tables->add(to_schema, to_table_name, to_path, replicated);
  }
  return 0;
}

int ha_duckdb::delete_table(const char *name, const dd::Table *table_def) {
  if (name == nullptr) return HA_ERR_GENERIC;

  std::string schema_name;
  std::string table_name;
  if (!parse_table_path(name, &schema_name, &table_name)) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Failed to parse table path for DuckDB drop");
    return HA_ERR_GENERIC;
  }

  const std::string path = resolve_duckdb_path(schema_name, table_def);
  MY_STAT stat_buf;
  if (my_stat(path.c_str(), &stat_buf, MYF(0)) == nullptr) {
    if (loaded_tables != nullptr) loaded_tables->erase(schema_name, table_name);
    return 0;
  }

  if (duckdb_instance_pool == nullptr) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "DuckDB instance pool unavailable");
    return HA_ERR_GENERIC;
  }

  try {
    std::shared_ptr<duckdb::DuckDB> db;
    std::shared_ptr<std::timed_mutex> writer_mutex;
    std::string pool_error;
    if (!duckdb_instance_pool->acquire(path, &db, &writer_mutex, &pool_error)) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               pool_error.empty() ? "Failed to acquire DuckDB instance"
                                  : pool_error.c_str());
      return HA_ERR_GENERIC;
    }
    DuckdbWriterGuard writer_guard(writer_mutex);
    if (!writer_guard.locked()) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "DuckDB writer lock unavailable");
      return HA_ERR_GENERIC;
    }

    duckdb::Connection conn(*db);
    std::string sql = "DROP TABLE IF EXISTS ";
    sql += qualified_table_name(schema_name, table_name);
    auto result = conn.Query(sql);
    if (!result || result->HasError()) {
      const std::string err =
          result ? result->GetError() : "DuckDB drop query failed";
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err.c_str());
      return HA_ERR_GENERIC;
    }
  } catch (const std::exception &ex) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ex.what());
    return HA_ERR_GENERIC;
  }

  if (loaded_tables != nullptr) loaded_tables->erase(schema_name, table_name);
  return 0;
}

int ha_duckdb::truncate(dd::Table *) {
  if (table_share != nullptr && table_share->is_secondary_engine()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "DuckDB secondary tables are read-only");
    return HA_ERR_WRONG_COMMAND;
  }
  if (!m_conn || table == nullptr) return HA_ERR_GENERIC;

  MY_STAT stat_buf;
  if (m_table_path.empty() ||
      my_stat(m_table_path.c_str(), &stat_buf, MYF(0)) == nullptr) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "DuckDB database file not found");
    return HA_ERR_GENERIC;
  }

  DuckdbWriterGuard writer_guard(m_writer_mutex);
  if (!writer_guard.locked()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "DuckDB writer lock unavailable");
    return HA_ERR_GENERIC;
  }

  try {
    std::string sql = "TRUNCATE TABLE ";
    sql += qualified_table_name(m_schema_name, m_table_name);
    auto result = m_conn->Query(sql);
    if (!result || result->HasError()) {
      const std::string err =
          result ? result->GetError() : "DuckDB truncate query failed";
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err.c_str());
      return HA_ERR_GENERIC;
    }
  } catch (const std::exception &ex) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ex.what());
    return HA_ERR_GENERIC;
  }

  reset_native_mrr_state();
  invalidate_stats_cache();
  m_result.reset();
  m_chunk.reset();
  m_scan_fields.clear();
  m_chunk_row = 0;
  m_index_result.reset();
  m_index_chunk.reset();
  m_index_fields.clear();
  m_index_chunk_row = 0;
  m_index_descending = false;
  stats.records = 0;
  return 0;
}

int ha_duckdb::open(const char *, int, unsigned int, const dd::Table *) {
  invalidate_stats_cache();
  DuckdbTableState *share =
      loaded_tables->get(table_share->db.str, table_share->table_name.str);
  if (share == nullptr && table_share->is_secondary_engine()) {
    const std::string path = resolve_duckdb_path(table_share);
    MY_STAT stat_buf;
    if (my_stat(path.c_str(), &stat_buf, MYF(0)) != nullptr) {
      std::string schema;
      if (table_share->db.str != nullptr && table_share->db.length > 0) {
        schema.assign(table_share->db.str, table_share->db.length);
      }
      std::string table_name;
      if (table_share->table_name.str != nullptr &&
          table_share->table_name.length > 0) {
        table_name.assign(table_share->table_name.str,
                          table_share->table_name.length);
      }
      if (!table_name.empty()) {
        bool exists = false;
        if (DuckdbTableExistsInFile(path, schema, table_name, &exists) &&
            exists) {
          const bool replicated = !table_share->secondary_load;
          loaded_tables->add(schema, table_name, path, replicated);
          share =
              loaded_tables->get(table_share->db.str, table_share->table_name.str);
        }
      }
    }
  }
  if (share == nullptr && table_share->is_secondary_engine()) {
    std::string reason = build_missing_loaded_tables_reason(current_thd);
    if (reason.empty()) {
      // Include specific table name in error message
      std::ostringstream oss;
      oss << "DuckDB secondary table not loaded: ";
      if (table_share->db.str != nullptr && table_share->db.length > 0) {
        oss << table_share->db.str << ".";
      }
      oss << table_share->table_name.str;
      oss << ". Run ALTER TABLE ... SECONDARY_LOAD";
      reason = oss.str();
    }
    DuckdbSetOffloadFailReason(current_thd, reason.c_str());
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), reason.c_str());
    return HA_ERR_GENERIC;
  }
  if (share == nullptr) {
    const std::string path = resolve_duckdb_path(table_share);
    MY_STAT stat_buf;
    if (my_stat(path.c_str(), &stat_buf, MYF(0)) == nullptr) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "DuckDB database file not found");
      return HA_ERR_GENERIC;
    }
    loaded_tables->add(table_share->db.str, table_share->table_name.str, path);
    share =
        loaded_tables->get(table_share->db.str, table_share->table_name.str);
    if (share == nullptr) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "Failed to register DuckDB table");
      return HA_ERR_GENERIC;
    }
  }
  thr_lock_data_init(&share->lock, &m_lock, nullptr);
  m_table_path = share->path;
  m_schema_name.clear();
  if (table_share->db.str != nullptr && table_share->db.length > 0) {
    m_schema_name.assign(table_share->db.str, table_share->db.length);
  }
  m_table_name = std::string(table_share->table_name.str,
                             table_share->table_name.length);
  ref_length = 0;
  if (const KEY *primary_key = duckdb_primary_key_info(table, table_share);
      primary_key != nullptr) {
    ref_length = primary_key->key_length;
  }

  try {
    if (duckdb_instance_pool == nullptr) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "DuckDB instance pool unavailable");
      return HA_ERR_GENERIC;
    }
    std::string pool_error;
    if (!duckdb_instance_pool->acquire(m_table_path, &m_db, &m_writer_mutex,
                                       &pool_error)) {
      const char *msg = pool_error.empty() ? "Failed to acquire DuckDB instance"
                                           : pool_error.c_str();
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), msg);
      return HA_ERR_GENERIC;
    }
    m_conn = std::make_unique<duckdb::Connection>(*m_db);
  } catch (const std::exception &ex) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ex.what());
    return HA_ERR_GENERIC;
  }

  return 0;
}

int ha_duckdb::close() {
  if (m_in_bulk_insert) end_bulk_insert();
  reset_native_mrr_state();
  invalidate_stats_cache();
  m_result.reset();
  m_chunk.reset();
  m_scan_fields.clear();
  m_index_result.reset();
  m_index_chunk.reset();
  m_index_fields.clear();
  m_pushed_cond_sql.clear();
  m_pushed_idx_cond_sql.clear();
  m_conn.reset();
  m_writer_mutex.reset();
  m_db.reset();
  return 0;
}

int ha_duckdb::rnd_init(bool) {
  DBUG_TRACE;
  m_chunk.reset();
  m_result.reset();
  m_chunk_row = 0;
  m_scan_fields = collect_projection_fields(table);

  if (!m_conn) return HA_ERR_GENERIC;

  try {
    std::string query = "SELECT ";
    query += build_select_list(m_scan_fields);
    query += " FROM ";
    query += qualified_table_name(m_schema_name, m_table_name);
    if (!m_pushed_cond_sql.empty()) {
      query += " WHERE ";
      query += m_pushed_cond_sql;
    }
    m_result = m_conn->Query(query);
    if (!m_result || m_result->HasError()) {
      const std::string err = m_result ? m_result->GetError() :
                                        "DuckDB query failed";
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err.c_str());
      return HA_ERR_GENERIC;
    }
  } catch (const std::exception &ex) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ex.what());
    return HA_ERR_GENERIC;
  }

  return 0;
}

int ha_duckdb::rnd_end() {
  reset_native_mrr_state();
  invalidate_stats_cache();
  m_result.reset();
  m_chunk.reset();
  m_scan_fields.clear();
  m_chunk_row = 0;
  m_index_result.reset();
  m_index_chunk.reset();
  m_index_fields.clear();
  m_index_chunk_row = 0;
  return 0;
}

int ha_duckdb::rnd_next(uchar *) {
  DBUG_TRACE;

  if (!m_result) return HA_ERR_END_OF_FILE;

  try {
    while (true) {
      if (!m_chunk || m_chunk_row >= m_chunk->size()) {
        m_chunk = m_result->Fetch();
        m_chunk_row = 0;
        if (!m_chunk || m_chunk->size() == 0) return HA_ERR_END_OF_FILE;
      }

      if (copy_chunk_row_to_table(table, *m_chunk, m_chunk_row, m_scan_fields) !=
          0) {
        return HA_ERR_GENERIC;
      }

      m_chunk_row++;
      return 0;
    }
  } catch (const std::exception &ex) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ex.what());
    return HA_ERR_GENERIC;
  }
}

int ha_duckdb::index_init(uint idx, bool) {
  const KEY *primary_key = duckdb_primary_key_info(table, table_share);
  if (primary_key == nullptr || idx != table_share->primary_key) {
    return HA_ERR_WRONG_INDEX;
  }
  active_index = idx;
  m_index_result.reset();
  m_index_chunk.reset();
  m_index_fields.clear();
  m_index_chunk_row = 0;
  m_index_descending = false;
  return 0;
}

int ha_duckdb::index_end() {
  reset_native_mrr_state();
  invalidate_stats_cache();
  m_index_result.reset();
  m_index_chunk.reset();
  m_index_fields.clear();
  m_index_chunk_row = 0;
  m_index_descending = false;
  active_index = MAX_KEY;
  return 0;
}

int ha_duckdb::fetch_index_scan_row(uchar *buf) {
  if (!m_index_result || buf == nullptr) return HA_ERR_END_OF_FILE;

  try {
    while (true) {
      if (!m_index_chunk || m_index_chunk_row >= m_index_chunk->size()) {
        m_index_chunk = m_index_result->Fetch();
        m_index_chunk_row = 0;
        if (!m_index_chunk || m_index_chunk->size() == 0) {
          return HA_ERR_END_OF_FILE;
        }
      }

      FieldOffsetGuard guard(table, buf);
      if (copy_chunk_row_to_table(table, *m_index_chunk, m_index_chunk_row,
                                  m_index_fields) != 0) {
        return HA_ERR_GENERIC;
      }

      m_index_chunk_row++;
      return 0;
    }
  } catch (const std::exception &ex) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ex.what());
    return HA_ERR_GENERIC;
  }
}

int ha_duckdb::execute_index_scan(const std::string &where_sql, bool descending,
                                  uchar *buf) {
  if (!m_conn || table == nullptr || active_index == MAX_KEY) {
    return HA_ERR_WRONG_INDEX;
  }
  const KEY *primary_key = duckdb_primary_key_info(table, table_share);
  if (primary_key == nullptr || active_index != table_share->primary_key) {
    return HA_ERR_WRONG_INDEX;
  }

  m_index_fields = collect_projection_fields(table);

  std::string effective_where = where_sql;
  effective_where = combine_predicates(effective_where, m_pushed_cond_sql);
  effective_where = combine_predicates(effective_where, m_pushed_idx_cond_sql);

  std::string sql = "SELECT ";
  sql += build_select_list(m_index_fields);
  sql += " FROM ";
  sql += qualified_table_name(m_schema_name, m_table_name);
  if (!effective_where.empty()) {
    sql += " WHERE ";
    sql += effective_where;
  }
  sql += build_primary_key_order_by(primary_key, descending);

  try {
    m_index_result = m_conn->Query(sql);
    if (!m_index_result || m_index_result->HasError()) {
      const std::string err =
          m_index_result ? m_index_result->GetError() : "DuckDB query failed";
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err.c_str());
      return HA_ERR_GENERIC;
    }
    m_index_chunk.reset();
    m_index_chunk_row = 0;
    m_index_descending = descending;
    return fetch_index_scan_row(buf);
  } catch (const std::exception &ex) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ex.what());
    return HA_ERR_GENERIC;
  }
}

int ha_duckdb::index_read_map(uchar *buf, const uchar *key,
                              key_part_map keypart_map,
                              enum ha_rkey_function find_flag) {
  DBUG_TRACE;
  ha_statistic_increment(&System_status_var::ha_read_key_count);

  if (active_index == MAX_KEY || active_index != table_share->primary_key) {
    return HA_ERR_WRONG_INDEX;
  }
  const KEY *primary_key = duckdb_primary_key_info(table, table_share);
  if (primary_key == nullptr) return HA_ERR_WRONG_INDEX;

  std::string where_sql;
  bool descending = false;

  if (key != nullptr && keypart_map != 0) {
    uint key_parts = 0;
    const uint key_len =
        calculate_key_len(table, active_index, keypart_map, &key_parts);
    if (key_len > 0 && key_parts > 0) {
      key_restore(table->record[0], key, primary_key, key_len);
      const std::vector<Field *> key_fields =
          collect_key_prefix_fields(primary_key, key_parts);
      const std::vector<duckdb::Value> key_values =
          collect_values(table, table->record[0], key_fields);

      switch (find_flag) {
        case HA_READ_KEY_EXACT:
          where_sql = build_where_clause(key_fields, key_values);
          break;
        case HA_READ_KEY_OR_NEXT:
          where_sql =
              build_lexicographic_predicate(key_fields, key_values, true, true);
          break;
        case HA_READ_KEY_OR_PREV:
          where_sql =
              build_lexicographic_predicate(key_fields, key_values, false, true);
          descending = true;
          break;
        case HA_READ_AFTER_KEY:
          where_sql =
              build_lexicographic_predicate(key_fields, key_values, true, false);
          break;
        case HA_READ_BEFORE_KEY:
          where_sql =
              build_lexicographic_predicate(key_fields, key_values, false, false);
          descending = true;
          break;
        case HA_READ_PREFIX:
          where_sql = build_where_clause(key_fields, key_values);
          break;
        case HA_READ_PREFIX_LAST:
          where_sql = build_where_clause(key_fields, key_values);
          descending = true;
          break;
        case HA_READ_PREFIX_LAST_OR_PREV:
          where_sql =
              build_lexicographic_predicate(key_fields, key_values, false, true);
          descending = true;
          break;
        default:
          return HA_ERR_WRONG_COMMAND;
      }
    }
  } else {
    switch (find_flag) {
      case HA_READ_KEY_EXACT:
      case HA_READ_KEY_OR_NEXT:
      case HA_READ_AFTER_KEY:
      case HA_READ_PREFIX:
        descending = false;
        break;
      case HA_READ_KEY_OR_PREV:
      case HA_READ_BEFORE_KEY:
      case HA_READ_PREFIX_LAST:
      case HA_READ_PREFIX_LAST_OR_PREV:
        descending = true;
        break;
      default:
        return HA_ERR_WRONG_COMMAND;
    }
  }

  return execute_index_scan(where_sql, descending, buf);
}

int ha_duckdb::index_read_last_map(uchar *buf, const uchar *key,
                                   key_part_map keypart_map) {
  return index_read_map(buf, key, keypart_map, HA_READ_PREFIX_LAST);
}

int ha_duckdb::index_next(uchar *buf) {
  if (m_index_descending) return HA_ERR_WRONG_COMMAND;
  return fetch_index_scan_row(buf);
}

int ha_duckdb::index_prev(uchar *buf) {
  if (!m_index_descending) return HA_ERR_WRONG_COMMAND;
  return fetch_index_scan_row(buf);
}

int ha_duckdb::index_first(uchar *buf) {
  return execute_index_scan(std::string(), false, buf);
}

int ha_duckdb::index_last(uchar *buf) {
  return execute_index_scan(std::string(), true, buf);
}

bool ha_duckdb::can_use_native_mrr(uint keyno, uint flags) const {
  if (table == nullptr || table_share == nullptr || m_conn == nullptr) {
    return false;
  }
  if (keyno != table_share->primary_key) return false;
  if (flags & (HA_MRR_USE_DEFAULT_IMPL | HA_MRR_SORTED | HA_MRR_NO_ASSOCIATION)) {
    return false;
  }
  const KEY *primary_key = duckdb_primary_key_info(table, table_share);
  if (primary_key == nullptr || primary_key->user_defined_key_parts == 0) {
    return false;
  }
  return true;
}

ha_rows ha_duckdb::multi_range_read_info_const(
    uint keyno, RANGE_SEQ_IF *seq, void *seq_init_param, uint n_ranges,
    uint *bufsz, uint *flags, bool *force_default_mrr, Cost_estimate *cost) {
  const uint requested_flags = *flags;
  ha_rows rows = handler::multi_range_read_info_const(
      keyno, seq, seq_init_param, n_ranges, bufsz, flags, force_default_mrr,
      cost);
  if (rows == HA_POS_ERROR) return rows;
  if (force_default_mrr != nullptr && *force_default_mrr) return rows;
  if (can_use_native_mrr(keyno, requested_flags)) {
    *flags &= ~HA_MRR_USE_DEFAULT_IMPL;
  }
  return rows;
}

ha_rows ha_duckdb::multi_range_read_info(uint keyno, uint n_ranges, uint n_rows,
                                         uint *bufsz, uint *flags,
                                         Cost_estimate *cost) {
  const uint requested_flags = *flags;
  ha_rows rows =
      handler::multi_range_read_info(keyno, n_ranges, n_rows, bufsz, flags, cost);
  if (rows == HA_POS_ERROR) return rows;
  if (can_use_native_mrr(keyno, requested_flags)) {
    *flags &= ~HA_MRR_USE_DEFAULT_IMPL;
  }
  return rows;
}

bool ha_duckdb::native_mrr_range_supported(const KEY *primary_key,
                                           const KEY_MULTI_RANGE &range) const {
  if (primary_key == nullptr) return false;
  if (!(range.range_flag & EQ_RANGE)) return false;
  if (range.range_flag & (NULL_RANGE | GEOM_FLAG)) return false;
  if (range.start_key.key == nullptr || range.end_key.key == nullptr) return false;
  if (range.start_key.flag != HA_READ_KEY_EXACT ||
      range.end_key.flag != HA_READ_AFTER_KEY) {
    return false;
  }
  const key_part_map full_keypart_map =
      make_prev_keypart_map(primary_key->user_defined_key_parts);
  if (range.start_key.keypart_map != full_keypart_map ||
      range.end_key.keypart_map != full_keypart_map) {
    return false;
  }
  if (range.start_key.length != primary_key->key_length ||
      range.end_key.length != primary_key->key_length) {
    return false;
  }
  return true;
}

void ha_duckdb::reset_native_mrr_state() {
  m_native_mrr_active = false;
  m_native_mrr_fallback = true;
  m_native_mrr_mode = 0;
  m_native_mrr_iter = nullptr;
  m_native_mrr_funcs = {};
  m_native_mrr_batch_size = 0;
  m_native_mrr_next_probe = 0;
  m_native_mrr_key_fields.clear();
  m_native_mrr_fields.clear();
  m_native_mrr_probes.clear();
  m_native_mrr_result.reset();
  m_native_mrr_chunk.reset();
  m_native_mrr_chunk_row = 0;
}

int ha_duckdb::start_next_native_mrr_batch() {
  if (!m_native_mrr_active || m_native_mrr_fallback || !m_conn) {
    return HA_ERR_GENERIC;
  }
  if (m_native_mrr_next_probe >= m_native_mrr_probes.size()) {
    return HA_ERR_END_OF_FILE;
  }

  const size_t batch_begin = m_native_mrr_next_probe;
  const size_t batch_end = std::min(batch_begin + m_native_mrr_batch_size,
                                    m_native_mrr_probes.size());
  std::string sql = "SELECT __mrr_probe.";
  sql += quote_ident("__probe_idx", std::strlen("__probe_idx"));
  if (!m_native_mrr_fields.empty()) {
    sql += ", ";
    sql += build_select_list(m_native_mrr_fields);
  }
  sql += " FROM ";
  sql += qualified_table_name(m_schema_name, m_table_name);
  sql += " AS __mrr_tbl JOIN (VALUES ";

  for (size_t i = batch_begin; i < batch_end; ++i) {
    if (i > batch_begin) sql += ", ";
    sql += "(";
    for (size_t key_part = 0; key_part < m_native_mrr_key_fields.size();
         ++key_part) {
      if (key_part > 0) sql += ", ";
      sql += m_native_mrr_probes[i].key_sql_values[key_part];
    }
    if (!m_native_mrr_key_fields.empty()) sql += ", ";
    sql += "'";
    sql += std::to_string(i);
    sql += "')";
  }

  sql += ") AS __mrr_probe(";
  for (size_t key_part = 0; key_part < m_native_mrr_key_fields.size();
       ++key_part) {
    if (key_part > 0) sql += ", ";
    const std::string col_name = "__k" + std::to_string(key_part);
    sql += quote_ident(col_name.c_str(), col_name.size());
  }
  if (!m_native_mrr_key_fields.empty()) sql += ", ";
  sql += quote_ident("__probe_idx", std::strlen("__probe_idx"));
  sql += ") ON ";

  for (size_t key_part = 0; key_part < m_native_mrr_key_fields.size();
       ++key_part) {
    if (key_part > 0) sql += " AND ";
    const std::string probe_col = "__k" + std::to_string(key_part);
    sql += "__mrr_tbl.";
    sql += quote_ident(m_native_mrr_key_fields[key_part]->field_name,
                       std::strlen(m_native_mrr_key_fields[key_part]->field_name));
    sql += " IS NOT DISTINCT FROM __mrr_probe.";
    sql += quote_ident(probe_col.c_str(), probe_col.size());
  }

  std::string pushed_predicate =
      combine_predicates(m_pushed_cond_sql, m_pushed_idx_cond_sql);
  if (!pushed_predicate.empty()) {
    sql += " WHERE ";
    sql += pushed_predicate;
  }
  sql += " ORDER BY __mrr_probe.";
  sql += quote_ident("__probe_idx", std::strlen("__probe_idx"));

  try {
    m_native_mrr_result = m_conn->Query(sql);
    if (!m_native_mrr_result || m_native_mrr_result->HasError()) {
      const std::string err = m_native_mrr_result
                                  ? m_native_mrr_result->GetError()
                                  : "DuckDB query failed";
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err.c_str());
      return HA_ERR_GENERIC;
    }
    m_native_mrr_chunk.reset();
    m_native_mrr_chunk_row = 0;
    m_native_mrr_next_probe = batch_end;
    return 0;
  } catch (const std::exception &ex) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ex.what());
    return HA_ERR_GENERIC;
  }
}

int ha_duckdb::multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                                     uint n_ranges, uint mode,
                                     HANDLER_BUFFER *buf) {
  reset_native_mrr_state();

  if (!can_use_native_mrr(active_index, mode) || seq == nullptr) {
    return handler::multi_range_read_init(seq, seq_init_param, n_ranges, mode,
                                          buf);
  }

  const KEY *primary_key = duckdb_primary_key_info(table, table_share);
  if (primary_key == nullptr) {
    return handler::multi_range_read_init(seq, seq_init_param, n_ranges, mode,
                                          buf);
  }

  m_native_mrr_key_fields = collect_primary_key_fields(table, table_share);
  if (m_native_mrr_key_fields.size() != primary_key->user_defined_key_parts) {
    return handler::multi_range_read_init(seq, seq_init_param, n_ranges, mode,
                                          buf);
  }

  m_native_mrr_funcs = *seq;
  m_native_mrr_iter = seq->init(seq_init_param, n_ranges, mode);
  m_native_mrr_mode = mode;
  m_native_mrr_fields = collect_projection_fields(table);

  KEY_MULTI_RANGE range;
  while (!seq->next(m_native_mrr_iter, &range)) {
    if (!native_mrr_range_supported(primary_key, range)) {
      reset_native_mrr_state();
      return handler::multi_range_read_init(seq, seq_init_param, n_ranges, mode,
                                            buf);
    }

    key_restore(table->record[0], pointer_cast<const uchar *>(range.start_key.key),
                primary_key, primary_key->key_length);
    const std::vector<duckdb::Value> key_values =
        collect_values(table, table->record[0], m_native_mrr_key_fields);
    if (key_values.size() != m_native_mrr_key_fields.size()) {
      reset_native_mrr_state();
      return handler::multi_range_read_init(seq, seq_init_param, n_ranges, mode,
                                            buf);
    }

    NativeMrrProbe probe;
    probe.key_sql_values.reserve(key_values.size());
    for (const duckdb::Value &value : key_values) {
      probe.key_sql_values.push_back(value_to_sql(value));
    }
    probe.range_ptr = range.ptr;
    m_native_mrr_probes.push_back(std::move(probe));
  }

  const size_t default_batch_size = 256;
  m_native_mrr_batch_size = default_batch_size;
  if (buf != nullptr && buf->buffer != nullptr && buf->buffer_end > buf->buffer) {
    const size_t buffer_bytes = buf->buffer_end - buf->buffer;
    const size_t bytes_per_probe =
        std::max<size_t>(primary_key->key_length + sizeof(void *), 1);
    const size_t buffer_limited_batch = buffer_bytes / bytes_per_probe;
    m_native_mrr_batch_size =
        std::max<size_t>(1, std::min(default_batch_size, buffer_limited_batch));
  }
  m_native_mrr_next_probe = 0;
  m_native_mrr_active = true;
  m_native_mrr_fallback = false;
  if (!(mode & HA_MRR_NO_ASSOCIATION)) {
    ha_statistic_increment(&System_status_var::ha_multi_range_read_init_count);
  }
  return 0;
}

int ha_duckdb::multi_range_read_next(char **range_info) {
  if (m_native_mrr_fallback) return handler::multi_range_read_next(range_info);
  if (!m_native_mrr_active) return HA_ERR_END_OF_FILE;

  while (true) {
    if (!m_native_mrr_result) {
      const int batch_result = start_next_native_mrr_batch();
      if (batch_result == HA_ERR_END_OF_FILE) return HA_ERR_END_OF_FILE;
      if (batch_result != 0) return batch_result;
    }

    if (!m_native_mrr_chunk || m_native_mrr_chunk_row >= m_native_mrr_chunk->size()) {
      m_native_mrr_chunk = m_native_mrr_result->Fetch();
      m_native_mrr_chunk_row = 0;
      if (!m_native_mrr_chunk || m_native_mrr_chunk->size() == 0) {
        m_native_mrr_result.reset();
        m_native_mrr_chunk.reset();
        continue;
      }
    }

    size_t probe_idx = 0;
    const duckdb::Value probe_value =
        m_native_mrr_chunk->GetValue(0, m_native_mrr_chunk_row);
    if (!parse_probe_index(probe_value, &probe_idx) ||
        probe_idx >= m_native_mrr_probes.size()) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Invalid DuckDB MRR probe id");
      return HA_ERR_GENERIC;
    }

    char *probe_ptr = m_native_mrr_probes[probe_idx].range_ptr;
    if (m_native_mrr_funcs.skip_record != nullptr &&
        m_native_mrr_funcs.skip_record(m_native_mrr_iter, probe_ptr, nullptr)) {
      m_native_mrr_chunk_row++;
      continue;
    }

    if (copy_chunk_row_to_table(table, *m_native_mrr_chunk, m_native_mrr_chunk_row,
                                m_native_mrr_fields, 1) != 0) {
      return HA_ERR_GENERIC;
    }

    if (range_info != nullptr) {
      *range_info = probe_ptr;
    }
    m_native_mrr_chunk_row++;
    return 0;
  }
}

const Item *ha_duckdb::cond_push(const Item *cond) {
  pushed_cond = nullptr;
  m_pushed_cond_sql.clear();

  if (cond == nullptr || table == nullptr) return cond;

  std::string predicate_sql;
  if (!build_pushdown_predicate(cond, table, &predicate_sql)) return cond;

  pushed_cond = cond;
  m_pushed_cond_sql = predicate_sql;
  return nullptr;
}

Item *ha_duckdb::idx_cond_push(uint keyno, Item *idx_cond) {
  cancel_pushed_idx_cond();

  if (idx_cond == nullptr || table == nullptr) return idx_cond;
  if (table_share == nullptr || keyno != table_share->primary_key) {
    return idx_cond;
  }

  std::string predicate_sql;
  if (!build_pushdown_predicate(idx_cond, table, &predicate_sql)) {
    return idx_cond;
  }

  pushed_idx_cond = idx_cond;
  pushed_idx_cond_keyno = keyno;
  m_pushed_idx_cond_sql = predicate_sql;
  return nullptr;
}

void ha_duckdb::cancel_pushed_idx_cond() {
  handler::cancel_pushed_idx_cond();
  m_pushed_idx_cond_sql.clear();
}

void ha_duckdb::append_row_to_appender(duckdb::Appender &appender,
                                       const uchar *buf) {
  FieldOffsetGuard guard(table, buf);
  const uint field_count = table->s->fields;
  appender.BeginRow();
  for (uint i = 0; i < field_count; ++i) {
    Field *field = table->field[i];
    if (field->is_null()) {
      appender.Append(duckdb::Value());
      continue;
    }
    String tmp;
    field->val_str(&tmp);
    if (is_binary_field(field)) {
      appender.Append(
          duckdb::Value::BLOB_RAW(std::string(tmp.ptr(), tmp.length())));
    } else {
      appender.Append(tmp.ptr(), static_cast<uint32_t>(tmp.length()));
    }
  }
  appender.EndRow();
}

void ha_duckdb::start_bulk_insert(ha_rows) {
  if (table_share->is_secondary_engine()) return;
  if (!m_conn || !m_writer_mutex) return;

  try {
    const auto wait_start = std::chrono::steady_clock::now();
    if (!m_writer_mutex->try_lock()) {
      duckdb_writer_lock_waits.fetch_add(1, std::memory_order_relaxed);
      m_writer_mutex->lock();
      const auto waited_ns = static_cast<ulonglong>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - wait_start)
              .count());
      duckdb_writer_lock_wait_ns.fetch_add(waited_ns,
                                           std::memory_order_relaxed);
    }
    duckdb_writer_lock_acquires.fetch_add(1, std::memory_order_relaxed);
    m_bulk_lock_held = true;

    if (m_schema_name.empty()) {
      m_bulk_appender = std::make_unique<duckdb::Appender>(*m_conn,
                                                           m_table_name);
    } else {
      m_bulk_appender = std::make_unique<duckdb::Appender>(*m_conn, "main",
                                                           m_table_name);
    }
    m_in_bulk_insert = true;
  } catch (const std::exception &) {
    m_bulk_appender.reset();
    if (m_bulk_lock_held) {
      m_writer_mutex->unlock();
      m_bulk_lock_held = false;
    }
    m_in_bulk_insert = false;
    // Fall back to per-row mode silently.
  }
}

int ha_duckdb::end_bulk_insert() {
  if (!m_in_bulk_insert) return 0;

  int ret = 0;
  try {
    if (m_bulk_appender) {
      m_bulk_appender->Close();
    }
  } catch (const std::exception &ex) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ex.what());
    ret = HA_ERR_GENERIC;
  }

  m_bulk_appender.reset();
  if (m_bulk_lock_held && m_writer_mutex) {
    m_writer_mutex->unlock();
    m_bulk_lock_held = false;
  }
  m_in_bulk_insert = false;
  invalidate_stats_cache();
  return ret;
}

int ha_duckdb::write_row(uchar *buf) {
  if (table_share->is_secondary_engine()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "DuckDB secondary tables are read-only");
    return HA_ERR_WRONG_COMMAND;
  }
  ha_statistic_increment(&System_status_var::ha_write_count);
  if (buf == nullptr || table == nullptr || !m_conn) return HA_ERR_GENERIC;

  // Bulk insert fast-path: lock and Appender already held.
  if (m_in_bulk_insert && m_bulk_appender) {
    try {
      append_row_to_appender(*m_bulk_appender, buf);
    } catch (const std::exception &ex) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ex.what());
      return HA_ERR_GENERIC;
    }
    duckdb_bulk_insert_rows.fetch_add(1, std::memory_order_relaxed);
    stats.records++;
    return 0;
  }

  // Per-row path (single-row INSERT, or bulk setup failed).
  DuckdbWriterGuard writer_guard(m_writer_mutex);
  if (!writer_guard.locked()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "DuckDB writer lock unavailable");
    return HA_ERR_GENERIC;
  }

  try {
    if (m_schema_name.empty()) {
      duckdb::Appender appender(*m_conn, m_table_name);
      append_row_to_appender(appender, buf);
      appender.Close();
    } else {
      duckdb::Appender appender(*m_conn, "main", m_table_name);
      append_row_to_appender(appender, buf);
      appender.Close();
    }
  } catch (const std::exception &ex) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ex.what());
    return HA_ERR_GENERIC;
  }

  invalidate_stats_cache();
  stats.records++;
  return 0;
}

int ha_duckdb::update_row(const uchar *old_data, uchar *new_data) {
  if (table_share->is_secondary_engine()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "DuckDB secondary tables are read-only");
    return HA_ERR_WRONG_COMMAND;
  }
  ha_statistic_increment(&System_status_var::ha_update_count);
  if (old_data == nullptr || new_data == nullptr || table == nullptr ||
      !m_conn) {
    return HA_ERR_GENERIC;
  }
  DuckdbWriterGuard writer_guard(m_writer_mutex);
  if (!writer_guard.locked()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "DuckDB writer lock unavailable");
    return HA_ERR_GENERIC;
  }

  const std::vector<Field *> set_fields =
      collect_fields(table, table->write_set);
  const std::vector<Field *> where_fields =
      collect_fields(table, table->read_set);
  if (set_fields.empty() || where_fields.empty()) return HA_ERR_GENERIC;

  std::vector<duckdb::Value> set_values =
      collect_values(table, new_data, set_fields);
  std::vector<duckdb::Value> where_values =
      collect_values(table, old_data, where_fields);

  std::string sql = "UPDATE ";
  sql += qualified_table_name(m_schema_name, m_table_name);
  sql += " SET ";
  sql += build_set_clause(set_fields, set_values);
  sql += " WHERE ";
  sql += build_where_clause(where_fields, where_values);

  try {
    auto result = m_conn->Query(sql);
    if (result->HasError()) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), result->GetError().c_str());
      return HA_ERR_GENERIC;
    }
  } catch (const std::exception &ex) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ex.what());
    return HA_ERR_GENERIC;
  }

  invalidate_stats_cache();
  return 0;
}

int ha_duckdb::delete_row(const uchar *buf) {
  if (table_share->is_secondary_engine()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "DuckDB secondary tables are read-only");
    return HA_ERR_WRONG_COMMAND;
  }
  ha_statistic_increment(&System_status_var::ha_delete_count);
  if (buf == nullptr || table == nullptr || !m_conn) return HA_ERR_GENERIC;
  DuckdbWriterGuard writer_guard(m_writer_mutex);
  if (!writer_guard.locked()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "DuckDB writer lock unavailable");
    return HA_ERR_GENERIC;
  }

  const std::vector<Field *> where_fields =
      collect_fields(table, table->read_set);
  if (where_fields.empty()) return HA_ERR_GENERIC;

  std::vector<duckdb::Value> where_values =
      collect_values(table, buf, where_fields);

  std::string sql = "DELETE FROM ";
  sql += qualified_table_name(m_schema_name, m_table_name);
  sql += " WHERE ";
  sql += build_where_clause(where_fields, where_values);

  try {
    auto result = m_conn->Query(sql);
    if (result->HasError()) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), result->GetError().c_str());
      return HA_ERR_GENERIC;
    }
  } catch (const std::exception &ex) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ex.what());
    return HA_ERR_GENERIC;
  }

  invalidate_stats_cache();
  stats.records = stats.records > 0 ? stats.records - 1 : 0;
  return 0;
}

void ha_duckdb::position(const uchar *record) {
  DBUG_TRACE;
  if (record == nullptr || ref_length == 0) return;
  const KEY *primary_key = duckdb_primary_key_info(table, table_share);
  if (primary_key == nullptr || primary_key->key_length != ref_length) return;
  key_copy(ref, record, primary_key, primary_key->key_length);
}

int ha_duckdb::rnd_pos(uchar *buf, uchar *pos) {
  DBUG_TRACE;
  ha_statistic_increment(&System_status_var::ha_read_rnd_count);

  if (buf == nullptr || pos == nullptr || table == nullptr || !m_conn) {
    return HA_ERR_KEY_NOT_FOUND;
  }

  const KEY *primary_key = duckdb_primary_key_info(table, table_share);
  if (primary_key == nullptr || ref_length == 0) return HA_ERR_KEY_NOT_FOUND;

  std::vector<Field *> where_fields =
      collect_primary_key_fields(table, table_share);
  if (where_fields.empty()) return HA_ERR_KEY_NOT_FOUND;

  key_restore(table->record[0], pos, primary_key, ref_length);
  std::vector<duckdb::Value> where_values =
      collect_values(table, table->record[0], where_fields);
  const std::vector<Field *> projection_fields = collect_projection_fields(table);

  std::string sql = "SELECT ";
  sql += build_select_list(projection_fields);
  sql += " FROM ";
  sql += qualified_table_name(m_schema_name, m_table_name);
  sql += " WHERE ";
  sql += build_where_clause(where_fields, where_values);
  sql += " LIMIT 1";

  try {
    auto result = m_conn->Query(sql);
    if (!result || result->HasError()) {
      const std::string err =
          result ? result->GetError() : "DuckDB query failed";
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err.c_str());
      return HA_ERR_GENERIC;
    }

    auto chunk = result->Fetch();
    if (!chunk || chunk->size() == 0) return HA_ERR_RECORD_DELETED;

    FieldOffsetGuard guard(table, buf);
    if (copy_chunk_row_to_table(table, *chunk, 0, projection_fields) != 0) {
      return HA_ERR_GENERIC;
    }
    return 0;
  } catch (const std::exception &ex) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ex.what());
    return HA_ERR_GENERIC;
  }
}

void ha_duckdb::invalidate_stats_cache() {
  m_stats_cached_query_id = -1;
  m_stats_cache_valid = false;
  m_stats_cached_rows = 0;
  m_stats_cached_pk_domain_valid = false;
  m_stats_cached_pk_integer = false;
  m_stats_cached_pk_min = 0.0L;
  m_stats_cached_pk_max = 0.0L;
}

bool ha_duckdb::ensure_stats_cache() {
  THD *thd = current_thd;
  const longlong query_id =
      thd != nullptr ? static_cast<longlong>(thd->query_id) : -1;
  if (m_stats_cache_valid && query_id != -1 &&
      m_stats_cached_query_id == query_id) {
    return true;
  }

  invalidate_stats_cache();
  m_stats_cached_query_id = query_id;

  handler *primary = ha_get_primary_handler();
  if (primary != nullptr) {
    if (primary->info(HA_STATUS_VARIABLE | HA_STATUS_CONST) == 0) {
      m_stats_cached_rows = primary->stats.records;
      m_stats_cache_valid = true;
    }
  } else if (m_conn != nullptr && table != nullptr && table_share != nullptr) {
    const KEY *primary_key = duckdb_primary_key_info(table, table_share);
    Field *primary_key_field = nullptr;
    bool include_pk_domain = false;
    if (primary_key != nullptr && primary_key->user_defined_key_parts == 1 &&
        primary_key->key_part[0].field != nullptr &&
        is_numeric_field_type(primary_key->key_part[0].field->type())) {
      include_pk_domain = true;
      primary_key_field = primary_key->key_part[0].field;
    }

    std::string sql = "SELECT COUNT(*)";
    if (include_pk_domain) {
      sql += ", MIN(";
      sql += quote_ident(primary_key_field->field_name,
                         std::strlen(primary_key_field->field_name));
      sql += "), MAX(";
      sql += quote_ident(primary_key_field->field_name,
                         std::strlen(primary_key_field->field_name));
      sql += ")";
    }
    sql += " FROM ";
    sql += qualified_table_name(m_schema_name, m_table_name);

    try {
      auto result = m_conn->Query(sql);
      if (result && !result->HasError()) {
        auto chunk = result->Fetch();
        if (chunk && chunk->size() > 0) {
          ha_rows rows = 0;
          if (duckdb_value_to_ha_rows(chunk->GetValue(0, 0), &rows)) {
            m_stats_cached_rows = rows;
            m_stats_cache_valid = true;
            if (include_pk_domain && chunk->ColumnCount() >= 3 &&
                !chunk->GetValue(1, 0).IsNull() && !chunk->GetValue(2, 0).IsNull()) {
              long double min_value = 0.0L;
              long double max_value = 0.0L;
              if (duckdb_value_to_long_double(chunk->GetValue(1, 0), &min_value) &&
                  duckdb_value_to_long_double(chunk->GetValue(2, 0), &max_value) &&
                  max_value >= min_value) {
                m_stats_cached_pk_domain_valid = true;
                m_stats_cached_pk_integer =
                    is_integer_field_type(primary_key_field->type());
                m_stats_cached_pk_min = min_value;
                m_stats_cached_pk_max = max_value;
              }
            }
          }
        }
      }
    } catch (const std::exception &) {
      // Ignore stats refresh errors and keep fallback estimates.
    }
  }

  if (!m_stats_cache_valid) return false;

  stats.records = m_stats_cached_rows;
  stats.mean_rec_length = table_share->reclength;
  const ulonglong rec_len = std::max<ulonglong>(1, table_share->reclength);
  stats.data_file_length = m_stats_cached_rows * rec_len;
  if (const KEY *pk = duckdb_primary_key_info(table, table_share); pk != nullptr) {
    stats.index_file_length =
        m_stats_cached_rows * std::max<ulonglong>(1, pk->key_length);
  } else {
    stats.index_file_length = stats.data_file_length;
  }
  stats.block_size = IO_SIZE;
  return true;
}

bool ha_duckdb::can_estimate_numeric_pk_range(const key_range *min_key,
                                              const key_range *max_key) {
  if (!m_stats_cache_valid || !m_stats_cached_pk_domain_valid ||
      m_stats_cached_rows == 0) {
    return false;
  }
  const KEY *primary_key = duckdb_primary_key_info(table, table_share);
  if (primary_key == nullptr || primary_key->user_defined_key_parts != 1 ||
      primary_key->key_part[0].field == nullptr) {
    return false;
  }
  if (!is_numeric_field_type(primary_key->key_part[0].field->type())) {
    return false;
  }
  if (min_key == nullptr && max_key == nullptr) return false;
  if (min_key != nullptr &&
      (min_key->key == nullptr || min_key->length != primary_key->key_length)) {
    return false;
  }
  if (max_key != nullptr &&
      (max_key->key == nullptr || max_key->length != primary_key->key_length)) {
    return false;
  }
  return true;
}

ha_rows ha_duckdb::estimate_numeric_pk_range_rows(const key_range *min_key,
                                                  const key_range *max_key) {
  const KEY *primary_key = duckdb_primary_key_info(table, table_share);
  if (primary_key == nullptr || primary_key->user_defined_key_parts != 1 ||
      primary_key->key_part[0].field == nullptr || m_stats_cached_rows == 0) {
    return 0;
  }
  Field *primary_key_field = primary_key->key_part[0].field;
  auto decode_key_bound = [&](const key_range *bound, long double *value,
                              bool *inclusive) -> bool {
    if (bound == nullptr || bound->key == nullptr || value == nullptr ||
        inclusive == nullptr || bound->length != primary_key->key_length) {
      return false;
    }
    key_restore(table->record[0], pointer_cast<const uchar *>(bound->key),
                primary_key, primary_key->key_length);
    *value = static_cast<long double>(primary_key_field->val_real());
    *inclusive =
        !(bound->flag == HA_READ_AFTER_KEY || bound->flag == HA_READ_BEFORE_KEY);
    return std::isfinite(*value);
  };

  long double low = m_stats_cached_pk_min;
  long double high = m_stats_cached_pk_max;
  bool low_inclusive = true;
  bool high_inclusive = true;
  long double bound_value = 0.0L;
  if (decode_key_bound(min_key, &bound_value, &low_inclusive)) low = bound_value;
  if (decode_key_bound(max_key, &bound_value, &high_inclusive)) high = bound_value;

  low = std::max(low, m_stats_cached_pk_min);
  high = std::min(high, m_stats_cached_pk_max);

  if (m_stats_cached_pk_integer) {
    if (min_key != nullptr && !low_inclusive) low += 1.0L;
    if (max_key != nullptr && !high_inclusive) high -= 1.0L;
  }

  if (high < low) return 0;

  long double domain_span = m_stats_cached_pk_max - m_stats_cached_pk_min;
  long double range_span = high - low;
  if (m_stats_cached_pk_integer) {
    domain_span += 1.0L;
    range_span += 1.0L;
  }
  if (domain_span <= 0.0L || range_span <= 0.0L) return 0;

  long double fraction = range_span / domain_span;
  fraction = std::max(0.0L, std::min(1.0L, fraction));
  long double estimate = fraction * static_cast<long double>(m_stats_cached_rows);

  if (estimate > 0.0L && estimate < 1.0L) estimate = 1.0L;
  if (estimate > static_cast<long double>(m_stats_cached_rows))
    estimate = static_cast<long double>(m_stats_cached_rows);
  return static_cast<ha_rows>(estimate + 0.5L);
}

int ha_duckdb::info(unsigned int flags) {
  handler *primary = ha_get_primary_handler();
  if (primary != nullptr) {
    int ret = primary->info(flags);
    if (ret == 0) {
      stats.records = primary->stats.records;
      m_stats_cached_rows = primary->stats.records;
      m_stats_cache_valid = true;
      THD *thd = current_thd;
      m_stats_cached_query_id =
          thd != nullptr ? static_cast<longlong>(thd->query_id) : -1;
    }
    return ret;
  }

  if (ensure_stats_cache()) return 0;
  if (stats.records == 0) stats.records = 10;
  return 0;
}

handler::Table_flags ha_duckdb::table_flags() const {
  return 0;
}

double ha_duckdb::scan_time() {
  if (!ensure_stats_cache()) return handler::scan_time();
  if (m_stats_cached_rows == 0) return 1.0;
  return std::max(1.0, rows2double(m_stats_cached_rows) / 4096.0) + 1.0;
}

double ha_duckdb::read_time(uint index, uint ranges, ha_rows rows) {
  if (table_share == nullptr || index != table_share->primary_key) {
    return handler::read_time(index, ranges, rows);
  }
  if (!ensure_stats_cache()) return handler::read_time(index, ranges, rows);

  const double total_rows = std::max(1.0, rows2double(m_stats_cached_rows));
  const double selected_rows = std::max(0.0, rows2double(rows));
  const double selectivity =
      std::max(0.0, std::min(1.0, selected_rows / total_rows));
  const double range_term = std::max(1.0, static_cast<double>(ranges)) * 0.1;
  const double row_term = selected_rows * (0.02 + 0.10 * selectivity);
  return range_term + row_term + 0.5;
}

double ha_duckdb::index_only_read_time(uint keynr, double records) {
  if (table_share == nullptr || keynr != table_share->primary_key) {
    return handler::index_only_read_time(keynr, records);
  }
  if (!ensure_stats_cache()) return handler::index_only_read_time(keynr, records);

  const double total_rows = std::max(1.0, rows2double(m_stats_cached_rows));
  const double selected_rows = std::max(0.0, records);
  const double selectivity =
      std::max(0.0, std::min(1.0, selected_rows / total_rows));
  return selected_rows * (0.01 + 0.05 * selectivity) + 0.25;
}

unsigned long ha_duckdb::index_flags(unsigned int idx, unsigned int part,
                                     bool all_parts) const {
  (void)part;
  (void)all_parts;
  const KEY *primary_key = duckdb_primary_key_info(table, table_share);
  if (primary_key == nullptr || idx != table_share->primary_key) return 0;
  constexpr unsigned long kSupportedFlags =
      HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER | HA_READ_RANGE |
      HA_KEYREAD_ONLY | HA_KEY_SCAN_NOT_ROR | HA_DO_INDEX_COND_PUSHDOWN;
  return kSupportedFlags;
}

ha_rows ha_duckdb::records_in_range(unsigned int index, key_range *min_key,
                                    key_range *max_key) {
  if (index != table_share->primary_key) return HA_POS_ERROR;
  handler *primary = ha_get_primary_handler();
  if (primary != nullptr)
    return primary->records_in_range(index, min_key, max_key);

  if (!ensure_stats_cache()) return stats.records == 0 ? 10 : stats.records;
  if (m_stats_cached_rows == 0) return 0;

  const KEY *primary_key = duckdb_primary_key_info(table, table_share);
  if (primary_key != nullptr && min_key != nullptr && max_key != nullptr &&
      min_key->key != nullptr && max_key->key != nullptr &&
      min_key->length == primary_key->key_length &&
      max_key->length == primary_key->key_length &&
      min_key->flag == HA_READ_KEY_EXACT && max_key->flag == HA_READ_AFTER_KEY &&
      std::memcmp(min_key->key, max_key->key, primary_key->key_length) == 0) {
    return 1;
  }

  if (can_estimate_numeric_pk_range(min_key, max_key)) {
    return estimate_numeric_pk_range_rows(min_key, max_key);
  }

  if (min_key != nullptr && max_key != nullptr) {
    return std::max<ha_rows>(1, m_stats_cached_rows / 8);
  }
  if (min_key != nullptr || max_key != nullptr) {
    return std::max<ha_rows>(1, m_stats_cached_rows / 4);
  }
  return m_stats_cached_rows;
}

int ha_duckdb::external_lock(THD *, int lock_type) {
  DBUG_TRACE;
  if (lock_type == F_UNLCK) {
    reset_native_mrr_state();
    invalidate_stats_cache();
    m_result.reset();
    m_chunk.reset();
    m_scan_fields.clear();
    m_chunk_row = 0;
    m_index_result.reset();
    m_index_chunk.reset();
    m_index_fields.clear();
    m_index_chunk_row = 0;
    m_index_descending = false;
  }
  return 0;
}

int ha_duckdb::reset() {
  if (m_in_bulk_insert) end_bulk_insert();
  reset_native_mrr_state();
  invalidate_stats_cache();
  m_pushed_cond_sql.clear();
  m_pushed_idx_cond_sql.clear();
  return 0;
}

THR_LOCK_DATA **ha_duckdb::store_lock(THD *thd, THR_LOCK_DATA **to,
                                      thr_lock_type lock_type) {
  if (lock_type != TL_IGNORE && m_lock.type == TL_UNLOCK) {
    const bool in_lock_tables = thd != nullptr && thd_in_lock_tables(thd);
    if ((lock_type >= TL_WRITE_CONCURRENT_INSERT && lock_type <= TL_WRITE) &&
        !in_lock_tables) {
      lock_type = TL_WRITE_ALLOW_WRITE;
    }
    if (lock_type == TL_READ_NO_INSERT && !in_lock_tables) {
      lock_type = TL_READ;
    }
    m_lock.type = lock_type;
  }
  *to++ = &m_lock;
  return to;
}

int ha_duckdb::load_table(const TABLE &table) {
  invalidate_stats_cache();
  const std::string db_name =
      (table.s->db.str != nullptr && table.s->db.length > 0)
          ? std::string(table.s->db.str, table.s->db.length)
          : std::string();
  const std::string schema_name = db_name;
  if (loaded_tables != nullptr &&
      loaded_tables->is_replicated(db_name, table.s->table_name.str)) {
    std::ostringstream oss;
    oss << "SECONDARY_LOAD is not supported for binlog-replicated table ";
    if (table.s->db.str != nullptr && table.s->db.length > 0) {
      oss << table.s->db.str << ".";
    }
    oss << table.s->table_name.str
        << "; data is already applied from binlog";
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), oss.str().c_str());
    return HA_ERR_GENERIC;
  }
  const std::string path = resolve_duckdb_path(table.s);
  if (!ensure_duckdb_file(path)) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Failed to create DuckDB file");
    return HA_ERR_GENERIC;
  }

  try {
    if (duckdb_instance_pool == nullptr) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "DuckDB instance pool unavailable");
      return HA_ERR_GENERIC;
    }
    std::shared_ptr<duckdb::DuckDB> db;
    std::shared_ptr<std::timed_mutex> writer_mutex;
    std::string pool_error;
    if (!duckdb_instance_pool->acquire(path, &db, &writer_mutex, &pool_error)) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               pool_error.empty() ? "Failed to acquire DuckDB instance"
                                  : pool_error.c_str());
      return HA_ERR_GENERIC;
    }
    DuckdbWriterGuard writer_guard(writer_mutex);
    if (!writer_guard.locked()) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "DuckDB writer lock unavailable");
      return HA_ERR_GENERIC;
    }
    duckdb::Connection con(*db);
    const std::string table_name(table.s->table_name.str,
                                 table.s->table_name.length);
    const std::string temp_table =
        make_loading_table_name(table_name, table.in_use);
    const std::string quoted_table =
        qualified_table_name(schema_name, table_name);
    const std::string quoted_temp =
        qualified_table_name(schema_name, temp_table);

    const std::string exists_sql =
        "SELECT 1 FROM information_schema.tables WHERE table_schema = 'main' "
        "AND table_name = " +
        value_to_sql(duckdb::Value(table_name)) +
        (schema_name.empty()
             ? std::string()
             : " AND table_catalog = " +
                   value_to_sql(duckdb::Value(schema_name))) +
        " LIMIT 1";
    auto exists_result = con.Query(exists_sql);
    if (exists_result->HasError()) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               exists_result->GetError().c_str());
      return HA_ERR_GENERIC;
    }
    auto exists_chunk = exists_result->Fetch();
    if (exists_chunk && exists_chunk->size() > 0) {
      sql_print_warning(
          "DuckDB secondary load will replace existing table %s in %s",
          table.s->table_name.str, path.c_str());
    }

    std::string create_sql = "CREATE TABLE " + quoted_temp + " (";
    for (uint i = 0; i < table.s->fields; ++i) {
      const Field *field = table.field[i];
      create_sql += quote_ident(field->field_name, strlen(field->field_name));
      create_sql += " ";
      const auto mapping = duckdb_type_mapping_for_field(field);
      EmitTypeMappingWarning(table.in_use, table.s->table_name.str,
                             field->field_name, mapping);
      create_sql += mapping.type;
      if (!field->is_nullable()) create_sql += " NOT NULL";
      if (i + 1 < table.s->fields) create_sql += ", ";
    }
    create_sql += ")";

    auto create_result = con.Query(create_sql);
    if (create_result->HasError()) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               create_result->GetError().c_str());
      return HA_ERR_GENERIC;
    }

    const std::string snapshot_gtid = capture_snapshot_gtid();
    if (!store_snapshot_gtid(con, snapshot_gtid)) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "Failed to store snapshot GTID set");
      return HA_ERR_GENERIC;
    }

    if (schema_name.empty()) {
      duckdb::Appender appender(con, temp_table);
      handler *primary = table.file;
      TABLE &mutable_table = const_cast<TABLE &>(table);
      if (primary->ha_rnd_init(true) != 0) {
        my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
                 "Failed to initialize primary table scan");
        return HA_ERR_GENERIC;
      }
      auto scan_guard = create_scope_guard([&]() { primary->ha_rnd_end(); });

      int error = 0;
      while (!(error = primary->ha_rnd_next(mutable_table.record[0]))) {
        appender.BeginRow();
        for (uint i = 0; i < table.s->fields; ++i) {
          Field *field = mutable_table.field[i];
          appender.Append(field_value(field));
        }
        appender.EndRow();
      }
      if (error != HA_ERR_END_OF_FILE && error != 0) {
        my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
                 "Primary table scan failed");
        return HA_ERR_GENERIC;
      }

      appender.Close();
    } else {
      duckdb::Appender appender(con, "main", temp_table);
      handler *primary = table.file;
      TABLE &mutable_table = const_cast<TABLE &>(table);
      if (primary->ha_rnd_init(true) != 0) {
        my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
                 "Failed to initialize primary table scan");
        return HA_ERR_GENERIC;
      }
      auto scan_guard = create_scope_guard([&]() { primary->ha_rnd_end(); });

      int error = 0;
      while (!(error = primary->ha_rnd_next(mutable_table.record[0]))) {
        appender.BeginRow();
        for (uint i = 0; i < table.s->fields; ++i) {
          Field *field = mutable_table.field[i];
          appender.Append(field_value(field));
        }
        appender.EndRow();
      }
      if (error != HA_ERR_END_OF_FILE && error != 0) {
        my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
                 "Primary table scan failed");
        return HA_ERR_GENERIC;
      }

      appender.Close();
    }

    auto begin_result = con.Query("BEGIN TRANSACTION");
    if (begin_result->HasError()) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               begin_result->GetError().c_str());
      return HA_ERR_GENERIC;
    }
    auto drop_result = con.Query("DROP TABLE IF EXISTS " + quoted_table);
    if (drop_result->HasError()) {
      con.Query("ROLLBACK");
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               drop_result->GetError().c_str());
      return HA_ERR_GENERIC;
    }
    auto rename_result = con.Query(
        "ALTER TABLE " + quoted_temp + " RENAME TO " +
        quote_ident(table_name.c_str(), table_name.size()));
    if (rename_result->HasError()) {
      con.Query("ROLLBACK");
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               rename_result->GetError().c_str());
      return HA_ERR_GENERIC;
    }
    auto commit_result = con.Query("COMMIT");
    if (commit_result->HasError()) {
      con.Query("ROLLBACK");
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               commit_result->GetError().c_str());
      return HA_ERR_GENERIC;
    }
  } catch (const std::exception &ex) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ex.what());
    return HA_ERR_GENERIC;
  }

  loaded_tables->add(table.s->db.str, table.s->table_name.str, path);
  return 0;
}

int ha_duckdb::unload_table(const char *db_name, const char *table_name,
                            bool error_if_not_loaded) {
  invalidate_stats_cache();
  if (error_if_not_loaded &&
      loaded_tables->get(db_name, table_name) == nullptr) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Table is not loaded on DuckDB");
    return 1;
  }
  loaded_tables->erase(db_name, table_name);
  return 0;
}

}  // namespace duckdb_se

static bool duckdb_binlog_apply_enabled = true;
static char *duckdb_binlog_apply_host = nullptr;
static char *duckdb_binlog_apply_user = nullptr;
static char *duckdb_binlog_apply_password = nullptr;
static char *duckdb_binlog_apply_socket = nullptr;
static char *duckdb_binlog_apply_start_gtid = nullptr;
static char *duckdb_binlog_apply_start_file = nullptr;
static ulonglong duckdb_binlog_apply_start_pos = 0;
static char *duckdb_binlog_apply_schema_filter = nullptr;
static ulonglong duckdb_binlog_apply_server_id = 0;
static uint duckdb_binlog_apply_port = 3306;
static bool duckdb_binlog_apply_paused = false;
static ulonglong duckdb_binlog_apply_throttle_rows_per_sec = 0;
static ulonglong duckdb_binlog_apply_throttle_bytes_per_sec = 0;
static ulonglong duckdb_binlog_apply_lag_alert_ms = 0;
static char *duckdb_binlog_apply_stop_at_gtid = nullptr;
static bool duckdb_binlog_apply_verbose = false;
static ulonglong duckdb_binlog_apply_batch_gtids = 100;
static ulonglong duckdb_binlog_apply_batch_rows = 0;
static ulonglong duckdb_binlog_apply_batch_bytes = 0;
static ulonglong duckdb_binlog_apply_batch_delay_ms = 200;
static ulonglong duckdb_binlog_apply_parallel_workers = 1;

static duckdb_se::BinlogApplyThreadOptions duckdb_make_binlog_apply_options() {
  duckdb_se::BinlogApplyThreadOptions options;
  options.enabled = duckdb_binlog_apply_enabled;
  options.host = duckdb_binlog_apply_host ? duckdb_binlog_apply_host
                                          : "127.0.0.1";
  options.user = duckdb_binlog_apply_user ? duckdb_binlog_apply_user : "root";
  options.password = duckdb_binlog_apply_password ? duckdb_binlog_apply_password
                                                  : "";
  options.socket = duckdb_binlog_apply_socket ? duckdb_binlog_apply_socket : "";
  options.port = duckdb_binlog_apply_port;
  options.server_id = static_cast<uint32_t>(duckdb_binlog_apply_server_id);
  options.start_gtid_set =
      duckdb_binlog_apply_start_gtid ? duckdb_binlog_apply_start_gtid : "";
  options.start_file =
      duckdb_binlog_apply_start_file ? duckdb_binlog_apply_start_file : "";
  options.start_position =
      static_cast<uint64_t>(duckdb_binlog_apply_start_pos);
  options.schema_filter = duckdb_binlog_apply_schema_filter
                              ? duckdb_binlog_apply_schema_filter
                              : "";
  options.duckdb_dir = duckdb_db_dir ? duckdb_db_dir : "";
  options.batch_max_gtids =
      static_cast<size_t>(duckdb_binlog_apply_batch_gtids);
  options.batch_max_rows = static_cast<size_t>(duckdb_binlog_apply_batch_rows);
  options.batch_max_bytes =
      static_cast<size_t>(duckdb_binlog_apply_batch_bytes);
  options.batch_max_delay_ms =
      static_cast<uint64_t>(duckdb_binlog_apply_batch_delay_ms);
  options.parallel_workers =
      static_cast<size_t>(duckdb_binlog_apply_parallel_workers);
  return options;
}

static void duckdb_binlog_apply_enabled_update(
    MYSQL_THD, SYS_VAR *, void *var_ptr, const void *save) {
  auto value = *static_cast<const bool *>(save);
  *static_cast<bool *>(var_ptr) = value;
  if (value) {
    duckdb_se::StartBinlogApplyThread(duckdb_make_binlog_apply_options());
  } else {
    duckdb_se::StopBinlogApplyThread();
  }
}

static void duckdb_binlog_apply_paused_update(
    MYSQL_THD, SYS_VAR *, void *var_ptr, const void *save) {
  auto value = *static_cast<const bool *>(save);
  *static_cast<bool *>(var_ptr) = value;
  duckdb_se::SetBinlogApplyPaused(value);
}

static void duckdb_binlog_apply_throttle_rows_update(
    MYSQL_THD, SYS_VAR *, void *var_ptr, const void *save) {
  auto value = *static_cast<const ulonglong *>(save);
  *static_cast<ulonglong *>(var_ptr) = value;
  duckdb_se::SetBinlogApplyThrottleRowsPerSec(value);
}

static void duckdb_binlog_apply_throttle_bytes_update(
    MYSQL_THD, SYS_VAR *, void *var_ptr, const void *save) {
  auto value = *static_cast<const ulonglong *>(save);
  *static_cast<ulonglong *>(var_ptr) = value;
  duckdb_se::SetBinlogApplyThrottleBytesPerSec(value);
}

static void duckdb_binlog_apply_lag_alert_update(
    MYSQL_THD, SYS_VAR *, void *var_ptr, const void *save) {
  auto value = *static_cast<const ulonglong *>(save);
  *static_cast<ulonglong *>(var_ptr) = value;
  duckdb_se::SetBinlogApplyLagAlertThresholdMs(value);
}

static void duckdb_binlog_apply_stop_at_gtid_update(
    MYSQL_THD, SYS_VAR *, void *var_ptr, const void *save) {
  *static_cast<const char **>(var_ptr) =
      *static_cast<const char **>(const_cast<void *>(save));
  const char *value =
      *static_cast<const char **>(const_cast<void *>(save));
  duckdb_se::SetBinlogApplyStopAtGtid(value ? value : "");
}

static void duckdb_binlog_apply_verbose_update(
    MYSQL_THD, SYS_VAR *, void *var_ptr, const void *save) {
  auto value = *static_cast<const bool *>(save);
  *static_cast<bool *>(var_ptr) = value;
  duckdb_se::SetDuckdbBinlogApplyVerbose(value);
}

enum DuckdbExecutionMode : ulong {
  DUCKDB_EXECUTION_MODE_SECONDARY = 0,
  DUCKDB_EXECUTION_MODE_PRIMARY = 1,
  DUCKDB_EXECUTION_MODE_DUAL = 2,
};

static const char *duckdb_execution_mode_names[] = {"SECONDARY", "PRIMARY",
                                                     "DUAL", NullS};
static TYPELIB duckdb_execution_mode_typelib = {
    array_elements(duckdb_execution_mode_names) - 1,
    "duckdb_execution_mode_typelib", duckdb_execution_mode_names, nullptr};

static ulong duckdb_execution_mode = DUCKDB_EXECUTION_MODE_SECONDARY;

static bool DuckdbSecondaryModeEnabled() {
  return duckdb_execution_mode != DUCKDB_EXECUTION_MODE_PRIMARY;
}

static const char *DuckdbExecutionModeToString() {
  switch (duckdb_execution_mode) {
    case DUCKDB_EXECUTION_MODE_PRIMARY:
      return "PRIMARY";
    case DUCKDB_EXECUTION_MODE_DUAL:
      return "DUAL";
    default:
      return "SECONDARY";
  }
}

static void ConfigureDuckdbHandlerton(handlerton *duckdb_hton);

static const char *duckdb_offload_mode_names[] = {"OFF", "ON", "FORCED",
                                                  NullS};
static TYPELIB duckdb_offload_mode_typelib = {
    array_elements(duckdb_offload_mode_names) - 1,
    "duckdb_offload_mode_typelib", duckdb_offload_mode_names, nullptr};

static ulong duckdb_offload_default_mode = 0;
static double duckdb_offload_cost_threshold = 0.0;

static constexpr const char kDuckdbInitConnectStart[] =
    "/*duckdb_offload_default_begin*/";
static constexpr const char kDuckdbInitConnectEnd[] =
    "/*duckdb_offload_default_end*/";

static const char *DuckdbOffloadModeToString(ulong mode) {
  switch (mode) {
    case 1:
      return "ON";
    case 2:
      return "FORCED";
    default:
      return "OFF";
  }
}

static std::string DuckdbBuildInitConnectSnippet() {
  if (!DuckdbSecondaryModeEnabled()) return "";
  if (duckdb_offload_default_mode == 0) return "";
  std::ostringstream oss;
  oss << kDuckdbInitConnectStart << " SET SESSION use_secondary_engine="
      << DuckdbOffloadModeToString(duckdb_offload_default_mode)
      << "; SET SESSION secondary_engine_cost_threshold="
      << std::setprecision(6) << std::fixed << duckdb_offload_cost_threshold
      << "; " << kDuckdbInitConnectEnd;
  return oss.str();
}

static std::string DuckdbStripInitConnectSnippet(const std::string &input) {
  const std::string start{kDuckdbInitConnectStart};
  const std::string end{kDuckdbInitConnectEnd};
  const auto start_pos = input.find(start);
  if (start_pos == std::string::npos) return input;
  auto end_pos = input.find(end, start_pos);
  if (end_pos == std::string::npos) return input;
  end_pos += end.size();
  std::string result = input;
  result.erase(start_pos, end_pos - start_pos);
  return result;
}

static bool duckdb_owns_init_connect = false;

static void DuckdbUpdateInitConnect() {
  mysql_rwlock_wrlock(&LOCK_sys_init_connect);
  std::string current;
  if (opt_init_connect.str && opt_init_connect.length > 0) {
    current.assign(opt_init_connect.str, opt_init_connect.length);
  }

  std::string stripped = DuckdbStripInitConnectSnippet(current);
  std::string snippet = DuckdbBuildInitConnectSnippet();
  std::string updated = stripped;
  if (!snippet.empty()) {
    if (!updated.empty() && updated.back() != ';') {
      updated.append("; ");
    } else if (!updated.empty()) {
      updated.push_back(' ');
    }
    updated.append(snippet);
  }

  if (updated != current) {
    char *new_value = static_cast<char *>(
        my_memdup(key_memory_Sys_var_charptr_value, updated.c_str(),
                  updated.size() + 1, MYF(MY_WME)));
    if (new_value != nullptr) {
      new_value[updated.size()] = 0;
      if (duckdb_owns_init_connect && opt_init_connect.str != nullptr) {
        my_free(opt_init_connect.str);
      }
      opt_init_connect.str = new_value;
      opt_init_connect.length = updated.size();
      duckdb_owns_init_connect = true;
    }
  }

  mysql_rwlock_unlock(&LOCK_sys_init_connect);
}

static void duckdb_offload_default_mode_update(
    MYSQL_THD, SYS_VAR *, void *var_ptr, const void *save) {
  auto value = *static_cast<const ulong *>(save);
  *static_cast<ulong *>(var_ptr) = value;
  DuckdbUpdateInitConnect();
}

static void duckdb_offload_cost_threshold_update(
    MYSQL_THD, SYS_VAR *, void *var_ptr, const void *save) {
  auto value = *static_cast<const double *>(save);
  *static_cast<double *>(var_ptr) = value;
  DuckdbUpdateInitConnect();
}

static void duckdb_execution_mode_update(
    MYSQL_THD, SYS_VAR *, void *var_ptr, const void *save) {
  auto value = *static_cast<const ulong *>(save);
  *static_cast<ulong *>(var_ptr) = value;
  ConfigureDuckdbHandlerton(static_cast<handlerton *>(
      duckdb_se::GetDuckdbPluginPtr()));
  DuckdbUpdateInitConnect();
}

static MYSQL_SYSVAR_BOOL(
    binlog_apply_enabled, duckdb_binlog_apply_enabled, PLUGIN_VAR_RQCMDARG,
    "Enable DuckDB binlog applier thread.",
    nullptr, duckdb_binlog_apply_enabled_update, true);

static MYSQL_SYSVAR_STR(
    db_dir, duckdb_db_dir, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
    "Base directory for per-schema DuckDB files (empty uses @@datadir).",
    nullptr, nullptr, "");

static MYSQL_SYSVAR_STR(
    binlog_apply_host, duckdb_binlog_apply_host,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
    "Source MySQL host for DuckDB binlog apply.",
    nullptr, nullptr, "127.0.0.1");

static MYSQL_SYSVAR_STR(
    binlog_apply_user, duckdb_binlog_apply_user,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
    "Source MySQL user for DuckDB binlog apply.",
    nullptr, nullptr, "root");

static MYSQL_SYSVAR_STR(
    binlog_apply_password, duckdb_binlog_apply_password,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
    "Source MySQL password for DuckDB binlog apply.",
    nullptr, nullptr, "");

static MYSQL_SYSVAR_STR(
    binlog_apply_socket, duckdb_binlog_apply_socket,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
    "Source MySQL socket for DuckDB binlog apply (optional).",
    nullptr, nullptr, "");

static MYSQL_SYSVAR_UINT(
    binlog_apply_port, duckdb_binlog_apply_port, PLUGIN_VAR_RQCMDARG,
    "Source MySQL port for DuckDB binlog apply.",
    nullptr, nullptr, 3306, 0, 65535, 0);

static MYSQL_SYSVAR_ULONGLONG(
    binlog_apply_server_id, duckdb_binlog_apply_server_id, PLUGIN_VAR_RQCMDARG,
    "Replica server_id for DuckDB binlog apply connection (0=auto).",
    nullptr, nullptr, 0, 0, ~0ULL, 0);

static MYSQL_SYSVAR_STR(
    binlog_apply_start_gtid, duckdb_binlog_apply_start_gtid,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
    "GTID set to start DuckDB binlog apply from (empty uses @@GLOBAL.GTID_EXECUTED).",
    nullptr, nullptr, "");

static MYSQL_SYSVAR_STR(
    binlog_apply_start_file, duckdb_binlog_apply_start_file,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
    "Binlog file to start DuckDB binlog apply from (requires start_pos).",
    nullptr, nullptr, "");

static MYSQL_SYSVAR_ULONGLONG(
    binlog_apply_start_pos, duckdb_binlog_apply_start_pos, PLUGIN_VAR_RQCMDARG,
    "Binlog position to start DuckDB binlog apply from when start_file is set.",
    nullptr, nullptr, 0, 0, ~0ULL, 0);

static MYSQL_SYSVAR_STR(
    binlog_apply_schema_filter, duckdb_binlog_apply_schema_filter,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
    "Optional schema filter for DuckDB binlog apply (empty applies all).",
    nullptr, nullptr, "");

static MYSQL_SYSVAR_BOOL(
    binlog_apply_paused, duckdb_binlog_apply_paused, PLUGIN_VAR_RQCMDARG,
    "Pause or resume DuckDB binlog apply.",
    nullptr, duckdb_binlog_apply_paused_update, false);

static MYSQL_SYSVAR_ULONGLONG(
    binlog_apply_throttle_rows_per_sec,
    duckdb_binlog_apply_throttle_rows_per_sec, PLUGIN_VAR_RQCMDARG,
    "Throttle DuckDB binlog apply by rows per second (0=unlimited).",
    nullptr, duckdb_binlog_apply_throttle_rows_update, 0, 0, ~0ULL, 0);

static MYSQL_SYSVAR_ULONGLONG(
    binlog_apply_throttle_bytes_per_sec,
    duckdb_binlog_apply_throttle_bytes_per_sec, PLUGIN_VAR_RQCMDARG,
    "Throttle DuckDB binlog apply by bytes per second (0=unlimited).",
    nullptr, duckdb_binlog_apply_throttle_bytes_update, 0, 0, ~0ULL, 0);

static MYSQL_SYSVAR_ULONGLONG(
    binlog_apply_lag_alert_ms, duckdb_binlog_apply_lag_alert_ms,
    PLUGIN_VAR_RQCMDARG,
    "Raise lag alert when apply lag exceeds this threshold in ms (0=disabled).",
    nullptr, duckdb_binlog_apply_lag_alert_update, 0, 0, ~0ULL, 0);

static MYSQL_SYSVAR_STR(
    binlog_apply_stop_at_gtid, duckdb_binlog_apply_stop_at_gtid,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
    "Pause apply after committing this GTID (empty disables).",
    nullptr, duckdb_binlog_apply_stop_at_gtid_update, "");

static MYSQL_SYSVAR_BOOL(
    binlog_apply_verbose, duckdb_binlog_apply_verbose, PLUGIN_VAR_RQCMDARG,
    "Enable verbose DuckDB binlog apply logging (debug).",
    nullptr, duckdb_binlog_apply_verbose_update, false);

static MYSQL_SYSVAR_ULONGLONG(
    binlog_apply_batch_gtids, duckdb_binlog_apply_batch_gtids,
    PLUGIN_VAR_RQCMDARG,
    "Max GTIDs per DuckDB apply commit (1 disables batching).",
    nullptr, nullptr, 100, 1, ~0ULL, 0);

static MYSQL_SYSVAR_ULONGLONG(
    binlog_apply_batch_rows, duckdb_binlog_apply_batch_rows,
    PLUGIN_VAR_RQCMDARG,
    "Max rows per DuckDB apply commit (0 disables).",
    nullptr, nullptr, 0, 0, ~0ULL, 0);

static MYSQL_SYSVAR_ULONGLONG(
    binlog_apply_batch_bytes, duckdb_binlog_apply_batch_bytes,
    PLUGIN_VAR_RQCMDARG,
    "Max bytes per DuckDB apply commit (0 disables).",
    nullptr, nullptr, 0, 0, ~0ULL, 0);

static MYSQL_SYSVAR_ULONGLONG(
    binlog_apply_batch_delay_ms, duckdb_binlog_apply_batch_delay_ms,
    PLUGIN_VAR_RQCMDARG,
    "Max delay in ms before forcing a DuckDB apply commit (0 disables).",
    nullptr, nullptr, 200, 0, ~0ULL, 0);

static MYSQL_SYSVAR_ULONGLONG(
    binlog_apply_parallel_workers, duckdb_binlog_apply_parallel_workers,
    PLUGIN_VAR_RQCMDARG,
    "Parallel apply workers per schema (1=disabled).",
    nullptr, nullptr, 1, 1, 64, 0);

static MYSQL_SYSVAR_ENUM(
    offload_default_mode, duckdb_offload_default_mode, PLUGIN_VAR_RQCMDARG,
    "Default offload mode for new sessions. Updates init_connect to set "
    "use_secondary_engine (OFF/ON/FORCED).",
    nullptr, duckdb_offload_default_mode_update, 0,
    &duckdb_offload_mode_typelib);

static MYSQL_SYSVAR_DOUBLE(
    offload_cost_threshold, duckdb_offload_cost_threshold, PLUGIN_VAR_RQCMDARG,
    "Secondary engine cost threshold to apply via init_connect (0 for tests).",
    nullptr, duckdb_offload_cost_threshold_update, 0.0, 0.0, DBL_MAX, 0);

static MYSQL_SYSVAR_ENUM(
    execution_mode, duckdb_execution_mode, PLUGIN_VAR_RQCMDARG,
    "DuckDB execution mode (SECONDARY/PRIMARY/DUAL). "
    "SECONDARY keeps current offload behavior, PRIMARY disables offload "
    "callbacks, DUAL enables both in one build.",
    nullptr, duckdb_execution_mode_update, DUCKDB_EXECUTION_MODE_SECONDARY,
    &duckdb_execution_mode_typelib);

static SYS_VAR *duckdb_system_variables[] = {
    MYSQL_SYSVAR(db_dir),
    MYSQL_SYSVAR(execution_mode),
    MYSQL_SYSVAR(offload_default_mode),
    MYSQL_SYSVAR(offload_cost_threshold),
    MYSQL_SYSVAR(binlog_apply_enabled),
    MYSQL_SYSVAR(binlog_apply_host),
    MYSQL_SYSVAR(binlog_apply_user),
    MYSQL_SYSVAR(binlog_apply_password),
    MYSQL_SYSVAR(binlog_apply_socket),
    MYSQL_SYSVAR(binlog_apply_port),
    MYSQL_SYSVAR(binlog_apply_server_id),
    MYSQL_SYSVAR(binlog_apply_start_gtid),
    MYSQL_SYSVAR(binlog_apply_start_file),
    MYSQL_SYSVAR(binlog_apply_start_pos),
    MYSQL_SYSVAR(binlog_apply_schema_filter),
    MYSQL_SYSVAR(binlog_apply_paused),
    MYSQL_SYSVAR(binlog_apply_throttle_rows_per_sec),
    MYSQL_SYSVAR(binlog_apply_throttle_bytes_per_sec),
    MYSQL_SYSVAR(binlog_apply_lag_alert_ms),
    MYSQL_SYSVAR(binlog_apply_stop_at_gtid),
    MYSQL_SYSVAR(binlog_apply_verbose),
    MYSQL_SYSVAR(binlog_apply_batch_gtids),
    MYSQL_SYSVAR(binlog_apply_batch_rows),
    MYSQL_SYSVAR(binlog_apply_batch_bytes),
    MYSQL_SYSVAR(binlog_apply_batch_delay_ms),
    MYSQL_SYSVAR(binlog_apply_parallel_workers),
    nullptr};

static int show_duckdb_execution_mode(MYSQL_THD, SHOW_VAR *var, char *buf) {
  var->type = SHOW_CHAR;
  var->value = buf;
  std::snprintf(buf, SHOW_VAR_FUNC_BUFF_SIZE, "%s",
                DuckdbExecutionModeToString());
  return 0;
}

static int show_duckdb_instance_pool_hits(MYSQL_THD, SHOW_VAR *var, char *) {
  static ulonglong value;
  value = duckdb_instance_pool_hits.load(std::memory_order_relaxed);
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_instance_pool_misses(MYSQL_THD, SHOW_VAR *var, char *) {
  static ulonglong value;
  value = duckdb_instance_pool_misses.load(std::memory_order_relaxed);
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_writer_lock_acquires(MYSQL_THD, SHOW_VAR *var, char *) {
  static ulonglong value;
  value = duckdb_writer_lock_acquires.load(std::memory_order_relaxed);
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_writer_lock_waits(MYSQL_THD, SHOW_VAR *var, char *) {
  static ulonglong value;
  value = duckdb_writer_lock_waits.load(std::memory_order_relaxed);
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_writer_lock_wait_ns(MYSQL_THD, SHOW_VAR *var, char *) {
  static ulonglong value;
  value = duckdb_writer_lock_wait_ns.load(std::memory_order_relaxed);
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_bulk_insert_rows(MYSQL_THD, SHOW_VAR *var, char *) {
  static ulonglong value;
  value = duckdb_bulk_insert_rows.load(std::memory_order_relaxed);
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_conversion_rows(MYSQL_THD, SHOW_VAR *var, char *) {
  static ulonglong value;
  value = duckdb_conversion_rows.load(std::memory_order_relaxed);
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_conversion_values(MYSQL_THD, SHOW_VAR *var, char *) {
  static ulonglong value;
  value = duckdb_conversion_values.load(std::memory_order_relaxed);
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_conversion_ns(MYSQL_THD, SHOW_VAR *var, char *) {
  static ulonglong value;
  value = duckdb_conversion_ns.load(std::memory_order_relaxed);
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_binlog_apply_paused(MYSQL_THD, SHOW_VAR *var, char *) {
  static bool value;
  value = duckdb_se::GetBinlogApplyControls().paused;
  var->type = SHOW_BOOL;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_binlog_applied_transactions(MYSQL_THD, SHOW_VAR *var,
                                                   char *) {
  static ulonglong value;
  value = duckdb_se::GetBinlogApplyMetrics().applied_transactions;
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_binlog_applied_rows(MYSQL_THD, SHOW_VAR *var, char *) {
  static ulonglong value;
  value = duckdb_se::GetBinlogApplyMetrics().applied_rows;
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_binlog_applied_bytes(MYSQL_THD, SHOW_VAR *var, char *) {
  static ulonglong value;
  value = duckdb_se::GetBinlogApplyMetrics().applied_bytes;
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_binlog_last_flush_rows(MYSQL_THD, SHOW_VAR *var,
                                              char *) {
  static ulonglong value;
  value = duckdb_se::GetBinlogApplyMetrics().last_flush_rows;
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_binlog_last_flush_bytes(MYSQL_THD, SHOW_VAR *var,
                                               char *) {
  static ulonglong value;
  value = duckdb_se::GetBinlogApplyMetrics().last_flush_bytes;
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_binlog_last_flush_ms(MYSQL_THD, SHOW_VAR *var, char *) {
  static ulonglong value;
  value = duckdb_se::GetBinlogApplyMetrics().last_flush_ms;
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_binlog_last_batch_size(MYSQL_THD, SHOW_VAR *var,
                                              char *) {
  static ulonglong value;
  value = duckdb_se::GetBinlogApplyMetrics().last_batch_size;
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_binlog_last_stage_ms(MYSQL_THD, SHOW_VAR *var, char *) {
  static ulonglong value;
  value = duckdb_se::GetBinlogApplyMetrics().last_stage_ms;
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_binlog_last_merge_delete_ms(MYSQL_THD, SHOW_VAR *var,
                                                   char *) {
  static ulonglong value;
  value = duckdb_se::GetBinlogApplyMetrics().last_merge_delete_ms;
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_binlog_last_apply_total_ms(MYSQL_THD, SHOW_VAR *var,
                                                  char *) {
  static ulonglong value;
  value = duckdb_se::GetBinlogApplyMetrics().last_apply_total_ms;
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_binlog_last_throttle_ms(MYSQL_THD, SHOW_VAR *var,
                                               char *) {
  static ulonglong value;
  value = duckdb_se::GetBinlogApplyMetrics().last_throttle_ms;
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_binlog_total_throttle_ms(MYSQL_THD, SHOW_VAR *var,
                                                char *) {
  static ulonglong value;
  value = duckdb_se::GetBinlogApplyMetrics().total_throttle_ms;
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_binlog_last_commit_epoch_ms(MYSQL_THD, SHOW_VAR *var,
                                                   char *) {
  static ulonglong value;
  value = duckdb_se::GetBinlogApplyMetrics().last_commit_epoch_ms;
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_binlog_last_commit_ms(MYSQL_THD, SHOW_VAR *var, char *) {
  static ulonglong value;
  value = duckdb_se::GetBinlogApplyMetrics().last_commit_ms;
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_binlog_rows_per_sec(MYSQL_THD, SHOW_VAR *var, char *) {
  static double value;
  const auto metrics = duckdb_se::GetBinlogApplyMetrics();
  if (metrics.last_flush_ms > 0) {
    value = static_cast<double>(metrics.last_flush_rows) * 1000.0 /
            static_cast<double>(metrics.last_flush_ms);
  } else {
    value = 0.0;
  }
  var->type = SHOW_DOUBLE;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_binlog_lag_ms(MYSQL_THD, SHOW_VAR *var, char *) {
  static ulonglong value;
  value = duckdb_se::GetBinlogApplyMetrics().lag_ms;
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_binlog_lag_seconds(MYSQL_THD, SHOW_VAR *var, char *) {
  static double value;
  const auto metrics = duckdb_se::GetBinlogApplyMetrics();
  value = static_cast<double>(metrics.lag_ms) / 1000.0;
  var->type = SHOW_DOUBLE;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_binlog_lag_alert(MYSQL_THD, SHOW_VAR *var, char *) {
  static bool value;
  value = duckdb_se::GetBinlogApplyMetrics().lag_alert;
  var->type = SHOW_BOOL;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_binlog_gtid_lag(MYSQL_THD, SHOW_VAR *var, char *) {
  static ulonglong value;
  value = duckdb_se::GetBinlogApplyMetrics().gtid_lag;
  var->type = SHOW_LONGLONG;
  var->value = reinterpret_cast<char *>(&value);
  return 0;
}

static int show_duckdb_binlog_last_gtid(MYSQL_THD, SHOW_VAR *var, char *buf) {
  var->type = SHOW_CHAR;
  var->value = buf;
  const auto metrics = duckdb_se::GetBinlogApplyMetrics();
  const char *gtid = metrics.last_gtid.empty() ? "" : metrics.last_gtid.c_str();
  std::snprintf(buf, SHOW_VAR_FUNC_BUFF_SIZE, "%s", gtid);
  return 0;
}

static int show_duckdb_binlog_source_gtid(MYSQL_THD, SHOW_VAR *var, char *buf) {
  var->type = SHOW_CHAR;
  var->value = buf;
  const auto metrics = duckdb_se::GetBinlogApplyMetrics();
  const char *gtid_set =
      metrics.source_gtid_set.empty() ? "" : metrics.source_gtid_set.c_str();
  std::snprintf(buf, SHOW_VAR_FUNC_BUFF_SIZE, "%s", gtid_set);
  return 0;
}

static SHOW_VAR duckdb_status_variables[] = {
    {"duckdb_execution_mode_state", (char *)show_duckdb_execution_mode,
     SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_instance_pool_hits", (char *)show_duckdb_instance_pool_hits,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_instance_pool_misses", (char *)show_duckdb_instance_pool_misses,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_writer_lock_acquires", (char *)show_duckdb_writer_lock_acquires,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_writer_lock_waits", (char *)show_duckdb_writer_lock_waits,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_writer_lock_wait_ns", (char *)show_duckdb_writer_lock_wait_ns,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_bulk_insert_rows", (char *)show_duckdb_bulk_insert_rows,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_conversion_rows", (char *)show_duckdb_conversion_rows, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_conversion_values", (char *)show_duckdb_conversion_values,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_conversion_ns", (char *)show_duckdb_conversion_ns, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_binlog_apply_paused_state",
     (char *)show_duckdb_binlog_apply_paused, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_binlog_applied_transactions",
     (char *)show_duckdb_binlog_applied_transactions, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_binlog_applied_rows", (char *)show_duckdb_binlog_applied_rows,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_binlog_applied_bytes", (char *)show_duckdb_binlog_applied_bytes,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_binlog_last_flush_rows",
     (char *)show_duckdb_binlog_last_flush_rows, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_binlog_last_flush_bytes",
     (char *)show_duckdb_binlog_last_flush_bytes, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_binlog_last_flush_ms", (char *)show_duckdb_binlog_last_flush_ms,
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_binlog_last_batch_size",
     (char *)show_duckdb_binlog_last_batch_size, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_binlog_last_stage_ms",
     (char *)show_duckdb_binlog_last_stage_ms, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_binlog_last_merge_delete_ms",
     (char *)show_duckdb_binlog_last_merge_delete_ms, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_binlog_last_apply_total_ms",
     (char *)show_duckdb_binlog_last_apply_total_ms, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_binlog_last_throttle_ms",
     (char *)show_duckdb_binlog_last_throttle_ms, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_binlog_total_throttle_ms",
     (char *)show_duckdb_binlog_total_throttle_ms, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_binlog_last_commit_epoch_ms",
     (char *)show_duckdb_binlog_last_commit_epoch_ms, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_binlog_last_commit_ms",
     (char *)show_duckdb_binlog_last_commit_ms, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_binlog_rows_per_sec",
     (char *)show_duckdb_binlog_rows_per_sec, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_binlog_lag_ms", (char *)show_duckdb_binlog_lag_ms, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_binlog_lag_seconds",
     (char *)show_duckdb_binlog_lag_seconds, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_binlog_lag_alert", (char *)show_duckdb_binlog_lag_alert, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_binlog_gtid_lag", (char *)show_duckdb_binlog_gtid_lag, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_binlog_last_gtid", (char *)show_duckdb_binlog_last_gtid, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_binlog_source_gtid",
     (char *)show_duckdb_binlog_source_gtid, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_UNDEF, SHOW_SCOPE_GLOBAL}};

static handler *duckdb_create_handler(handlerton *hton, TABLE_SHARE *table,
                                      bool, MEM_ROOT *mem_root) {
  return new (mem_root) duckdb_se::ha_duckdb(hton, table);
}

static void ConfigureDuckdbHandlerton(handlerton *duckdb_hton) {
  if (duckdb_hton == nullptr) return;
  // Routing policy:
  // PRIMARY -> disable secondary/offload callbacks.
  // SECONDARY/DUAL -> keep existing offload callbacks enabled.
  duckdb_hton->create = duckdb_create_handler;
  duckdb_hton->state = SHOW_OPTION_YES;
  duckdb_hton->db_type = DB_TYPE_UNKNOWN;
  duckdb_hton->prepare_secondary_engine = nullptr;
  duckdb_hton->optimize_secondary_engine = nullptr;
  duckdb_hton->get_secondary_engine_offload_or_exec_fail_reason = nullptr;
  duckdb_hton->set_secondary_engine_offload_fail_reason = nullptr;
  duckdb_hton->secondary_engine_flags = {};
  duckdb_hton->flags = HTON_NO_PARTITION;

  if (!DuckdbSecondaryModeEnabled()) {
    return;
  }

  duckdb_hton->flags = HTON_IS_SECONDARY_ENGINE;
  duckdb_hton->prepare_secondary_engine = PrepareSecondaryEngine;
  duckdb_hton->optimize_secondary_engine = OptimizeSecondaryEngine;
  duckdb_hton->get_secondary_engine_offload_or_exec_fail_reason =
      DuckdbGetOffloadFailReason;
  duckdb_hton->set_secondary_engine_offload_fail_reason =
      DuckdbSetOffloadFailReason;
  // Use the external executor path so join queries can run inside DuckDB.
  duckdb_hton->secondary_engine_flags = MakeSecondaryEngineFlags(
      SecondaryEngineFlag::SUPPORTS_HASH_JOIN,
      SecondaryEngineFlag::SUPPORTS_NESTED_LOOP_JOIN,
      SecondaryEngineFlag::AGGREGATION_IS_UNORDERED,
      SecondaryEngineFlag::USE_EXTERNAL_EXECUTOR);
}

static int duckdb_init_func(void *p) {
  DBUG_TRACE;

  loaded_tables = new LoadedTables();
  duckdb_instance_pool = new DuckdbInstancePool();
  duckdb_se::SetDuckdbPluginPtr(p);
  duckdb_se::SetBinlogApplyPaused(duckdb_binlog_apply_paused);
  duckdb_se::SetBinlogApplyThrottleRowsPerSec(
      duckdb_binlog_apply_throttle_rows_per_sec);
  duckdb_se::SetBinlogApplyThrottleBytesPerSec(
      duckdb_binlog_apply_throttle_bytes_per_sec);
  duckdb_se::SetBinlogApplyLagAlertThresholdMs(
      duckdb_binlog_apply_lag_alert_ms);
  duckdb_se::SetBinlogApplyStopAtGtid(
      duckdb_binlog_apply_stop_at_gtid ? duckdb_binlog_apply_stop_at_gtid : "");
  duckdb_se::SetDuckdbBinlogApplyVerbose(duckdb_binlog_apply_verbose);
  DuckdbUpdateInitConnect();
  (void)duckdb_validate_read_only_settings();
  duckdb_se::StartBinlogApplyThread(duckdb_make_binlog_apply_options());
  ConfigureDuckdbHandlerton(static_cast<handlerton *>(p));
  return 0;
}

static int duckdb_deinit_func(void *) {
  DBUG_TRACE;

  duckdb_se::StopBinlogApplyThread();
  duckdb_se::SetDuckdbPluginPtr(nullptr);
  delete duckdb_instance_pool;
  duckdb_instance_pool = nullptr;
  delete loaded_tables;
  loaded_tables = nullptr;
  return 0;
}

struct st_mysql_storage_engine duckdb_storage_engine = {
    MYSQL_HANDLERTON_INTERFACE_VERSION};

mysql_declare_plugin(duckdb_se){
    MYSQL_STORAGE_ENGINE_PLUGIN,
    &duckdb_storage_engine,
    "DUCKDB",
    "Percona Inc.",
    "DuckDB storage engine (primary + secondary offload modes)",
    PLUGIN_LICENSE_GPL,
    duckdb_init_func,   /* Plugin Init */
    nullptr,            /* Plugin check uninstall */
    duckdb_deinit_func, /* Plugin Deinit */
    0x0001 /* 0.1 */,
    duckdb_status_variables, /* status variables */
    duckdb_system_variables, /* system variables */
    nullptr, /* config options */
    0,       /* flags */
} mysql_declare_plugin_end;
