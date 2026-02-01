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

#include <gtest/gtest.h>

#include <ctime>
#include <string>
#include <vector>

#include "my_byteorder.h"
#include "my_time.h"
#include "sql/my_decimal.h"
#include "storage/duckdb/duckdb_row_decoder.h"

// Stub for my_decimal::check_result - minimal implementation for testing
int my_decimal::check_result(uint, int result) const { return result; }

namespace {

using duckdb_se::BinlogTableMap;
using duckdb_se::DecodeWriteRows;
using duckdb_se::Row;
using duckdb_se::Status;

void AppendBytes(std::vector<uint8_t> &out, const void *data, size_t len) {
  const auto *ptr = static_cast<const uint8_t *>(data);
  out.insert(out.end(), ptr, ptr + len);
}

void AppendInt32(std::vector<uint8_t> &out, int32_t value) {
  uchar buf[4];
  int4store(buf, static_cast<uint32_t>(value));
  AppendBytes(out, buf, sizeof(buf));
}

void AppendLengthPrefixedString(std::vector<uint8_t> &out,
                                const std::string &value, uint32 max_len) {
  if (max_len < 256) {
    out.push_back(static_cast<uint8_t>(value.size()));
  } else {
    uchar buf[2];
    int2store(buf, static_cast<uint16>(value.size()));
    AppendBytes(out, buf, sizeof(buf));
  }
  AppendBytes(out, value.data(), value.size());
}

void AppendBlob(std::vector<uint8_t> &out, const std::string &value,
                uint8_t length_bytes) {
  uchar buf[4];
  switch (length_bytes) {
    case 1:
      out.push_back(static_cast<uint8_t>(value.size()));
      break;
    case 2:
      int2store(buf, static_cast<uint16>(value.size()));
      AppendBytes(out, buf, 2);
      break;
    case 3:
      int3store(buf, static_cast<uint32>(value.size()));
      AppendBytes(out, buf, 3);
      break;
    case 4:
      int4store(buf, static_cast<uint32>(value.size()));
      AppendBytes(out, buf, 4);
      break;
    default:
      break;
  }
  AppendBytes(out, value.data(), value.size());
}

std::string FormatTimestamp(my_time_t seconds, uint32 usec, uint decimals) {
  std::tm tm_val{};
  std::time_t sec = static_cast<std::time_t>(seconds);
#ifdef _WIN32
  gmtime_s(&tm_val, &sec);
#else
  gmtime_r(&sec, &tm_val);
#endif
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

TEST(DuckDBRowDecoderTest, DecodeNullsAndBinary) {
  BinlogTableMap map;
  map.column_types = {MYSQL_TYPE_LONG, MYSQL_TYPE_VARCHAR, MYSQL_TYPE_BLOB,
                      MYSQL_TYPE_JSON, MYSQL_TYPE_BIT};
  map.metadata = {0x14, 0x00, 0x02, 0x00, 0x01, 0x01};
  map.null_bitmap = {0x00};

  std::vector<uint8_t> columns = {0x1F};

  std::vector<uint8_t> row_data;
  row_data.push_back(0x08);
  AppendInt32(row_data, 42);
  AppendLengthPrefixedString(row_data, "hello", 20);
  AppendBlob(row_data, std::string("\x01\x02\x03", 3), 2);
  AppendBytes(row_data, "\x01\xFF", 2);

  std::vector<Row> rows;
  Status st = DecodeWriteRows(map, columns, row_data, &rows);
  ASSERT_TRUE(st.ok()) << st.message;
  ASSERT_EQ(1u, rows.size());
  ASSERT_EQ(5u, rows[0].size());

  EXPECT_EQ("42", rows[0][0].value);
  EXPECT_EQ("hello", rows[0][1].value);
  EXPECT_FALSE(rows[0][2].is_null);
  EXPECT_TRUE(rows[0][2].is_blob);
  EXPECT_EQ(std::string("\x01\x02\x03", 3), rows[0][2].value);
  EXPECT_TRUE(rows[0][3].is_null);
  EXPECT_TRUE(rows[0][4].is_blob);
  EXPECT_EQ(std::string("\x01\xFF", 2), rows[0][4].value);
}

// Test decimal and temporal decoding with pre-encoded binary data.
// The encoding is derived from MySQL's binary format for these types.
TEST(DuckDBRowDecoderTest, DecodeDecimalAndTemporal) {
  BinlogTableMap map;
  map.column_types = {MYSQL_TYPE_NEWDECIMAL, MYSQL_TYPE_DATE, MYSQL_TYPE_YEAR};
  // DECIMAL(10,2): metadata = 2 bytes: precision=10, scale=2
  // DATE: no metadata
  // YEAR: no metadata
  map.metadata = {10, 2};  // Two bytes for NEWDECIMAL metadata
  map.null_bitmap = {0x00};

  std::vector<uint8_t> columns = {0x07};  // 3 columns present

  std::vector<uint8_t> row_data;
  row_data.push_back(0x00);  // null bitmap - no nulls

  // Pre-encoded DECIMAL(10,2) value "1234.56"
  // Binary format: 5 bytes for DECIMAL(10,2)
  // Encoded: 0x80 0x00 0x04 0xD2 0x38 (1234.56 in MySQL decimal binary format)
  const uint8_t dec_data[] = {0x80, 0x00, 0x04, 0xD2, 0x38};
  AppendBytes(row_data, dec_data, sizeof(dec_data));

  // Pre-encoded DATE: 2024-12-31
  // Packed: (2024 << 9) | (12 << 5) | 31 = 0x0FD19F stored as 3 bytes LE
  const uint32 date_packed = (2024U << 9) | (12U << 5) | 31U;
  uchar date_buf[3];
  int3store(date_buf, date_packed);
  AppendBytes(row_data, date_buf, sizeof(date_buf));

  // YEAR: 2024 stored as (2024 - 1900) = 124
  row_data.push_back(124);

  std::vector<Row> rows;
  Status st = DecodeWriteRows(map, columns, row_data, &rows);
  ASSERT_TRUE(st.ok()) << st.message;
  ASSERT_EQ(1u, rows.size());
  ASSERT_EQ(3u, rows[0].size());

  EXPECT_EQ("1234.56", rows[0][0].value);
  EXPECT_EQ("2024-12-31", rows[0][1].value);
  EXPECT_EQ("2024", rows[0][2].value);
}

}  // namespace
