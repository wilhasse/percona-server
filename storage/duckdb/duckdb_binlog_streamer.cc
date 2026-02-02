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

#include "storage/duckdb/duckdb_binlog_streamer.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>

#include "libbinlogevents/include/binlog_event.h"
#include "libbinlogevents/include/control_events.h"
#include "libbinlogevents/include/gtids/gtidset.h"
#include "libbinlogevents/include/rows_event.h"
#include "my_byteorder.h"
#include "storage/duckdb/duckdb_gtid_utils.h"

namespace {

using binary_log::Format_description_event;
using binary_log::Gtid_event;
using binary_log::Log_event_type;
using binary_log::gtids::Gno_interval;
using binary_log::gtids::Gtid_set;
using binary_log::gtids::Uuid;

class Write_rows_event_view : public binary_log::Write_rows_event {
 public:
  Write_rows_event_view(const char *buf, const Format_description_event *fde)
      : binary_log::Rows_event(buf, fde),
        binary_log::Write_rows_event(buf, fde) {}
  const std::vector<uint8_t> &columns_before() const {
    return columns_before_image;
  }
  const std::vector<uint8_t> &columns_after() const {
    return columns_after_image;
  }
  const std::vector<uint8_t> &rows() const { return row; }
};

class Update_rows_event_view : public binary_log::Update_rows_event {
 public:
  Update_rows_event_view(const char *buf, const Format_description_event *fde)
      : binary_log::Rows_event(buf, fde),
        binary_log::Update_rows_event(buf, fde) {}
  const std::vector<uint8_t> &columns_before() const {
    return columns_before_image;
  }
  const std::vector<uint8_t> &columns_after() const {
    return columns_after_image;
  }
  const std::vector<uint8_t> &rows() const { return row; }
};

class Delete_rows_event_view : public binary_log::Delete_rows_event {
 public:
  Delete_rows_event_view(const char *buf, const Format_description_event *fde)
      : binary_log::Rows_event(buf, fde),
        binary_log::Delete_rows_event(buf, fde) {}
  const std::vector<uint8_t> &columns_before() const {
    return columns_before_image;
  }
  const std::vector<uint8_t> &columns_after() const {
    return columns_after_image;
  }
  const std::vector<uint8_t> &rows() const { return row; }
};


bool EncodeGtidSet(const Gtid_set &gtids, std::vector<uint8_t> *out) {
  if (!out) return false;
  out->clear();
  const auto &contents = gtids.get_gtid_set();
  if (contents.empty()) return true;
  char tmp[8];
  int8store(tmp, static_cast<ulonglong>(contents.size()));
  out->insert(out->end(), tmp, tmp + 8);
  for (const auto &entry : contents) {
    const auto &uuid = entry.first;
    const auto &intervals = entry.second;
    out->insert(out->end(), uuid.bytes, uuid.bytes + Uuid::BYTE_LENGTH);
    int8store(tmp, static_cast<ulonglong>(intervals.size()));
    out->insert(out->end(), tmp, tmp + 8);
    for (const auto &interval : intervals) {
      int8store(tmp, interval.get_start());
      out->insert(out->end(), tmp, tmp + 8);
      int8store(tmp, interval.get_end() + 1);
      out->insert(out->end(), tmp, tmp + 8);
    }
  }
  return true;
}

// Helper to query a single string value from MySQL
bool QuerySingleStringValue(MYSQL *mysql, const char *sql,
                            std::string *value_out) {
  if (!mysql || !value_out) return false;
  if (mysql_real_query(mysql, sql, std::strlen(sql)) != 0) return false;
  MYSQL_RES *res = mysql_store_result(mysql);
  if (!res) return false;
  MYSQL_ROW row = mysql_fetch_row(res);
  bool success = false;
  if (row && row[0]) {
    *value_out = row[0];
    success = true;
  }
  mysql_free_result(res);
  return success;
}

// Validate required binlog configuration on the source server
duckdb_se::Status ValidateBinlogConfig(MYSQL *mysql) {
  std::string value;

  // Check GTID mode - must be ON for GTID-based replication
  if (!QuerySingleStringValue(mysql, "SELECT @@GLOBAL.gtid_mode", &value)) {
    return duckdb_se::Status::Error(duckdb_se::StatusCode::kInvalid,
                                    "Failed to query @@GLOBAL.gtid_mode");
  }
  if (value != "ON") {
    return duckdb_se::Status::Error(
        duckdb_se::StatusCode::kInvalid,
        "gtid_mode must be ON for DuckDB binlog replication (current: " +
            value + ")");
  }

  // Check binlog_format - must be ROW for row-based replication
  if (!QuerySingleStringValue(mysql, "SELECT @@GLOBAL.binlog_format", &value)) {
    return duckdb_se::Status::Error(duckdb_se::StatusCode::kInvalid,
                                    "Failed to query @@GLOBAL.binlog_format");
  }
  if (value != "ROW") {
    return duckdb_se::Status::Error(
        duckdb_se::StatusCode::kInvalid,
        "binlog_format must be ROW for DuckDB binlog replication (current: " +
            value + ")");
  }

  // Check binlog_row_image - must be FULL for complete row data
  if (!QuerySingleStringValue(mysql, "SELECT @@GLOBAL.binlog_row_image",
                              &value)) {
    return duckdb_se::Status::Error(
        duckdb_se::StatusCode::kInvalid,
        "Failed to query @@GLOBAL.binlog_row_image");
  }
  if (value != "FULL") {
    return duckdb_se::Status::Error(
        duckdb_se::StatusCode::kInvalid,
        "binlog_row_image must be FULL for DuckDB binlog replication "
        "(current: " +
            value + ")");
  }

  return duckdb_se::Status::Ok();
}

}  // namespace

namespace duckdb_se {

DuckDBBinlogStreamer::DuckDBBinlogStreamer() = default;

DuckDBBinlogStreamer::~DuckDBBinlogStreamer() { (void)Close(); }

Status DuckDBBinlogStreamer::Open(const BinlogStreamOptions &options) {
  if (open_) {
    return Status::Error(StatusCode::kAlreadyInitialized,
                         "Binlog streamer already open");
  }
  options_ = options;
  mysql_ = mysql_init(nullptr);
  if (!mysql_) {
    return Status::Error(StatusCode::kInvalid, "mysql_init failed");
  }
  if (!mysql_real_connect(mysql_, options_.host.c_str(), options_.user.c_str(),
                          options_.password.empty()
                              ? nullptr
                              : options_.password.c_str(),
                          nullptr, options_.port,
                          options_.socket.empty() ? nullptr
                                                  : options_.socket.c_str(),
                          0)) {
    const std::string msg = mysql_error(mysql_);
    mysql_close(mysql_);
    mysql_ = nullptr;
    return Status::Error(StatusCode::kInvalid,
                         "mysql_real_connect failed: " + msg);
  }

  // Validate required binlog configuration before proceeding
  {
    Status st = ValidateBinlogConfig(mysql_);
    if (!st.ok()) {
      mysql_close(mysql_);
      mysql_ = nullptr;
      return st;
    }
  }

  // Enforce GTID auto-position by ensuring a GTID set is provided.
  if (options_.gtid_set.empty()) {
    std::string executed;
    if (!QuerySingleStringValue(mysql_, "SELECT @@GLOBAL.GTID_EXECUTED",
                                &executed)) {
      mysql_close(mysql_);
      mysql_ = nullptr;
      return Status::Error(StatusCode::kInvalid,
                           "Failed to query @@GLOBAL.GTID_EXECUTED for "
                           "auto-position");
    }
    if (executed.empty()) {
      mysql_close(mysql_);
      mysql_ = nullptr;
      return Status::Error(StatusCode::kInvalid,
                           "GTID_EXECUTED is empty; auto-position requires a "
                           "GTID set. Run a transaction or set "
                           "duckdb_binlog_apply_start_gtid.");
    }
    options_.gtid_set = std::move(executed);
  }

  const std::string checksum_sql =
      "SET @master_binlog_checksum = 'NONE', "
      "@source_binlog_checksum = 'NONE'";
  if (mysql_real_query(mysql_, checksum_sql.c_str(), checksum_sql.size()) != 0) {
    const std::string msg = mysql_error(mysql_);
    mysql_close(mysql_);
    mysql_ = nullptr;
    return Status::Error(StatusCode::kInvalid,
                         "Failed to set binlog checksum: " + msg);
  }

  rpl_ = MYSQL_RPL{};
  rpl_.file_name_length = 0;
  rpl_.file_name = "";
  rpl_.start_position = 4;
  rpl_.server_id = options_.server_id;
  rpl_.flags = MYSQL_RPL_SKIP_HEARTBEAT | MYSQL_RPL_GTID;
  if (options_.non_blocking) {
    constexpr unsigned int kNonBlockingFlag = 1u << 0;
    rpl_.flags |= kNonBlockingFlag;
  }

  if (!options_.gtid_set.empty()) {
    Status st = ParseGtidSet(options_.gtid_set);
    if (!st.ok()) {
      mysql_close(mysql_);
      mysql_ = nullptr;
      return st;
    }
    rpl_.gtid_set_arg = gtid_encoded_;
    rpl_.gtid_set_encoded_size = gtid_encoded_size_;
  } else {
    rpl_.gtid_set_arg = nullptr;
    rpl_.gtid_set_encoded_size = 0;
  }

  if (mysql_binlog_open(mysql_, &rpl_) != 0) {
    const std::string msg = mysql_error(mysql_);
    mysql_close(mysql_);
    mysql_ = nullptr;
    return Status::Error(StatusCode::kInvalid,
                         "mysql_binlog_open failed: " + msg);
  }

  fde_ = std::make_unique<Format_description_event>(BINLOG_VERSION, "8.0.0");
  open_ = true;
  return Status::Ok();
}

Status DuckDBBinlogStreamer::Close() {
  if (!open_) return Status::Ok();
  mysql_binlog_close(mysql_, &rpl_);
  mysql_close(mysql_);
  mysql_ = nullptr;
  if (gtid_encoded_) {
    free(gtid_encoded_);
    gtid_encoded_ = nullptr;
  }
  gtid_encoded_size_ = 0;
  raw_buffer_.clear();
  table_maps_.clear();
  current_gtid_.clear();
  fde_.reset();
  open_ = false;
  return Status::Ok();
}

Status DuckDBBinlogStreamer::ParseGtidSet(const std::string &gtid_set) {
  Gtid_set parsed;
  std::string error;
  if (!ParseGtidSetString(gtid_set, &parsed, &error)) {
    return Status::Error(StatusCode::kInvalid,
                         error.empty() ? "Invalid GTID set" : error);
  }
  std::vector<uint8_t> encoded;
  EncodeGtidSet(parsed, &encoded);
  if (!encoded.empty()) {
    gtid_encoded_ = malloc(encoded.size());
    if (!gtid_encoded_) {
      return Status::Error(StatusCode::kInvalid,
                           "Failed to allocate GTID set buffer");
    }
    std::memcpy(gtid_encoded_, encoded.data(), encoded.size());
    gtid_encoded_size_ = encoded.size();
  }
  return Status::Ok();
}

Status DuckDBBinlogStreamer::ReadRawEvent(std::vector<uint8_t> *buffer) {
  if (!buffer) {
    return Status::Error(StatusCode::kInvalid, "Buffer is null");
  }
  if (!open_) {
    return Status::Error(StatusCode::kNotInitialized,
                         "Binlog streamer not open");
  }
  if (mysql_binlog_fetch(mysql_, &rpl_) != 0) {
    const std::string msg = mysql_error(mysql_);
    return Status::Error(StatusCode::kInvalid,
                         "mysql_binlog_fetch failed: " + msg);
  }
  if (rpl_.size == 0) {
    buffer->clear();
    return Status::Error(StatusCode::kInvalid, "No binlog data available");
  }
  const unsigned char *payload = rpl_.buffer + 1;
  const unsigned long payload_size = rpl_.size - 1;
  buffer->assign(payload, payload + payload_size);
  return Status::Ok();
}

Status DuckDBBinlogStreamer::NextEvent(BinlogEvent *event) {
  if (!event) {
    return Status::Error(StatusCode::kInvalid, "Event output is null");
  }
  event->type = BinlogEvent::Type::kOther;
  event->gtid.clear();
  event->table_id = 0;
  event->schema.clear();
  event->table.clear();
  event->columns_before.clear();
  event->columns_after.clear();
  event->row_data.clear();

  if (!fde_) {
    fde_ = std::make_unique<Format_description_event>(BINLOG_VERSION, "8.0.0");
  }

  for (;;) {
    Status st = ReadRawEvent(&raw_buffer_);
    if (!st.ok()) return st;
    if (raw_buffer_.empty()) {
      return Status::Error(StatusCode::kInvalid, "No binlog data available");
    }
    const auto ev_type = static_cast<Log_event_type>(
        raw_buffer_[EVENT_TYPE_OFFSET]);
    const char *buf = reinterpret_cast<const char *>(raw_buffer_.data());

    switch (ev_type) {
      case binary_log::FORMAT_DESCRIPTION_EVENT: {
        auto next_fde = std::make_unique<Format_description_event>(buf, fde_.get());
        fde_ = std::move(next_fde);
        continue;
      }
      case binary_log::TABLE_MAP_EVENT: {
        binary_log::Table_map_event tme(buf, fde_.get());
        BinlogTableMap map;
        map.table_id = tme.get_table_id();
        map.schema = tme.get_db_name();
        map.table = tme.get_table_name();
        if (tme.m_coltype && tme.m_colcnt > 0) {
          map.column_types.assign(tme.m_coltype, tme.m_coltype + tme.m_colcnt);
        }
        if (tme.m_field_metadata && tme.m_field_metadata_size > 0) {
          map.metadata.assign(tme.m_field_metadata,
                              tme.m_field_metadata + tme.m_field_metadata_size);
        }
        const size_t null_bitmap_size = (tme.m_colcnt + 7) / 8;
        if (tme.m_null_bits && null_bitmap_size > 0) {
          map.null_bitmap.assign(tme.m_null_bits,
                                 tme.m_null_bits + null_bitmap_size);
        }
        table_maps_[map.table_id] = map;
        event->type = BinlogEvent::Type::kTableMap;
        event->table_id = map.table_id;
        event->schema = map.schema;
        event->table = map.table;
        return Status::Ok();
      }
      case binary_log::GTID_LOG_EVENT: {
        Gtid_event gev(buf, fde_.get());
        binary_log::gtids::Gtid gtid(gev.get_uuid(), gev.get_gno());
        current_gtid_ = gtid.to_string();
        event->type = BinlogEvent::Type::kGtid;
        event->gtid = current_gtid_;
        return Status::Ok();
      }
      case binary_log::WRITE_ROWS_EVENT:
      case binary_log::WRITE_ROWS_EVENT_V1: {
        Write_rows_event_view wev(buf, fde_.get());
        event->type = BinlogEvent::Type::kWriteRows;
        event->table_id = wev.get_table_id();
        event->columns_before = wev.columns_before();
        event->columns_after = wev.columns_after();
        event->row_data = wev.rows();
        event->gtid = current_gtid_;
        if (auto it = table_maps_.find(event->table_id);
            it != table_maps_.end()) {
          event->schema = it->second.schema;
          event->table = it->second.table;
        }
        return Status::Ok();
      }
      case binary_log::UPDATE_ROWS_EVENT:
      case binary_log::UPDATE_ROWS_EVENT_V1:
      case binary_log::PARTIAL_UPDATE_ROWS_EVENT: {
        Update_rows_event_view uev(buf, fde_.get());
        event->type = BinlogEvent::Type::kUpdateRows;
        event->table_id = uev.get_table_id();
        event->columns_before = uev.columns_before();
        event->columns_after = uev.columns_after();
        event->row_data = uev.rows();
        event->gtid = current_gtid_;
        if (auto it = table_maps_.find(event->table_id);
            it != table_maps_.end()) {
          event->schema = it->second.schema;
          event->table = it->second.table;
        }
        return Status::Ok();
      }
      case binary_log::DELETE_ROWS_EVENT:
      case binary_log::DELETE_ROWS_EVENT_V1: {
        Delete_rows_event_view dev(buf, fde_.get());
        event->type = BinlogEvent::Type::kDeleteRows;
        event->table_id = dev.get_table_id();
        event->columns_before = dev.columns_before();
        event->columns_after = dev.columns_after();
        event->row_data = dev.rows();
        event->gtid = current_gtid_;
        if (auto it = table_maps_.find(event->table_id);
            it != table_maps_.end()) {
          event->schema = it->second.schema;
          event->table = it->second.table;
        }
        return Status::Ok();
      }
      case binary_log::XID_EVENT: {
        event->type = BinlogEvent::Type::kXid;
        event->gtid = current_gtid_;
        return Status::Ok();
      }
      case binary_log::QUERY_EVENT: {
        event->type = BinlogEvent::Type::kQuery;
        event->gtid = current_gtid_;
        return Status::Ok();
      }
      default:
        break;
    }
  }
}

const BinlogTableMap *DuckDBBinlogStreamer::GetTableMap(
    uint64_t table_id) const {
  auto it = table_maps_.find(table_id);
  if (it == table_maps_.end()) return nullptr;
  return &it->second;
}

}  // namespace duckdb_se
