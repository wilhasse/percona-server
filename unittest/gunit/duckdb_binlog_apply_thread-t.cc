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

#include <filesystem>
#include <string>

#include "storage/duckdb/duckdb_adapter.h"
#include "storage/duckdb/duckdb_binlog_apply_thread_test.h"
#include "storage/duckdb/duckdb_binlog_applier.h"

// Stub for MySQL memory instrumentation symbol used by binlogevents
#include "mysql/psi/psi_memory.h"
PSI_memory_key key_memory_log_event = 0;

int log_message(int, ...) { return 0; }

char mysql_real_data_home[1] = "";

namespace duckdb_se {

void RegisterLoadedTable(const std::string &, const std::string &,
                         const std::string &) {}

void UnregisterLoadedTable(const std::string &, const std::string &) {}

}  // namespace duckdb_se

namespace {

using duckdb_se::ApplyDdlEventForTest;
using duckdb_se::BinlogApplyThreadOptions;
using duckdb_se::BinlogEvent;
using duckdb_se::DuckDBAdapter;
using duckdb_se::DuckDBBinlogApplier;
using duckdb_se::DuckDBConfig;
using duckdb_se::DuckdbApplyThreadTestHooks;
using duckdb_se::MySQLTableDef;
using duckdb_se::ResetDuckdbApplyThreadTestHooks;
using duckdb_se::SetDuckdbApplyThreadTestHooks;
using duckdb_se::Status;
using duckdb_se::TableId;

std::string TempDirectory() {
  try {
    return std::filesystem::temp_directory_path().string();
  } catch (...) {
    return "/tmp";
  }
}

void CleanupDuckdbFiles(const std::string &path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::remove(path + ".wal", ec);
}

struct RenameCapture {
  std::string from_table;
  std::string to_table;
  int calls{0};
};

RenameCapture g_rename_capture;

Status RenameHook(const BinlogApplyThreadOptions &, const TableId &from,
                  const TableId &to) {
  g_rename_capture.from_table = from.table;
  g_rename_capture.to_table = to.table;
  g_rename_capture.calls++;
  return Status::Ok();
}

TEST(DuckDBBinlogApplyThreadTest, RenameUsesSavedIds) {
  const std::string schema = "duckdb_apply_thread";
  const std::string dir = TempDirectory();
  const std::string path =
      (std::filesystem::path(dir) / (schema + ".duckdb")).string();
  CleanupDuckdbFiles(path);

  DuckDBAdapter adapter;
  DuckDBConfig cfg;
  cfg.read_only = false;
  ASSERT_TRUE(adapter.Init(path, cfg).ok());

  MySQLTableDef def;
  def.schema = schema;
  def.name = "t1";
  def.columns.push_back({"id", "INTEGER", true});
  def.primary_key.push_back("id");
  ASSERT_TRUE(adapter.CreateTable(def).ok());
  adapter.Shutdown();

  g_rename_capture = RenameCapture{};
  DuckdbApplyThreadTestHooks hooks;
  hooks.rename_mysql_table_in_dd = RenameHook;
  SetDuckdbApplyThreadTestHooks(hooks);

  BinlogEvent event;
  event.type = BinlogEvent::Type::kQuery;
  event.schema = schema;
  event.query = "RENAME TABLE " + schema + ".t1 TO " + schema + ".t2";

  BinlogApplyThreadOptions options;
  options.duckdb_dir = dir;
  DuckDBBinlogApplier::Options applier_options;

  Status st = ApplyDdlEventForTest(event, options, applier_options, "test:1");
  ASSERT_TRUE(st.ok()) << st.message;
  EXPECT_EQ(1, g_rename_capture.calls);
  EXPECT_EQ("t1", g_rename_capture.from_table);
  EXPECT_EQ("t2", g_rename_capture.to_table);

  ResetDuckdbApplyThreadTestHooks();
  CleanupDuckdbFiles(path);
}

}  // namespace
