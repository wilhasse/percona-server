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

#include "storage/duckdb/duckdb_row_decoder.h"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

#include "decimal.h"
#include "libbinlogevents/export/binary_log_funcs.h"
#include "my_byteorder.h"
#include "my_time.h"
#include "sql/my_decimal.h"
#include "field_types.h"

namespace duckdb_se {
namespace {

bool BitmapIsSet(const std::vector<uint8_t> &bitmap, size_t index) {
  const size_t byte = index / 8;
  const size_t bit = index % 8;
  if (byte >= bitmap.size()) return false;
  return (bitmap[byte] & (1u << bit)) != 0;
}

size_t CountIncludedColumns(const std::vector<uint8_t> &bitmap,
                            size_t column_count) {
  size_t count = 0;
  for (size_t i = 0; i < column_count; ++i) {
    if (BitmapIsSet(bitmap, i)) ++count;
  }
  return count;
}

Status EnsureFullImage(const std::vector<uint8_t> &bitmap,
                       size_t column_count) {
  const size_t expected_bytes = (column_count + 7) / 8;
  if (bitmap.size() < expected_bytes) {
    return Status::Error(StatusCode::kInvalid,
                         "Column bitmap shorter than column count");
  }
  for (size_t i = 0; i < column_count; ++i) {
    if (!BitmapIsSet(bitmap, i)) {
      return Status::Error(StatusCode::kInvalid,
                           "binlog_row_image must be FULL for row decoding");
    }
  }
  return Status::Ok();
}

bool ReadGmTime(std::time_t seconds, std::tm *out) {
#ifdef _WIN32
  return gmtime_s(out, &seconds) == 0;
#else
  return gmtime_r(&seconds, out) != nullptr;
#endif
}

std::string FormatTimestamp(my_time_t seconds, uint32 usec, uint decimals) {
  std::tm tm_val{};
  std::time_t sec = static_cast<std::time_t>(seconds);
  if (!ReadGmTime(sec, &tm_val)) {
    return {};
  }
  MYSQL_TIME ltime{};
  ltime.year = tm_val.tm_year + 1900;
  ltime.month = tm_val.tm_mon + 1;
  ltime.day = tm_val.tm_mday;
  ltime.hour = tm_val.tm_hour;
  ltime.minute = tm_val.tm_min;
  ltime.second = tm_val.tm_sec;
  ltime.second_part = usec;
  ltime.time_type = MYSQL_TIMESTAMP_DATETIME;
  char buf[MAX_DATE_STRING_REP_LENGTH];
  const int len = my_datetime_to_str(ltime, buf, decimals);
  return std::string(buf, static_cast<size_t>(len));
}

std::string FloatToString(float value) {
  std::ostringstream oss;
  oss.setf(std::ios::fmtflags(0), std::ios::floatfield);
  oss << std::setprecision(std::numeric_limits<float>::digits10 + 2) << value;
  return oss.str();
}

std::string DoubleToString(double value) {
  std::ostringstream oss;
  oss.setf(std::ios::fmtflags(0), std::ios::floatfield);
  oss << std::setprecision(std::numeric_limits<double>::digits10 + 2) << value;
  return oss.str();
}

Status DecodeLengthPrefixedString(const uint8_t *ptr, const uint8_t *end,
                                  uint32 max_len, Cell *cell,
                                  size_t *consumed) {
  if (!cell || !consumed) {
    return Status::Error(StatusCode::kInvalid, "Invalid output pointers");
  }
  if (max_len < 256) {
    if (ptr + 1 > end) {
      return Status::Error(StatusCode::kInvalid, "VARCHAR length missing");
    }
    const uint32 len = *ptr;
    if (ptr + 1 + len > end) {
      return Status::Error(StatusCode::kInvalid, "VARCHAR value truncated");
    }
    cell->value.assign(reinterpret_cast<const char *>(ptr + 1), len);
    cell->is_blob = false;
    *consumed = 1 + len;
    return Status::Ok();
  }
  if (ptr + 2 > end) {
    return Status::Error(StatusCode::kInvalid, "VARCHAR length missing");
  }
  const uint32 len = uint2korr(ptr);
  if (ptr + 2 + len > end) {
    return Status::Error(StatusCode::kInvalid, "VARCHAR value truncated");
  }
  cell->value.assign(reinterpret_cast<const char *>(ptr + 2), len);
  cell->is_blob = false;
  *consumed = 2 + len;
  return Status::Ok();
}

Status DecodeBlob(const uint8_t *ptr, const uint8_t *end, uint32 meta,
                  Cell *cell, size_t *consumed) {
  if (!cell || !consumed) {
    return Status::Error(StatusCode::kInvalid, "Invalid output pointers");
  }
  if (meta < 1 || meta > 4) {
    return Status::Error(StatusCode::kInvalid, "Invalid blob metadata");
  }
  if (ptr + meta > end) {
    return Status::Error(StatusCode::kInvalid, "Blob length missing");
  }
  uint32 len = 0;
  switch (meta) {
    case 1:
      len = *ptr;
      break;
    case 2:
      len = uint2korr(ptr);
      break;
    case 3:
      len = uint3korr(ptr);
      break;
    case 4:
      len = uint4korr(ptr);
      break;
    default:
      break;
  }
  if (ptr + meta + len > end) {
    return Status::Error(StatusCode::kInvalid, "Blob value truncated");
  }
  cell->is_blob = true;
  cell->value.assign(reinterpret_cast<const char *>(ptr + meta), len);
  *consumed = meta + len;
  return Status::Ok();
}

Status DecodeCell(enum_field_types type, uint meta, const uint8_t *ptr,
                  const uint8_t *end, Cell *cell, size_t *consumed) {
  if (!cell || !consumed) {
    return Status::Error(StatusCode::kInvalid, "Invalid output pointers");
  }
  if (ptr > end) {
    return Status::Error(StatusCode::kInvalid, "Row pointer out of range");
  }
  cell->is_null = false;
  cell->is_blob = false;

  switch (type) {
    case MYSQL_TYPE_TINY: {
      if (ptr + 1 > end)
        return Status::Error(StatusCode::kInvalid, "TINYINT truncated");
      const int8_t val = static_cast<int8_t>(*ptr);
      cell->value = std::to_string(static_cast<int>(val));
      *consumed = 1;
      return Status::Ok();
    }
    case MYSQL_TYPE_SHORT: {
      if (ptr + 2 > end)
        return Status::Error(StatusCode::kInvalid, "SMALLINT truncated");
      const int16_t val = static_cast<int16_t>(sint2korr(ptr));
      cell->value = std::to_string(val);
      *consumed = 2;
      return Status::Ok();
    }
    case MYSQL_TYPE_INT24: {
      if (ptr + 3 > end)
        return Status::Error(StatusCode::kInvalid, "MEDIUMINT truncated");
      const int32_t val = sint3korr(ptr);
      cell->value = std::to_string(val);
      *consumed = 3;
      return Status::Ok();
    }
    case MYSQL_TYPE_LONG: {
      if (ptr + 4 > end)
        return Status::Error(StatusCode::kInvalid, "INT truncated");
      const int32_t val = sint4korr(ptr);
      cell->value = std::to_string(val);
      *consumed = 4;
      return Status::Ok();
    }
    case MYSQL_TYPE_LONGLONG: {
      if (ptr + 8 > end)
        return Status::Error(StatusCode::kInvalid, "BIGINT truncated");
      const longlong val = sint8korr(ptr);
      cell->value = std::to_string(static_cast<long long>(val));
      *consumed = 8;
      return Status::Ok();
    }
    case MYSQL_TYPE_FLOAT: {
      if (ptr + 4 > end)
        return Status::Error(StatusCode::kInvalid, "FLOAT truncated");
      const float val = float4get(ptr);
      cell->value = FloatToString(val);
      *consumed = 4;
      return Status::Ok();
    }
    case MYSQL_TYPE_DOUBLE: {
      if (ptr + 8 > end)
        return Status::Error(StatusCode::kInvalid, "DOUBLE truncated");
      const double val = float8get(ptr);
      cell->value = DoubleToString(val);
      *consumed = 8;
      return Status::Ok();
    }
    case MYSQL_TYPE_NEWDECIMAL: {
      const uint precision = meta >> 8;
      const uint decimals = meta & 0xFF;
      const int bin_size = my_decimal_get_binary_size(precision, decimals);
      if (ptr + bin_size > end) {
        return Status::Error(StatusCode::kInvalid, "DECIMAL truncated");
      }
      my_decimal dec;
      const int err = binary2my_decimal(E_DEC_FATAL_ERROR, ptr, &dec, precision,
                                        decimals);
      if (err != E_DEC_OK) {
        return Status::Error(StatusCode::kInvalid, "DECIMAL decode error");
      }
      char buf[DECIMAL_MAX_STR_LENGTH + 1];
      int len = static_cast<int>(sizeof(buf));
      if (decimal2string(&dec, buf, &len) != E_DEC_OK) {
        return Status::Error(StatusCode::kInvalid, "DECIMAL to string failed");
      }
      cell->value.assign(buf, static_cast<size_t>(len));
      *consumed = static_cast<size_t>(bin_size);
      return Status::Ok();
    }
    case MYSQL_TYPE_BIT: {
      const uint32 nbits = ((meta >> 8) * 8) + (meta & 0xFF);
      const uint32 len = (nbits + 7) / 8;
      if (ptr + len > end) {
        return Status::Error(StatusCode::kInvalid, "BIT truncated");
      }
      cell->is_blob = true;
      cell->value.assign(reinterpret_cast<const char *>(ptr), len);
      *consumed = len;
      return Status::Ok();
    }
    case MYSQL_TYPE_TIMESTAMP: {
      if (ptr + 4 > end)
        return Status::Error(StatusCode::kInvalid, "TIMESTAMP truncated");
      const uint32 secs = uint4korr(ptr);
      cell->value = FormatTimestamp(static_cast<my_time_t>(secs), 0, 0);
      *consumed = 4;
      return Status::Ok();
    }
    case MYSQL_TYPE_TIMESTAMP2: {
      const uint32 len = my_timestamp_binary_length(meta);
      if (ptr + len > end)
        return Status::Error(StatusCode::kInvalid, "TIMESTAMP2 truncated");
      my_timeval tv{};
      my_timestamp_from_binary(&tv, ptr, meta);
      cell->value =
          FormatTimestamp(tv.m_tv_sec, static_cast<uint32>(tv.m_tv_usec), meta);
      *consumed = len;
      return Status::Ok();
    }
    case MYSQL_TYPE_DATETIME: {
      if (ptr + 8 > end)
        return Status::Error(StatusCode::kInvalid, "DATETIME truncated");
      const uint64 i64 = uint8korr(ptr);
      const uint64 date = i64 / 1000000;
      const uint64 time = i64 % 1000000;
      const int year = static_cast<int>(date / 10000);
      const int month = static_cast<int>((date % 10000) / 100);
      const int day = static_cast<int>(date % 100);
      const int hour = static_cast<int>(time / 10000);
      const int minute = static_cast<int>((time % 10000) / 100);
      const int second = static_cast<int>(time % 100);
      char buf[32];
      const int len = snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                               year, month, day, hour, minute, second);
      cell->value.assign(buf, static_cast<size_t>(len));
      *consumed = 8;
      return Status::Ok();
    }
    case MYSQL_TYPE_DATETIME2: {
      const uint32 len = my_datetime_binary_length(meta);
      if (ptr + len > end)
        return Status::Error(StatusCode::kInvalid, "DATETIME2 truncated");
      MYSQL_TIME ltime{};
      const longlong packed = my_datetime_packed_from_binary(ptr, meta);
      TIME_from_longlong_datetime_packed(&ltime, packed);
      char buf[MAX_DATE_STRING_REP_LENGTH];
      const int out_len = my_datetime_to_str(ltime, buf, meta);
      cell->value.assign(buf, static_cast<size_t>(out_len));
      *consumed = len;
      return Status::Ok();
    }
    case MYSQL_TYPE_TIME: {
      if (ptr + 3 > end)
        return Status::Error(StatusCode::kInvalid, "TIME truncated");
      const uint32 i32 = uint3korr(ptr);
      const int hour = static_cast<int>(i32 / 10000);
      const int minute = static_cast<int>((i32 % 10000) / 100);
      const int second = static_cast<int>(i32 % 100);
      char buf[16];
      const int len = snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hour, minute,
                               second);
      cell->value.assign(buf, static_cast<size_t>(len));
      *consumed = 3;
      return Status::Ok();
    }
    case MYSQL_TYPE_TIME2: {
      const uint32 len = my_time_binary_length(meta);
      if (ptr + len > end)
        return Status::Error(StatusCode::kInvalid, "TIME2 truncated");
      MYSQL_TIME ltime{};
      const longlong packed = my_time_packed_from_binary(ptr, meta);
      TIME_from_longlong_time_packed(&ltime, packed);
      char buf[MAX_DATE_STRING_REP_LENGTH];
      const int out_len = my_time_to_str(ltime, buf, meta);
      cell->value.assign(buf, static_cast<size_t>(out_len));
      *consumed = len;
      return Status::Ok();
    }
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE: {
      if (ptr + 3 > end)
        return Status::Error(StatusCode::kInvalid, "DATE truncated");
      const uint32 tmp = uint3korr(ptr);
      const int day = static_cast<int>(tmp & 31);
      const int month = static_cast<int>((tmp >> 5) & 15);
      const int year = static_cast<int>(tmp >> 9);
      MYSQL_TIME ltime{};
      ltime.year = year;
      ltime.month = month;
      ltime.day = day;
      ltime.time_type = MYSQL_TIMESTAMP_DATE;
      char buf[MAX_DATE_STRING_REP_LENGTH];
      const int out_len = my_date_to_str(ltime, buf);
      cell->value.assign(buf, static_cast<size_t>(out_len));
      *consumed = 3;
      return Status::Ok();
    }
    case MYSQL_TYPE_YEAR: {
      if (ptr + 1 > end)
        return Status::Error(StatusCode::kInvalid, "YEAR truncated");
      const uint32 year = static_cast<uint32>(*ptr) + 1900;
      cell->value = std::to_string(year);
      *consumed = 1;
      return Status::Ok();
    }
    case MYSQL_TYPE_ENUM: {
      const uint32 pack_len = meta & 0xFF;
      if (pack_len == 0 || ptr + pack_len > end) {
        return Status::Error(StatusCode::kInvalid, "ENUM truncated");
      }
      uint32 value = 0;
      switch (pack_len) {
        case 1:
          value = *ptr;
          break;
        case 2:
          value = uint2korr(ptr);
          break;
        case 3:
          value = uint3korr(ptr);
          break;
        case 4:
          value = uint4korr(ptr);
          break;
        default:
          return Status::Error(StatusCode::kInvalid, "Unsupported ENUM size");
      }
      cell->value = std::to_string(value);
      *consumed = pack_len;
      return Status::Ok();
    }
    case MYSQL_TYPE_SET: {
      const uint32 pack_len = meta & 0xFF;
      if (pack_len == 0 || ptr + pack_len > end) {
        return Status::Error(StatusCode::kInvalid, "SET truncated");
      }
      uint64 value = 0;
      for (uint32 i = 0; i < pack_len; ++i) {
        value |= (static_cast<uint64>(ptr[i]) << (8 * i));
      }
      cell->value = std::to_string(value);
      *consumed = pack_len;
      return Status::Ok();
    }
    case MYSQL_TYPE_JSON: {
      if (ptr + 4 > end) {
        return Status::Error(StatusCode::kInvalid, "JSON length missing");
      }
      const uint32 len = uint4korr(ptr);
      if (ptr + 4 + len > end) {
        return Status::Error(StatusCode::kInvalid, "JSON value truncated");
      }
      cell->is_blob = true;
      cell->value.assign(reinterpret_cast<const char *>(ptr + 4), len);
      *consumed = 4 + len;
      return Status::Ok();
    }
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_GEOMETRY:
      return DecodeBlob(ptr, end, meta, cell, consumed);
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING: {
      const uint32 max_len = meta;
      return DecodeLengthPrefixedString(ptr, end, max_len, cell, consumed);
    }
    case MYSQL_TYPE_STRING: {
      uint32 max_len = meta;
      if (meta >= 256) {
        const uint byte0 = meta >> 8;
        const uint byte1 = meta & 0xFF;
        if ((byte0 & 0x30) != 0x30) {
          max_len = byte1 | (((byte0 & 0x30) ^ 0x30) << 4);
        } else {
          max_len = meta & 0xFF;
        }
      }
      return DecodeLengthPrefixedString(ptr, end, max_len, cell, consumed);
    }
    case MYSQL_TYPE_DECIMAL: {
      const uint32 len = calc_field_size(type, ptr, meta);
      if (ptr + len > end) {
        return Status::Error(StatusCode::kInvalid, "DECIMAL truncated");
      }
      cell->value.assign(reinterpret_cast<const char *>(ptr), len);
      *consumed = len;
      return Status::Ok();
    }
    default:
      return Status::Error(StatusCode::kNotImplemented,
                           "Unsupported column type in row decoder");
  }
}

struct ColumnInfo {
  enum_field_types type;
  uint meta;
};

Status BuildColumnInfo(const BinlogTableMap &map,
                       std::vector<ColumnInfo> *columns) {
  if (!columns) {
    return Status::Error(StatusCode::kInvalid, "Column output is null");
  }
  columns->clear();
  columns->reserve(map.column_types.size());

  size_t index = 0;
  for (size_t i = 0; i < map.column_types.size(); ++i) {
    enum_field_types binlog_type =
        static_cast<enum_field_types>(map.column_types[i]);
    bool is_array = false;
    if (binlog_type == MYSQL_TYPE_TYPED_ARRAY) {
      if (index >= map.metadata.size()) {
        return Status::Error(StatusCode::kInvalid,
                             "Typed array metadata missing");
      }
      binlog_type = static_cast<enum_field_types>(map.metadata[index++]);
      is_array = true;
    }

    uint meta = 0;
    switch (binlog_type) {
      case MYSQL_TYPE_TINY_BLOB:
      case MYSQL_TYPE_BLOB:
      case MYSQL_TYPE_MEDIUM_BLOB:
      case MYSQL_TYPE_LONG_BLOB:
      case MYSQL_TYPE_DOUBLE:
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_GEOMETRY:
      case MYSQL_TYPE_TIME2:
      case MYSQL_TYPE_DATETIME2:
      case MYSQL_TYPE_TIMESTAMP2:
      case MYSQL_TYPE_JSON: {
        if (index >= map.metadata.size()) {
          return Status::Error(StatusCode::kInvalid,
                               "Column metadata missing");
        }
        meta = map.metadata[index++];
        break;
      }
      case MYSQL_TYPE_SET:
      case MYSQL_TYPE_ENUM:
      case MYSQL_TYPE_STRING: {
        if (index + 1 >= map.metadata.size()) {
          return Status::Error(StatusCode::kInvalid,
                               "Column metadata missing");
        }
        meta = static_cast<uint>(map.metadata[index++]) << 8U;
        meta += map.metadata[index++];
        break;
      }
      case MYSQL_TYPE_BIT: {
        if (index + 1 >= map.metadata.size()) {
          return Status::Error(StatusCode::kInvalid,
                               "Column metadata missing");
        }
        meta = map.metadata[index++];
        meta += static_cast<uint>(map.metadata[index++]) << 8U;
        break;
      }
      case MYSQL_TYPE_VARCHAR: {
        if (is_array) {
          if (index + 2 >= map.metadata.size()) {
            return Status::Error(StatusCode::kInvalid,
                                 "Column metadata missing");
          }
          meta = uint3korr(&map.metadata[index]);
          index += 3;
        } else {
          if (index + 1 >= map.metadata.size()) {
            return Status::Error(StatusCode::kInvalid,
                                 "Column metadata missing");
          }
          meta = uint2korr(&map.metadata[index]);
          index += 2;
        }
        break;
      }
      case MYSQL_TYPE_NEWDECIMAL: {
        if (index + 1 >= map.metadata.size()) {
          return Status::Error(StatusCode::kInvalid,
                               "Column metadata missing");
        }
        meta = static_cast<uint>(map.metadata[index++]) << 8U;
        meta += map.metadata[index++];
        break;
      }
      default:
        meta = 0;
        break;
    }

    enum_field_types type = binlog_type;
    if (binlog_type == MYSQL_TYPE_STRING) {
      const int real_type = meta >> 8;
      if (real_type == MYSQL_TYPE_ENUM || real_type == MYSQL_TYPE_SET) {
        type = static_cast<enum_field_types>(real_type);
      }
    } else if (binlog_type == MYSQL_TYPE_DATE) {
      type = MYSQL_TYPE_NEWDATE;
    }

    columns->push_back(ColumnInfo{type, meta});
  }

  return Status::Ok();
}

Status DecodeRow(const std::vector<ColumnInfo> &columns_info,
                 const std::vector<uint8_t> &columns,
                 const uint8_t *ptr, const uint8_t *end, Row *row,
                 size_t *consumed) {
  if (!row || !consumed) {
    return Status::Error(StatusCode::kInvalid, "Invalid output pointers");
  }
  const size_t column_count = columns_info.size();
  const size_t included = CountIncludedColumns(columns, column_count);
  const size_t null_bytes = (included + 7) / 8;
  if (ptr + null_bytes > end) {
    return Status::Error(StatusCode::kInvalid, "Row null bitmap truncated");
  }
  const uint8_t *row_start = ptr;
  const uint8_t *null_bitmap = ptr;
  ptr += null_bytes;

  row->clear();
  row->reserve(column_count);
  size_t null_bit_index = 0;

  for (size_t i = 0; i < column_count; ++i) {
    if (!BitmapIsSet(columns, i)) {
      row->push_back(Cell{true, false, {}});
      continue;
    }
    const bool is_null =
        (null_bitmap[null_bit_index / 8] & (1u << (null_bit_index % 8))) != 0;
    ++null_bit_index;
    if (is_null) {
      row->push_back(Cell{true, false, {}});
      continue;
    }
    Cell cell;
    size_t cell_size = 0;
    const auto &col = columns_info[i];
    Status st = DecodeCell(col.type, col.meta, ptr, end, &cell, &cell_size);
    if (!st.ok()) return st;
    row->push_back(std::move(cell));
    ptr += cell_size;
  }
  *consumed = static_cast<size_t>(ptr - row_start);
  return Status::Ok();
}

Status DecodeRowsInternal(const BinlogTableMap &map,
                          const std::vector<uint8_t> &columns,
                          const std::vector<uint8_t> &row_data,
                          std::vector<Row> *rows) {
  if (!rows) {
    return Status::Error(StatusCode::kInvalid, "Rows output is null");
  }
  std::vector<ColumnInfo> columns_info;
  Status st = BuildColumnInfo(map, &columns_info);
  if (!st.ok()) return st;

  st = EnsureFullImage(columns, columns_info.size());
  if (!st.ok()) return st;

  const uint8_t *ptr = row_data.data();
  const uint8_t *end = row_data.data() + row_data.size();
  rows->clear();

  while (ptr < end) {
    Row row;
    size_t consumed = 0;
    st = DecodeRow(columns_info, columns, ptr, end, &row, &consumed);
    if (!st.ok()) {
      // If we've decoded at least one row and the remaining data looks like
      // trailing padding (small amount, starting with zeros), ignore it
      const size_t remaining = static_cast<size_t>(end - ptr);
      if (!rows->empty() && remaining <= 4 && *ptr == 0) {
        break;
      }
      return st;
    }
    if (consumed == 0) {
      return Status::Error(StatusCode::kInvalid, "Row decode made no progress");
    }
    ptr += consumed;
    rows->push_back(std::move(row));
  }
  // Allow small amounts of trailing padding (common in binlog events)
  const size_t trailing = static_cast<size_t>(end - ptr);
  if (trailing > 4) {
    return Status::Error(StatusCode::kInvalid,
                         "Row payload length mismatch");
  }
  return Status::Ok();
}

Status DecodeUpdateRowsInternal(
    const BinlogTableMap &map, const std::vector<uint8_t> &columns_before,
    const std::vector<uint8_t> &columns_after,
    const std::vector<uint8_t> &row_data, std::vector<Row> *before_rows,
    std::vector<Row> *after_rows) {
  if (!before_rows || !after_rows) {
    return Status::Error(StatusCode::kInvalid, "Update output is null");
  }
  std::vector<ColumnInfo> columns_info;
  Status st = BuildColumnInfo(map, &columns_info);
  if (!st.ok()) return st;
  st = EnsureFullImage(columns_before, columns_info.size());
  if (!st.ok()) return st;
  st = EnsureFullImage(columns_after, columns_info.size());
  if (!st.ok()) return st;

  const uint8_t *ptr = row_data.data();
  const uint8_t *end = row_data.data() + row_data.size();
  before_rows->clear();
  after_rows->clear();

  while (ptr < end) {
    Row before;
    size_t before_consumed = 0;
    st = DecodeRow(columns_info, columns_before, ptr, end, &before,
                   &before_consumed);
    if (!st.ok()) return st;
    ptr += before_consumed;

    Row after;
    size_t after_consumed = 0;
    st =
        DecodeRow(columns_info, columns_after, ptr, end, &after, &after_consumed);
    if (!st.ok()) return st;
    ptr += after_consumed;

    before_rows->push_back(std::move(before));
    after_rows->push_back(std::move(after));
  }
  if (before_rows->size() != after_rows->size()) {
    return Status::Error(StatusCode::kInvalid,
                         "Update row counts do not match");
  }
  if (ptr != end) {
    return Status::Error(StatusCode::kInvalid,
                         "Update payload length mismatch");
  }
  return Status::Ok();
}

}  // namespace

Status DecodeWriteRows(const BinlogTableMap &map,
                       const std::vector<uint8_t> &columns,
                       const std::vector<uint8_t> &row_data,
                       std::vector<Row> *rows) {
  return DecodeRowsInternal(map, columns, row_data, rows);
}

Status DecodeDeleteRows(const BinlogTableMap &map,
                        const std::vector<uint8_t> &columns,
                        const std::vector<uint8_t> &row_data,
                        std::vector<Row> *rows) {
  return DecodeRowsInternal(map, columns, row_data, rows);
}

Status DecodeUpdateRows(const BinlogTableMap &map,
                        const std::vector<uint8_t> &columns_before,
                        const std::vector<uint8_t> &columns_after,
                        const std::vector<uint8_t> &row_data,
                        std::vector<Row> *before_rows,
                        std::vector<Row> *after_rows) {
  return DecodeUpdateRowsInternal(map, columns_before, columns_after, row_data,
                                  before_rows, after_rows);
}

}  // namespace duckdb_se
