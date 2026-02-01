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

#ifndef PLUGIN_DUCKDB_BINLOG_APPLIER_H_
#define PLUGIN_DUCKDB_BINLOG_APPLIER_H_

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "storage/duckdb/duckdb_adapter.h"

namespace duckdb_se {

struct BinlogApplierOptions {
  size_t max_rows = 50000;
  size_t max_bytes = 64 * 1024 * 1024;
  std::chrono::milliseconds max_delay = std::chrono::milliseconds(200);
};

struct BinlogApplyControls {
  bool paused{false};
  uint64_t throttle_rows_per_sec{0};
  uint64_t throttle_bytes_per_sec{0};
  uint64_t lag_alert_threshold_ms{0};
  std::string stop_at_gtid;
};

struct BinlogApplyMetrics {
  uint64_t applied_transactions{0};
  uint64_t applied_rows{0};
  uint64_t applied_bytes{0};
  uint64_t last_flush_rows{0};
  uint64_t last_flush_bytes{0};
  uint64_t last_flush_ms{0};
  uint64_t last_throttle_ms{0};
  uint64_t total_throttle_ms{0};
  uint64_t last_commit_epoch_ms{0};
  uint64_t last_commit_ms{0};
  uint64_t lag_ms{0};
  bool lag_alert{false};
  std::string last_gtid;
};

BinlogApplyControls GetBinlogApplyControls();
BinlogApplyMetrics GetBinlogApplyMetrics();
void SetBinlogApplyPaused(bool paused);
void SetBinlogApplyThrottleRowsPerSec(uint64_t rows_per_sec);
void SetBinlogApplyThrottleBytesPerSec(uint64_t bytes_per_sec);
void SetBinlogApplyLagAlertThresholdMs(uint64_t threshold_ms);
void SetBinlogApplyStopAtGtid(const std::string &gtid);

class DuckDBBinlogApplier {
 public:
  using Options = BinlogApplierOptions;

  explicit DuckDBBinlogApplier(DuckDBAdapter *adapter,
                               Options options = Options{});

  Status BeginTransaction(Gtid gtid);
  Status AppendInsert(TableId table, Row row);
  Status AppendInsertBatch(TableId table, RowBatch batch);
  Status AddUpdateStatement(TableId table, std::string sql);
  Status AddDeleteStatement(TableId table, std::string sql);
  Status ApplyDDL(DDLChange change);
  Status CommitTransaction();
  Status RollbackTransaction();

 private:
  struct TableKey {
    std::string schema;
    std::string table;

    bool operator<(const TableKey &other) const {
      if (schema != other.schema) return schema < other.schema;
      return table < other.table;
    }
  };

  struct TableBuffer {
    RowBatch inserts;
    UpdateBatch updates;
    DeleteBatch deletes;
    BulkUpdateBatch bulk_updates;
    BulkDeleteBatch bulk_deletes;
    std::vector<std::string> columns;
    std::map<std::string, size_t> column_index;
    std::map<uint64_t, std::vector<size_t>> bulk_update_old_hashes;
    std::map<uint64_t, std::vector<size_t>> bulk_update_new_hashes;
    bool columns_loaded{false};
    size_t row_count{0};
    size_t byte_count{0};
  };

  Status EnsureApplyTxn();
  Status FlushBuffered(bool force);
  bool ShouldFlush() const;
  void WaitIfPaused() const;
  void UpdateMetrics(size_t rows, size_t bytes,
                     std::chrono::milliseconds apply_ms);
  void MaybeThrottle(size_t rows, size_t bytes,
                     std::chrono::milliseconds apply_ms);
  void ResetBuffers();
  Status ApplyWatermark();
  std::string EscapeLiteral(const std::string &value) const;

  DuckDBAdapter *adapter_{nullptr};
  Options options_{};
  bool in_txn_{false};
  bool skip_txn_{false};
  bool have_buffered_{false};
  Gtid current_gtid_{};
  ApplyTxn apply_txn_{};
  std::chrono::steady_clock::time_point first_event_time_{};

  size_t buffered_rows_{0};
  size_t buffered_bytes_{0};
  std::map<TableKey, TableBuffer> buffers_;
};

}  // namespace duckdb_se

#endif  // PLUGIN_DUCKDB_BINLOG_APPLIER_H_
