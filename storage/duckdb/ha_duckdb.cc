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

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/common/types/value.hpp"
#include "my_bitmap.h"
#include "my_dbug.h"
#include "my_sys.h"
#include "mysqld_error.h"
#include "sql_error.h"
#include "sql/field.h"
#include "mysql/plugin.h"
#include "sql/mysqld.h"
#include "sql/query_result.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"
#include "sql/sql_thd_internal_api.h"
#include "sql/visible_fields.h"
#include "scope_guard.h"
#include "storage/duckdb/duckdb_binlog_applier.h"
#include "storage/duckdb/duckdb_compat.h"
#include "sql/field.h"
#include "sql/table.h"
#include "sql_string.h"
#include "template_utils.h"
#include "thr_lock.h"

namespace {

struct DuckdbTableState {
  explicit DuckdbTableState(std::string path_in) : path(std::move(path_in)) {
    thr_lock_init(&lock);
  }

  ~DuckdbTableState() { thr_lock_delete(&lock); }

  DuckdbTableState(const DuckdbTableState &) = delete;
  DuckdbTableState &operator=(const DuckdbTableState &) = delete;

  THR_LOCK lock;
  std::string path;
};

class LoadedTables {
  std::map<std::pair<std::string, std::string>,
           std::unique_ptr<DuckdbTableState>>
      m_tables;
  std::mutex m_mutex;

 public:
  void add(const std::string &db, const std::string &table,
           const std::string &path) {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_tables[{db, table}] = std::make_unique<DuckdbTableState>(path);
  }

  DuckdbTableState *get(const std::string &db, const std::string &table) {
    std::lock_guard<std::mutex> guard(m_mutex);
    auto it = m_tables.find(std::make_pair(db, table));
    return it == m_tables.end() ? nullptr : it->second.get();
  }

  void erase(const std::string &db, const std::string &table) {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_tables.erase(std::make_pair(db, table));
  }
};

LoadedTables *loaded_tables{nullptr};

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
    default:
      break;
  }
  return field->binary();
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
  const bool unsigned_flag = field->unsigned_flag;
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

std::string duckdb_type_for_field(const Field *field) {
  return duckdb_type_mapping_for_field(field).type;
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

  String tmp;
  field->val_str(&tmp);
  if (is_binary_field(field)) {
    return duckdb::Value::BLOB_RAW(std::string(tmp.ptr(), tmp.length()));
  }
  return duckdb::Value(std::string(tmp.ptr(), tmp.length()));
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

std::string default_duckdb_path(const TABLE_SHARE *share) {
  std::string dir = mysql_real_data_home;
  if (!dir.empty() && dir.back() != FN_LIBCHAR) dir.push_back(FN_LIBCHAR);
  dir.append(share->db.str, share->db.length);
  dir.push_back(FN_LIBCHAR);
  std::string file;
  file.append(share->table_name.str, share->table_name.length);
  file.append(".duckdb");
  return dir + file;
}

std::string resolve_duckdb_path(const TABLE_SHARE *share) {
  if (share->secondary_engine_attribute.length > 0) {
    return std::string(share->secondary_engine_attribute.str,
                       share->secondary_engine_attribute.length);
  }
  return default_duckdb_path(share);
}

bool ensure_duckdb_file(const std::string &path) {
  // Check if directory exists; DuckDB will create the file itself
  MY_STAT stat_buf;
  std::string dir = path.substr(0, path.rfind(FN_LIBCHAR));
  if (my_stat(dir.c_str(), &stat_buf, MYF(0)) == nullptr) {
    return false;  // Directory doesn't exist
  }
  // Remove existing database file and WAL file - DuckDB needs to create fresh
  // This handles cases where an empty/corrupt file was left behind, or
  // a WAL file from a previous failed attempt exists
  my_delete(path.c_str(), MYF(0));
  my_delete((path + ".wal").c_str(), MYF(0));
  return true;
}

bool is_ident_char(char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= 'a' && ch <= 'z') || ch == '_';
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

std::string rewrite_qualified_table(std::string sql, const std::string &db,
                                    const std::string &table) {
  if (db.empty() || table.empty()) return sql;

  const std::string quoted_table = quote_ident(table.c_str(), table.size());
  const std::string quoted_db = quote_ident(db.c_str(), db.size());
  const std::string quoted_pattern = quoted_db + "." + quoted_table;

  std::string out;
  out.reserve(sql.size());
  const size_t len = sql.size();
  for (size_t i = 0; i < len;) {
    const char ch = sql[i];
    if (ch == '\'') {
      out.push_back(ch);
      ++i;
      while (i < len) {
        out.push_back(sql[i]);
        if (sql[i] == '\'') {
          ++i;
          if (i < len && sql[i] == '\'') {
            out.push_back(sql[i]);
            ++i;
            continue;
          }
          break;
        }
        ++i;
      }
      continue;
    }

    if (match_ci(sql, i, quoted_pattern)) {
      out.append(quoted_table);
      i += quoted_pattern.size();
      continue;
    }

    if (match_ci(sql, i, db)) {
      const size_t db_end = i + db.size();
      if (db_end < len && sql[db_end] == '.' &&
          match_ci(sql, db_end + 1, table)) {
        const size_t table_end = db_end + 1 + table.size();
        const bool left_ok = (i == 0) || !is_ident_char(sql[i - 1]);
        const bool right_ok =
            (table_end >= len) || !is_ident_char(sql[table_end]);
        if (left_ok && right_ok) {
          out.append(table);
          i = table_end;
          continue;
        }
      }
    }

    out.push_back(ch);
    ++i;
  }
  return out;
}

bool is_simple_select(LEX *lex, Table_ref **base_table,
                      std::string *reason) {
  if (lex == nullptr || base_table == nullptr) return false;
  *base_table = nullptr;

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
    *base_table = tl;
    ++base_count;
  }

  if (base_count != 1) {
    if (reason) *reason = "Only single-table SELECT is supported";
    return false;
  }

  return true;
}

bool uses_only_field_items(const mem_root_deque<Item *> &fields,
                           TABLE *base_table, std::string *reason) {
  for (Item *item : VisibleFields(fields)) {
    Item *real = item->real_item();
    if (real->type() != Item::FIELD_ITEM) {
      if (reason) *reason = "Only direct column projections are supported";
      return false;
    }
    auto *field_item = down_cast<Item_field *>(real);
    if (field_item->field == nullptr ||
        field_item->field->table != base_table) {
      if (reason) *reason = "Columns from a single base table are required";
      return false;
    }
  }
  return true;
}

static bool DuckdbExecuteQuery(JOIN *join, Query_result *query_result) {
  if (join == nullptr || query_result == nullptr) return true;
  THD *thd = join->thd;
  if (join->fields == nullptr) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "DuckDB select list not available");
    return true;
  }
  auto *ctx = down_cast<Duckdb_execution_context *>(
      thd->lex->secondary_engine_execution_context());
  if (ctx == nullptr) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "DuckDB execution context missing");
    return true;
  }

  try {
    duckdb::DBConfig config(true);
    duckdb::DuckDB db(ctx->db_path, &config);
    duckdb::Connection conn(db);
    auto result = conn.SendQuery(ctx->sql);
    if (!result || result->HasError()) {
      const std::string err =
          result ? result->GetError() : "DuckDB query failed";
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err.c_str());
      return true;
    }

    const size_t field_count = CountVisibleFields(*join->fields);
    if (result->ColumnCount() != field_count) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "DuckDB result column count mismatch");
      return true;
    }

    std::vector<Field *> out_fields;
    out_fields.reserve(field_count);
    for (Item *item : VisibleFields(*join->fields)) {
      Item *real = item->real_item();
      if (real->type() != Item::FIELD_ITEM) {
        my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
                 "Unsupported SELECT list for DuckDB offload");
        return true;
      }
      auto *field_item = down_cast<Item_field *>(real);
      out_fields.push_back(field_item->field);
    }

    ha_rows sent = 0;
    while (true) {
      auto chunk = result->Fetch();
      if (!chunk || chunk->size() == 0) break;

      for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
        for (duckdb::idx_t col = 0; col < chunk->ColumnCount(); ++col) {
          Field *field = out_fields[col];
          const duckdb::Value value = chunk->GetValue(col, row);
          if (value.IsNull()) {
            field->set_null();
            continue;
          }
          field->set_notnull();
          std::string text;
          const auto physical = value.type().InternalType();
          if (physical == duckdb::PhysicalType::VARCHAR) {
            text = duckdb::StringValue::Get(value);
          } else {
            text = value.ToString();
          }
          field->store(text.data(), text.size(), field->charset());
        }

        if (query_result->send_data(thd, *join->fields)) return true;
        ++sent;
      }
    }
    join->send_records = sent;
  } catch (const std::exception &ex) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ex.what());
    return true;
  }

  return false;
}

static bool PrepareSecondaryEngine(THD *thd, LEX *lex) {
  auto *ctx = new (thd->mem_root) Duckdb_execution_context;
  if (ctx == nullptr) return true;
  lex->set_secondary_engine_execution_context(ctx);

  Table_ref *base_table = nullptr;
  std::string reason;
  if (!is_simple_select(lex, &base_table, &reason)) {
    ctx->eligible = false;
    ctx->fail_reason = reason.empty() ? "Query not eligible for DuckDB" : reason;
    return false;
  }

  if (base_table == nullptr || base_table->table == nullptr ||
      base_table->table->s == nullptr) {
    ctx->eligible = false;
    ctx->fail_reason = "Base table not available for DuckDB offload";
    return false;
  }

  if (base_table->db != nullptr && base_table->db_length > 0) {
    ctx->db.assign(base_table->db, base_table->db_length);
  } else {
    ctx->db.clear();
  }
  if (base_table->table_name != nullptr && base_table->table_name_length > 0) {
    ctx->table.assign(base_table->table_name, base_table->table_name_length);
  } else {
    ctx->table.clear();
  }
  ctx->db_path = resolve_duckdb_path(base_table->table->s);
  ctx->base_table = base_table->table;
  ctx->eligible = true;

  lex->add_statement_options(OPTION_NO_CONST_TABLES |
                             OPTION_NO_SUBQUERY_DURING_OPTIMIZATION);
  return false;
}

static bool OptimizeSecondaryEngine(THD *thd, LEX *lex) {
  auto *ctx = down_cast<Duckdb_execution_context *>(
      lex->secondary_engine_execution_context());
  if (ctx == nullptr) return false;

  Query_block *qb = lex->unit->first_query_block();
  if (qb == nullptr || qb->join == nullptr || qb->join->fields == nullptr) {
    ctx->fail_reason = "DuckDB offload requires a simple SELECT plan";
    thd->get_stmt_da()->set_error_status(thd, ER_PREPARE_FOR_PRIMARY_ENGINE);
    return true;
  }

  if (!ctx->eligible) {
    thd->get_stmt_da()->set_error_status(thd, ER_PREPARE_FOR_PRIMARY_ENGINE);
    return true;
  }
  if (ctx->base_table == nullptr) {
    ctx->fail_reason = "DuckDB offload base table missing";
    thd->get_stmt_da()->set_error_status(thd, ER_PREPARE_FOR_PRIMARY_ENGINE);
    return true;
  }

  if (!uses_only_field_items(*qb->join->fields, ctx->base_table,
                             &ctx->fail_reason)) {
    thd->get_stmt_da()->set_error_status(thd, ER_PREPARE_FOR_PRIMARY_ENGINE);
    return true;
  }

  const LEX_CSTRING &query = thd->query();
  duckdb_se::DuckdbRewriteResult rewrite =
      duckdb_se::RewriteForDuckdb(std::string(query.str, query.length));
  if (!rewrite.ok) {
    ctx->fail_reason = rewrite.reason.empty()
                           ? "DuckDB compatibility rewrite failed"
                           : rewrite.reason;
    thd->get_stmt_da()->set_error_status(thd, ER_PREPARE_FOR_PRIMARY_ENGINE);
    return true;
  }
  ctx->sql =
      rewrite_qualified_table(std::move(rewrite.sql), ctx->db, ctx->table);

  try {
    duckdb::DBConfig config(true);
    duckdb::DuckDB db(ctx->db_path, &config);
    duckdb::Connection conn(db);
    auto prepared = conn.Prepare(ctx->sql);
    if (prepared->HasError()) {
      ctx->fail_reason = prepared->GetError();
      thd->get_stmt_da()->set_error_status(thd, ER_PREPARE_FOR_PRIMARY_ENGINE);
      return true;
    }
    const size_t field_count = CountVisibleFields(*qb->join->fields);
    if (prepared->ColumnCount() != field_count) {
      ctx->fail_reason = "DuckDB result column count mismatch";
      thd->get_stmt_da()->set_error_status(thd, ER_PREPARE_FOR_PRIMARY_ENGINE);
      return true;
    }
  } catch (const std::exception &ex) {
    ctx->fail_reason = ex.what();
    thd->get_stmt_da()->set_error_status(thd, ER_PREPARE_FOR_PRIMARY_ENGINE);
    return true;
  }

  for (Query_block *block = lex->unit->first_query_block(); block != nullptr;
       block = block->next_query_block()) {
    if (block->join != nullptr) {
      block->join->override_executor_func = DuckdbExecuteQuery;
    }
  }

  return false;
}

static const char *DuckdbGetOffloadFailReason(THD *thd) {
  auto *ctx = down_cast<Duckdb_execution_context *>(
      thd->lex->secondary_engine_execution_context());
  if (ctx == nullptr || ctx->fail_reason.empty()) return nullptr;
  return ctx->fail_reason.c_str();
}

static void DuckdbSetOffloadFailReason(THD *thd, const char *reason) {
  auto *ctx = down_cast<Duckdb_execution_context *>(
      thd->lex->secondary_engine_execution_context());
  if (ctx == nullptr) return;
  ctx->fail_reason = reason ? reason : "";
}

}  // namespace

namespace duckdb_se {

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

  if (my_stat(path.c_str(), &stat_buf, MYF(0)) != nullptr) {
    my_error(ER_TABLE_EXISTS_ERROR, MYF(0), table_arg->s->table_name.str);
    return HA_ERR_TABLE_EXIST;
  }

  my_delete((path + ".wal").c_str(), MYF(0));

  try {
    duckdb::DBConfig config(false);
    duckdb::DuckDB db(path, &config);
    duckdb::Connection conn(db);
    std::string create_sql = "CREATE TABLE ";
    create_sql += quote_ident(table_arg->s->table_name.str,
                              table_arg->s->table_name.length);
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
    create_sql += ")";

    auto result = conn.Query(create_sql);
    if (result->HasError()) {
      my_error(ER_CANT_CREATE_TABLE, MYF(0), table_arg->s->table_name.str,
               HA_ERR_GENERIC, result->GetError().c_str());
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

int ha_duckdb::open(const char *, int, unsigned int, const dd::Table *) {
  DuckdbTableState *share =
      loaded_tables->get(table_share->db.str, table_share->table_name.str);
  if (share == nullptr && table_share->is_secondary_engine()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Table has not been loaded");
    return HA_ERR_GENERIC;
  }
  if (share == nullptr) {
    const std::string path = resolve_duckdb_path(table_share);
    MY_STAT stat_buf;
    if (my_stat(path.c_str(), &stat_buf, MYF(0)) == nullptr) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "DuckDB table file not found");
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
  m_table_name = std::string(table_share->table_name.str,
                             table_share->table_name.length);

  try {
    duckdb::DBConfig config(table_share->is_secondary_engine());
    m_db = std::make_unique<duckdb::DuckDB>(m_table_path, &config);
    m_conn = std::make_unique<duckdb::Connection>(*m_db);
  } catch (const std::exception &ex) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ex.what());
    return HA_ERR_GENERIC;
  }

  return 0;
}

int ha_duckdb::close() {
  m_result.reset();
  m_chunk.reset();
  m_conn.reset();
  m_db.reset();
  return 0;
}

int ha_duckdb::rnd_init(bool) {
  DBUG_TRACE;
  m_chunk.reset();
  m_result.reset();
  m_chunk_row = 0;

  if (!m_conn) return HA_ERR_GENERIC;

  try {
    const std::string query = "SELECT * FROM " +
                              quote_ident(m_table_name.c_str(),
                                          m_table_name.size());
    m_result = m_conn->SendQuery(query);
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
  m_result.reset();
  m_chunk.reset();
  m_chunk_row = 0;
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

      const uint field_count = table->s->fields;
      if (m_chunk->ColumnCount() != field_count) {
        my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
                 "DuckDB column count mismatch");
        return HA_ERR_GENERIC;
      }

      for (uint i = 0; i < field_count; ++i) {
        Field *field = table->field[i];
        if (table->read_set &&
            !bitmap_is_set(table->read_set, field->field_index())) {
          continue;
        }
        const duckdb::Value value = m_chunk->GetValue(i, m_chunk_row);
        if (value.IsNull()) {
          field->set_null();
          continue;
        }
        field->set_notnull();
        const auto physical = value.type().InternalType();
        if (physical == duckdb::PhysicalType::VARCHAR) {
          const std::string &str = duckdb::StringValue::Get(value);
          field->store(str.data(), str.size(), field->charset());
        } else {
          const std::string str = value.ToString();
          field->store(str.data(), str.size(), field->charset());
        }
      }

      m_chunk_row++;
      return 0;
    }
  } catch (const std::exception &ex) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ex.what());
    return HA_ERR_GENERIC;
  }
}

int ha_duckdb::write_row(uchar *buf) {
  if (table_share->is_secondary_engine()) return HA_ERR_WRONG_COMMAND;
  THD *thd = ha_thd();
  if (thd == nullptr || !thd_slave_thread(thd)) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "DuckDB DML is restricted to replication applier threads");
    return HA_ERR_WRONG_COMMAND;
  }
  ha_statistic_increment(&System_status_var::ha_write_count);
  if (buf == nullptr || table == nullptr) return HA_ERR_GENERIC;

  try {
    duckdb::DBConfig config(false);
    duckdb::DuckDB db(m_table_path, &config);
    duckdb::Connection conn(db);
    duckdb::Appender appender(conn, m_table_name);

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
        appender.Append(duckdb::Value::BLOB_RAW(
            std::string(tmp.ptr(), tmp.length())));
      } else {
        appender.Append(tmp.ptr(), static_cast<uint32_t>(tmp.length()));
      }
    }
    appender.EndRow();
    appender.Close();
  } catch (const std::exception &ex) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ex.what());
    return HA_ERR_GENERIC;
  }

  stats.records++;
  return 0;
}

int ha_duckdb::update_row(const uchar *old_data, uchar *new_data) {
  if (table_share->is_secondary_engine()) return HA_ERR_WRONG_COMMAND;
  THD *thd = ha_thd();
  if (thd == nullptr || !thd_slave_thread(thd)) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "DuckDB DML is restricted to replication applier threads");
    return HA_ERR_WRONG_COMMAND;
  }
  ha_statistic_increment(&System_status_var::ha_update_count);
  if (old_data == nullptr || new_data == nullptr || table == nullptr) {
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
  sql += quote_ident(m_table_name.c_str(), m_table_name.size());
  sql += " SET ";
  sql += build_set_clause(set_fields, set_values);
  sql += " WHERE ";
  sql += build_where_clause(where_fields, where_values);

  try {
    duckdb::DBConfig config(false);
    duckdb::DuckDB db(m_table_path, &config);
    duckdb::Connection conn(db);
    auto result = conn.Query(sql);
    if (result->HasError()) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), result->GetError().c_str());
      return HA_ERR_GENERIC;
    }
  } catch (const std::exception &ex) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ex.what());
    return HA_ERR_GENERIC;
  }

  return 0;
}

int ha_duckdb::delete_row(const uchar *buf) {
  if (table_share->is_secondary_engine()) return HA_ERR_WRONG_COMMAND;
  THD *thd = ha_thd();
  if (thd == nullptr || !thd_slave_thread(thd)) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "DuckDB DML is restricted to replication applier threads");
    return HA_ERR_WRONG_COMMAND;
  }
  ha_statistic_increment(&System_status_var::ha_delete_count);
  if (buf == nullptr || table == nullptr) return HA_ERR_GENERIC;

  const std::vector<Field *> where_fields =
      collect_fields(table, table->read_set);
  if (where_fields.empty()) return HA_ERR_GENERIC;

  std::vector<duckdb::Value> where_values =
      collect_values(table, buf, where_fields);

  std::string sql = "DELETE FROM ";
  sql += quote_ident(m_table_name.c_str(), m_table_name.size());
  sql += " WHERE ";
  sql += build_where_clause(where_fields, where_values);

  try {
    duckdb::DBConfig config(false);
    duckdb::DuckDB db(m_table_path, &config);
    duckdb::Connection conn(db);
    auto result = conn.Query(sql);
    if (result->HasError()) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), result->GetError().c_str());
      return HA_ERR_GENERIC;
    }
  } catch (const std::exception &ex) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ex.what());
    return HA_ERR_GENERIC;
  }

  stats.records = stats.records > 0 ? stats.records - 1 : 0;
  return 0;
}

int ha_duckdb::rnd_pos(uchar *, uchar *) {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

int ha_duckdb::info(unsigned int flags) {
  handler *primary = ha_get_primary_handler();
  if (primary == nullptr) return 0;
  int ret = primary->info(flags);
  if (ret == 0) {
    stats.records = primary->stats.records;
  }
  return ret;
}

handler::Table_flags ha_duckdb::table_flags() const {
  return HA_NO_INDEX_ACCESS;
}

unsigned long ha_duckdb::index_flags(unsigned int idx, unsigned int part,
                                     bool all_parts) const {
  const handler *primary = ha_get_primary_handler();
  const unsigned long primary_flags =
      primary == nullptr ? 0 : primary->index_flags(idx, part, all_parts);
  return ((HA_READ_RANGE | HA_KEY_SCAN_NOT_ROR) & primary_flags);
}

ha_rows ha_duckdb::records_in_range(unsigned int index, key_range *min_key,
                                    key_range *max_key) {
  return ha_get_primary_handler()->records_in_range(index, min_key, max_key);
}

THR_LOCK_DATA **ha_duckdb::store_lock(THD *, THR_LOCK_DATA **to,
                                      thr_lock_type lock_type) {
  if (lock_type != TL_IGNORE && m_lock.type == TL_UNLOCK) m_lock.type = lock_type;
  *to++ = &m_lock;
  return to;
}

int ha_duckdb::load_table(const TABLE &table) {
  const std::string path = resolve_duckdb_path(table.s);
  if (!ensure_duckdb_file(path)) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Failed to create DuckDB file");
    return HA_ERR_GENERIC;
  }

  try {
    duckdb::DuckDB db(path);
    duckdb::Connection con(db);
    const std::string table_name(table.s->table_name.str,
                                 table.s->table_name.length);
    const std::string quoted_table =
        quote_ident(table.s->table_name.str, table.s->table_name.length);

    auto drop_result = con.Query("DROP TABLE IF EXISTS " + quoted_table);
    if (drop_result->HasError()) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               drop_result->GetError().c_str());
      return HA_ERR_GENERIC;
    }

    std::string create_sql = "CREATE TABLE " + quoted_table + " (";
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

    duckdb::Appender appender(con, table_name);

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
        if (field->is_null()) {
          appender.Append(duckdb::Value());
          continue;
        }
        String tmp;
        field->val_str(&tmp);
        if (is_binary_field(field)) {
          std::string data(tmp.ptr(), tmp.length());
          appender.Append(duckdb::Value::BLOB(data));
        } else {
          appender.Append(tmp.ptr(), static_cast<uint32_t>(tmp.length()));
        }
      }
      appender.EndRow();
    }
    if (error != HA_ERR_END_OF_FILE && error != 0) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "Primary table scan failed");
      return HA_ERR_GENERIC;
    }

    appender.Close();
  } catch (const std::exception &ex) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ex.what());
    return HA_ERR_GENERIC;
  }

  loaded_tables->add(table.s->db.str, table.s->table_name.str, path);
  return 0;
}

int ha_duckdb::unload_table(const char *db_name, const char *table_name,
                            bool error_if_not_loaded) {
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

static bool duckdb_binlog_apply_paused = false;
static ulonglong duckdb_binlog_apply_throttle_rows_per_sec = 0;
static ulonglong duckdb_binlog_apply_throttle_bytes_per_sec = 0;
static ulonglong duckdb_binlog_apply_lag_alert_ms = 0;
static char *duckdb_binlog_apply_stop_at_gtid = nullptr;

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

static SYS_VAR *duckdb_system_variables[] = {
    MYSQL_SYSVAR(binlog_apply_paused),
    MYSQL_SYSVAR(binlog_apply_throttle_rows_per_sec),
    MYSQL_SYSVAR(binlog_apply_throttle_bytes_per_sec),
    MYSQL_SYSVAR(binlog_apply_lag_alert_ms),
    MYSQL_SYSVAR(binlog_apply_stop_at_gtid),
    nullptr};

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

static int show_duckdb_binlog_lag_ms(MYSQL_THD, SHOW_VAR *var, char *) {
  static ulonglong value;
  value = duckdb_se::GetBinlogApplyMetrics().lag_ms;
  var->type = SHOW_LONGLONG;
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

static int show_duckdb_binlog_last_gtid(MYSQL_THD, SHOW_VAR *var, char *buf) {
  var->type = SHOW_CHAR;
  var->value = buf;
  const auto metrics = duckdb_se::GetBinlogApplyMetrics();
  const char *gtid = metrics.last_gtid.empty() ? "" : metrics.last_gtid.c_str();
  std::snprintf(buf, SHOW_VAR_FUNC_BUFF_SIZE, "%s", gtid);
  return 0;
}

static SHOW_VAR duckdb_status_variables[] = {
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
    {"duckdb_binlog_lag_ms", (char *)show_duckdb_binlog_lag_ms, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_binlog_lag_alert", (char *)show_duckdb_binlog_lag_alert, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_binlog_last_gtid", (char *)show_duckdb_binlog_last_gtid, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_UNDEF, SHOW_SCOPE_GLOBAL}};

static handler *duckdb_create_handler(handlerton *hton, TABLE_SHARE *table,
                                      bool, MEM_ROOT *mem_root) {
  return new (mem_root) duckdb_se::ha_duckdb(hton, table);
}

static int duckdb_init_func(void *p) {
  DBUG_TRACE;

  loaded_tables = new LoadedTables();
  duckdb_se::SetBinlogApplyPaused(duckdb_binlog_apply_paused);
  duckdb_se::SetBinlogApplyThrottleRowsPerSec(
      duckdb_binlog_apply_throttle_rows_per_sec);
  duckdb_se::SetBinlogApplyThrottleBytesPerSec(
      duckdb_binlog_apply_throttle_bytes_per_sec);
  duckdb_se::SetBinlogApplyLagAlertThresholdMs(
      duckdb_binlog_apply_lag_alert_ms);
  duckdb_se::SetBinlogApplyStopAtGtid(
      duckdb_binlog_apply_stop_at_gtid ? duckdb_binlog_apply_stop_at_gtid : "");

  handlerton *duckdb_hton = static_cast<handlerton *>(p);
  duckdb_hton->create = duckdb_create_handler;
  duckdb_hton->state = SHOW_OPTION_YES;
  duckdb_hton->flags = HTON_IS_SECONDARY_ENGINE;
  duckdb_hton->db_type = DB_TYPE_UNKNOWN;
  duckdb_hton->prepare_secondary_engine = PrepareSecondaryEngine;
  duckdb_hton->optimize_secondary_engine = OptimizeSecondaryEngine;
  duckdb_hton->get_secondary_engine_offload_or_exec_fail_reason =
      DuckdbGetOffloadFailReason;
  duckdb_hton->set_secondary_engine_offload_fail_reason =
      DuckdbSetOffloadFailReason;
  duckdb_hton->secondary_engine_flags = MakeSecondaryEngineFlags(
      SecondaryEngineFlag::USE_EXTERNAL_EXECUTOR);
  return 0;
}

static int duckdb_deinit_func(void *) {
  DBUG_TRACE;

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
    "DuckDB secondary storage engine (embedded)",
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
