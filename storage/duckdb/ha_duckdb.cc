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

#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "duckdb.hpp"
#include "duckdb/common/types/value.hpp"
#include "my_bitmap.h"
#include "my_dbug.h"
#include "my_sys.h"
#include "mysqld_error.h"
#include "mysql/plugin.h"
#include "sql/mysqld.h"
#include "sql/sql_class.h"
#include "scope_guard.h"
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
  File file = my_open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, MYF(MY_WME));
  if (file < 0) return false;
  my_close(file, MYF(0));
  return true;
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

mysql_declare_plugin(duckdb){
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
