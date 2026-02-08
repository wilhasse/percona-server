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
#include <mutex>
#include <string>
#include <vector>

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
class Appender;
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
  int rename_table(const char *from, const char *to,
                   const dd::Table *from_table_def,
                   dd::Table *to_table_def) override;
  int delete_table(const char *name, const dd::Table *table_def) override;
  int truncate(dd::Table *table_def) override;

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
  ha_rows multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                      void *seq_init_param, uint n_ranges,
                                      uint *bufsz, uint *flags,
                                      bool *force_default_mrr,
                                      Cost_estimate *cost) override;
  ha_rows multi_range_read_info(uint keyno, uint n_ranges, uint n_rows,
                                uint *bufsz, uint *flags,
                                Cost_estimate *cost) override;
  int multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                            uint n_ranges, uint mode,
                            HANDLER_BUFFER *buf) override;
  int multi_range_read_next(char **range_info) override;
  const Item *cond_push(const Item *cond) override;
  Item *idx_cond_push(uint keyno, Item *idx_cond) override;
  void cancel_pushed_idx_cond() override;

  int write_row(uchar *buf) override;
  void start_bulk_insert(ha_rows rows) override;
  int end_bulk_insert() override;

  int update_row(const uchar *old_data, uchar *new_data) override;

  int delete_row(const uchar *buf) override;

  int rnd_pos(uchar *buf, uchar *pos) override;

  void position(const uchar *record) override;

  int external_lock(THD *thd, int lock_type) override;
  int reset() override;

  int info(unsigned int) override;
  double scan_time() override;
  double read_time(uint index, uint ranges, ha_rows rows) override;
  double index_only_read_time(uint keynr, double records) override;

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
  bool can_use_native_mrr(uint keyno, uint flags) const;
  void reset_native_mrr_state();
  void invalidate_stats_cache();
  bool ensure_stats_cache();
  bool can_estimate_numeric_pk_range(const key_range *min_key,
                                     const key_range *max_key);
  ha_rows estimate_numeric_pk_range_rows(const key_range *min_key,
                                         const key_range *max_key);
  bool native_mrr_range_supported(const KEY *primary_key,
                                  const KEY_MULTI_RANGE &range) const;
  int start_next_native_mrr_batch();
  void append_row_to_appender(duckdb::Appender &appender, const uchar *buf);

  struct NativeMrrProbe {
    std::vector<std::string> key_sql_values;
    char *range_ptr{nullptr};
  };

  THR_LOCK_DATA m_lock;
  std::string m_table_path;
  std::string m_schema_name;
  std::string m_table_name;
  std::shared_ptr<duckdb::DuckDB> m_db;
  std::shared_ptr<std::timed_mutex> m_writer_mutex;
  std::unique_ptr<duckdb::Connection> m_conn;
  std::unique_ptr<duckdb::QueryResult> m_result;
  std::unique_ptr<duckdb::DataChunk> m_chunk;
  uint64_t m_chunk_row{0};
  std::vector<Field *> m_scan_fields;
  std::unique_ptr<duckdb::QueryResult> m_index_result;
  std::unique_ptr<duckdb::DataChunk> m_index_chunk;
  uint64_t m_index_chunk_row{0};
  std::vector<Field *> m_index_fields;
  bool m_index_descending{false};
  std::string m_pushed_cond_sql;
  std::string m_pushed_idx_cond_sql;
  bool m_native_mrr_active{false};
  bool m_native_mrr_fallback{true};
  uint m_native_mrr_mode{0};
  range_seq_t m_native_mrr_iter{nullptr};
  RANGE_SEQ_IF m_native_mrr_funcs{};
  size_t m_native_mrr_batch_size{0};
  size_t m_native_mrr_next_probe{0};
  std::vector<Field *> m_native_mrr_key_fields;
  std::vector<Field *> m_native_mrr_fields;
  std::vector<NativeMrrProbe> m_native_mrr_probes;
  std::unique_ptr<duckdb::QueryResult> m_native_mrr_result;
  std::unique_ptr<duckdb::DataChunk> m_native_mrr_chunk;
  uint64_t m_native_mrr_chunk_row{0};
  longlong m_stats_cached_query_id{-1};
  bool m_stats_cache_valid{false};
  ha_rows m_stats_cached_rows{0};
  bool m_stats_cached_pk_domain_valid{false};
  bool m_stats_cached_pk_integer{false};
  long double m_stats_cached_pk_min{0.0L};
  long double m_stats_cached_pk_max{0.0L};
  std::unique_ptr<duckdb::Appender> m_bulk_appender;
  bool m_in_bulk_insert{false};
  bool m_bulk_lock_held{false};
};

}  // namespace duckdb_se

#endif  // PLUGIN_DUCKDB_HA_DUCKDB_H_
