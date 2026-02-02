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

#ifndef PLUGIN_DUCKDB_BINLOG_STREAMER_H_
#define PLUGIN_DUCKDB_BINLOG_STREAMER_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mysql.h"
#include "storage/duckdb/duckdb_adapter.h"

namespace binary_log {
class Format_description_event;
}

namespace duckdb_se {

struct BinlogStreamOptions {
  std::string host{"127.0.0.1"};
  std::string user{"root"};
  std::string password;
  std::string socket;
  uint32_t port{3306};
  uint32_t server_id{24844};
  bool non_blocking{true};
  std::string gtid_set;
};

struct BinlogTableMap {
  uint64_t table_id{0};
  std::string schema;
  std::string table;
  std::vector<uint8_t> column_types;
  std::vector<uint8_t> metadata;
  std::vector<uint8_t> null_bitmap;
};

struct BinlogEvent {
  enum class Type {
    kGtid,
    kTableMap,
    kWriteRows,
    kUpdateRows,
    kDeleteRows,
    kXid,
    kQuery,
    kOther
  };

  Type type{Type::kOther};
  std::string gtid;
  uint64_t table_id{0};
  std::string schema;
  std::string table;
  std::vector<uint8_t> columns_before;
  std::vector<uint8_t> columns_after;
  std::vector<uint8_t> row_data;
  std::string query;
};

class DuckDBBinlogStreamer {
 public:
  DuckDBBinlogStreamer();
  ~DuckDBBinlogStreamer();

  Status Open(const BinlogStreamOptions &options);
  Status Close();

  Status NextEvent(BinlogEvent *event);
  const BinlogTableMap *GetTableMap(uint64_t table_id) const;

 private:
  Status ReadRawEvent(std::vector<uint8_t> *buffer);
  Status ParseGtidSet(const std::string &gtid_set);

  BinlogStreamOptions options_{};
  MYSQL *mysql_{nullptr};
  bool open_{false};
  std::string current_gtid_;
  std::vector<uint8_t> raw_buffer_;
  void *gtid_encoded_{nullptr};
  size_t gtid_encoded_size_{0};
  MYSQL_RPL rpl_{};
  std::map<uint64_t, BinlogTableMap> table_maps_;
  std::unique_ptr<binary_log::Format_description_event> fde_;
};

}  // namespace duckdb_se

#endif  // PLUGIN_DUCKDB_BINLOG_STREAMER_H_
