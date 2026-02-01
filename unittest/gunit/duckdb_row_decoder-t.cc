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

#include "decimal.h"
#include "my_byteorder.h"
#include "my_time.h"
#include "sql/my_decimal.h"
#include "storage/duckdb/duckdb_row_decoder.h"

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

TEST(DuckDBRowDecoderTest, DecodeDecimalAndTemporal) {
  BinlogTableMap map;
  map.column_types = {MYSQL_TYPE_NEWDECIMAL, MYSQL_TYPE_DATE,
                      MYSQL_TYPE_DATETIME2,  MYSQL_TYPE_TIME2,
                      MYSQL_TYPE_TIMESTAMP2, MYSQL_TYPE_YEAR};
  map.metadata = {10, 2, 3, 3, 6};
  map.null_bitmap = {0x00};

  std::vector<uint8_t> columns = {0x3F};

  std::vector<uint8_t> row_data;
  row_data.push_back(0x00);

  my_decimal dec;
  const char *dec_end = nullptr;
  ASSERT_EQ(E_DEC_OK,
            str2my_decimal(E_DEC_FATAL_ERROR, "1234.56", &dec, &dec_end));
  const int dec_size = my_decimal_get_binary_size(10, 2);
  std::vector<uint8_t> dec_buf(static_cast<size_t>(dec_size));
  ASSERT_EQ(E_DEC_OK, my_decimal2binary(E_DEC_FATAL_ERROR, &dec, dec_buf.data(),
                                       10, 2));
  AppendBytes(row_data, dec_buf.data(), dec_buf.size());

  const uint32 date_packed = (2024U << 9) | (12U << 5) | 31U;
  uchar date_buf[3];
  int3store(date_buf, date_packed);
  AppendBytes(row_data, date_buf, sizeof(date_buf));

  MYSQL_TIME dt{};
  dt.year = 2024;
  dt.month = 12;
  dt.day = 31;
  dt.hour = 23;
  dt.minute = 59;
  dt.second = 59;
  dt.second_part = 123000;
  dt.time_type = MYSQL_TIMESTAMP_DATETIME;
  const longlong dt_packed = TIME_to_longlong_datetime_packed(dt);
  const uint32 dt_len = my_datetime_binary_length(3);
  std::vector<uint8_t> dt_buf(dt_len);
  my_datetime_packed_to_binary(dt_packed, dt_buf.data(), 3);
  AppendBytes(row_data, dt_buf.data(), dt_buf.size());

  MYSQL_TIME tm{};
  tm.hour = 12;
  tm.minute = 34;
  tm.second = 56;
  tm.second_part = 789000;
  tm.time_type = MYSQL_TIMESTAMP_TIME;
  const longlong tm_packed = TIME_to_longlong_time_packed(tm);
  const uint32 tm_len = my_time_binary_length(3);
  std::vector<uint8_t> tm_buf(tm_len);
  my_time_packed_to_binary(tm_packed, tm_buf.data(), 3);
  AppendBytes(row_data, tm_buf.data(), tm_buf.size());

  my_timeval tv{};
  tv.m_tv_sec = 1704067200;
  tv.m_tv_usec = 123456;
  const uint32 ts_len = my_timestamp_binary_length(6);
  std::vector<uint8_t> ts_buf(ts_len);
  my_timestamp_to_binary(&tv, ts_buf.data(), 6);
  AppendBytes(row_data, ts_buf.data(), ts_buf.size());

  row_data.push_back(static_cast<uint8_t>(2024 - 1900));

  std::vector<Row> rows;
  Status st = DecodeWriteRows(map, columns, row_data, &rows);
  ASSERT_TRUE(st.ok()) << st.message;
  ASSERT_EQ(1u, rows.size());
  ASSERT_EQ(6u, rows[0].size());

  EXPECT_EQ("1234.56", rows[0][0].value);

  MYSQL_TIME date{};
  date.year = 2024;
  date.month = 12;
  date.day = 31;
  date.time_type = MYSQL_TIMESTAMP_DATE;
  char date_buf_str[MAX_DATE_STRING_REP_LENGTH];
  const int date_len = my_date_to_str(date, date_buf_str);
  EXPECT_EQ(std::string(date_buf_str, static_cast<size_t>(date_len)),
            rows[0][1].value);

  char dt_buf_str[MAX_DATE_STRING_REP_LENGTH];
  const int dt_len_str = my_datetime_to_str(dt, dt_buf_str, 3);
  EXPECT_EQ(std::string(dt_buf_str, static_cast<size_t>(dt_len_str)),
            rows[0][2].value);

  char tm_buf_str[MAX_DATE_STRING_REP_LENGTH];
  const int tm_len_str = my_time_to_str(tm, tm_buf_str, 3);
  EXPECT_EQ(std::string(tm_buf_str, static_cast<size_t>(tm_len_str)),
            rows[0][3].value);

  EXPECT_EQ(FormatTimestamp(tv.m_tv_sec, tv.m_tv_usec, 6), rows[0][4].value);
  EXPECT_EQ("2024", rows[0][5].value);
}

}  // namespace
