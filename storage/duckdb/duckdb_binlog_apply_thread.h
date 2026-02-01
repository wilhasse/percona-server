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

#ifndef PLUGIN_DUCKDB_BINLOG_APPLY_THREAD_H_
#define PLUGIN_DUCKDB_BINLOG_APPLY_THREAD_H_

#include <cstdint>
#include <string>

namespace duckdb_se {

struct BinlogApplyThreadOptions {
  bool enabled{false};
  std::string host{"127.0.0.1"};
  std::string user{"root"};
  std::string password;
  std::string socket;
  uint32_t port{3306};
  uint32_t server_id{0};
  std::string start_gtid_set;
  std::string schema_filter;
};

bool StartBinlogApplyThread(const BinlogApplyThreadOptions &options);
void StopBinlogApplyThread();
bool BinlogApplyThreadRunning();

}  // namespace duckdb_se

#endif  // PLUGIN_DUCKDB_BINLOG_APPLY_THREAD_H_
