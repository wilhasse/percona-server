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

#ifndef PLUGIN_DUCKDB_HA_DUCKDB_H_
#define PLUGIN_DUCKDB_HA_DUCKDB_H_

#include <memory>
#include <string>

#include "my_base.h"
#include "sql/handler.h"
#include "thr_lock.h"

class THD;
struct TABLE;
struct TABLE_SHARE;

namespace dd {
class Table;
}

namespace duckdb {
class Connection;
class DataChunk;
class DuckDB;
class QueryResult;
}

namespace duckdb_se {

class ha_duckdb : public handler {
 public:
 ha_duckdb(handlerton *hton, TABLE_SHARE *table_share);

 private:
  int create(const char *name, TABLE *table, HA_CREATE_INFO *info,
             dd::Table *table_def) override;

  int open(const char *name, int mode, unsigned int test_if_locked,
           const dd::Table *table_def) override;

  int close() override;

  int rnd_init(bool scan) override;
  int rnd_end() override;

  int rnd_next(uchar *buf) override;

  int index_init(uint idx, bool sorted) override;
  int index_end() override;
  int index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map,
                     enum ha_rkey_function find_flag) override;
  int index_read_last_map(uchar *buf, const uchar *key,
                          key_part_map keypart_map) override;
  int index_next(uchar *buf) override;
  int index_prev(uchar *buf) override;
  int index_first(uchar *buf) override;
  int index_last(uchar *buf) override;

  int write_row(uchar *buf) override;

  int update_row(const uchar *old_data, uchar *new_data) override;

  int delete_row(const uchar *buf) override;

  int rnd_pos(uchar *buf, uchar *pos) override;

  void position(const uchar *record) override;

  int external_lock(THD *thd, int lock_type) override;

  int info(unsigned int) override;

  ha_rows records_in_range(unsigned int index, key_range *min_key,
                           key_range *max_key) override;

  unsigned long index_flags(unsigned int, unsigned int, bool) const override;

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             thr_lock_type lock_type) override;

  Table_flags table_flags() const override;

  uint max_supported_keys() const override { return MAX_KEY; }

  uint max_supported_key_length() const override { return MAX_KEY_LENGTH; }

  uint max_supported_key_part_length(HA_CREATE_INFO *create_info
                                     [[maybe_unused]]) const override {
    return MAX_KEY_LENGTH;
  }

  const char *table_type() const override { return "DUCKDB"; }

  int load_table(const TABLE &table) override;

  int unload_table(const char *db_name, const char *table_name,
                   bool error_if_not_loaded) override;

  int execute_index_scan(const std::string &where_sql, bool descending,
                         uchar *buf);
  int fetch_index_scan_row(uchar *buf);

  THR_LOCK_DATA m_lock;
  std::string m_table_path;
  std::string m_schema_name;
  std::string m_table_name;
  std::unique_ptr<duckdb::DuckDB> m_db;
  std::unique_ptr<duckdb::Connection> m_conn;
  std::unique_ptr<duckdb::QueryResult> m_result;
  std::unique_ptr<duckdb::DataChunk> m_chunk;
  uint64_t m_chunk_row{0};
  std::unique_ptr<duckdb::QueryResult> m_index_result;
  std::unique_ptr<duckdb::DataChunk> m_index_chunk;
  uint64_t m_index_chunk_row{0};
  bool m_index_descending{false};
};

}  // namespace duckdb_se

#endif  // PLUGIN_DUCKDB_HA_DUCKDB_H_
