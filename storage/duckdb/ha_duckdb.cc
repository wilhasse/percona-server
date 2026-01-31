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
#include "mysql/plugin.h"
#include "sql/mysqld.h"
#include "sql/query_result.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"
#include "sql/visible_fields.h"
#include "scope_guard.h"
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

std::string duckdb_type_for_field(const Field *field) {
  return is_binary_field(field) ? "BLOB" : "VARCHAR";
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

std::string normalize_query_for_duckdb(std::string sql) {
  for (char &ch : sql) {
    if (ch == '`') ch = '"';
  }
  return sql;
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
          if (physical == duckdb::PhysicalType::VARCHAR ||
              physical == duckdb::PhysicalType::BLOB) {
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
  ctx->sql = normalize_query_for_duckdb(
      std::string(query.str, query.length));
  ctx->sql = rewrite_qualified_table(std::move(ctx->sql), ctx->db, ctx->table);

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

int ha_duckdb::open(const char *, int, unsigned int, const dd::Table *) {
  DuckdbTableState *share =
      loaded_tables->get(table_share->db.str, table_share->table_name.str);
  if (share == nullptr) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Table has not been loaded");
    return HA_ERR_GENERIC;
  }
  thr_lock_data_init(&share->lock, &m_lock, nullptr);
  m_table_path = share->path;
  m_table_name = std::string(table_share->table_name.str,
                             table_share->table_name.length);

  try {
    duckdb::DBConfig config(true);
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
      create_sql += duckdb_type_for_field(field);
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

static handler *duckdb_create_handler(handlerton *hton, TABLE_SHARE *table,
                                      bool, MEM_ROOT *mem_root) {
  return new (mem_root) duckdb_se::ha_duckdb(hton, table);
}

static int duckdb_init_func(void *p) {
  DBUG_TRACE;

  loaded_tables = new LoadedTables();

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
    nullptr, /* status variables */
    nullptr, /* system variables */
    nullptr, /* config options */
    0,       /* flags */
} mysql_declare_plugin_end;
