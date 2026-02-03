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

#ifndef PLUGIN_DUCKDB_BINLOG_APPLY_THREAD_TEST_H_
#define PLUGIN_DUCKDB_BINLOG_APPLY_THREAD_TEST_H_

#include <string>

#include "storage/duckdb/duckdb_adapter.h"
#include "storage/duckdb/duckdb_binlog_apply_thread.h"
#include "storage/duckdb/duckdb_binlog_applier.h"
#include "storage/duckdb/duckdb_binlog_streamer.h"

namespace duckdb_se {

struct DuckdbApplyThreadTestHooks {
  using EnsureMySQLTableFn =
      Status (*)(const BinlogApplyThreadOptions &, const MySQLTableDef &, bool);
  using DropMySQLTableFn =
      Status (*)(const std::string &, const std::string &);
  using RenameMySQLTableFn =
      Status (*)(const BinlogApplyThreadOptions &, const TableId &,
                 const TableId &);
  using TruncateMySQLTableFn =
      Status (*)(const std::string &, const std::string &);
  using FetchTableDefFn =
      Status (*)(const BinlogApplyThreadOptions &, const std::string &,
                 const std::string &, MySQLTableDef *);

  EnsureMySQLTableFn ensure_mysql_table_in_dd{nullptr};
  DropMySQLTableFn drop_mysql_table_in_dd{nullptr};
  RenameMySQLTableFn rename_mysql_table_in_dd{nullptr};
  TruncateMySQLTableFn truncate_mysql_table_in_dd{nullptr};
  FetchTableDefFn fetch_table_def_from_source{nullptr};
};

void SetDuckdbApplyThreadTestHooks(const DuckdbApplyThreadTestHooks &hooks);
void ResetDuckdbApplyThreadTestHooks();

Status ApplyDdlEventForTest(
    const BinlogEvent &event, const BinlogApplyThreadOptions &options,
    const DuckDBBinlogApplier::Options &applier_options, const std::string &gtid);

}  // namespace duckdb_se

#endif  // PLUGIN_DUCKDB_BINLOG_APPLY_THREAD_TEST_H_
