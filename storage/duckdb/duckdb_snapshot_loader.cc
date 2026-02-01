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

#include <mysql.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "storage/duckdb/duckdb_adapter.h"

namespace {

using duckdb_se::ApplyTxn;
using duckdb_se::Cell;
using duckdb_se::DuckDBAdapter;
using duckdb_se::DuckDBConfig;
using duckdb_se::Gtid;
using duckdb_se::MySQLTableDef;
using duckdb_se::Row;
using duckdb_se::RowBatch;
using duckdb_se::Status;
using duckdb_se::TableId;

struct Options {
  std::string host{"127.0.0.1"};
  std::string user{"root"};
  std::string password;
  std::string socket;
  unsigned int port{3306};
  std::string schema;
  std::string table;
  std::string duckdb_path;
  std::string duckdb_dir;
  size_t batch_rows{1000};
  bool overwrite{false};
  bool verbose{false};
  bool validate{false};
  bool validate_sum{false};
  bool show_help{false};
};

void PrintUsage(const char *argv0) {
  std::cerr
      << "Usage: " << argv0
      << " --schema <schema> [--table <table>] "
         "--duckdb-path <path>|--duckdb-dir <dir> [options]\n\n"
         "Options:\n"
         "  --host <host>            Source MySQL host (default 127.0.0.1)\n"
         "  --port <port>            Source MySQL port (default 3306)\n"
         "  --user <user>            Source MySQL user (default root)\n"
         "  --password <pass>        Source MySQL password\n"
         "  --socket <path>          Source MySQL socket\n"
         "  --schema <schema>        Schema to load (required)\n"
         "  --table <table>          Load a single table only\n"
         "  --duckdb-path <path>     DuckDB database file path\n"
         "  --duckdb-dir <dir>       Directory for per-schema DuckDB file\n"
         "  --batch <rows>           Appender batch size (default 1000)\n"
         "  --overwrite              Drop table if it exists\n"
         "  --validate               Validate row counts after load\n"
         "  --validate-sum           Validate SUM() for up to 3 numeric columns\n"
         "  --verbose                Verbose output\n"
         "  --help                   Show this help\n";
}

int GetPid() {
#ifdef _WIN32
  return _getpid();
#else
  return ::getpid();
#endif
}

bool StartsWith(const std::string &value, const std::string &prefix) {
  return value.compare(0, prefix.size(), prefix) == 0;
}

bool ParseSizeT(const std::string &value, size_t *out) {
  if (!out) return false;
  errno = 0;
  char *end = nullptr;
  unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
  if (errno != 0 || end == value.c_str() || *end != '\0') return false;
  *out = static_cast<size_t>(parsed);
  return true;
}

bool ParseUInt(const std::string &value, unsigned int *out) {
  if (!out) return false;
  errno = 0;
  char *end = nullptr;
  unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
  if (errno != 0 || end == value.c_str() || *end != '\0') return false;
  *out = static_cast<unsigned int>(parsed);
  return true;
}

bool ParseArgs(int argc, char **argv, Options *opts) {
  if (!opts) return false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      opts->show_help = true;
      return true;
    }
    auto consume_value = [&](std::string *out) -> bool {
      if (i + 1 >= argc) return false;
      *out = argv[++i];
      return true;
    };
    auto consume_uint = [&](unsigned int *out) -> bool {
      if (i + 1 >= argc) return false;
      return ParseUInt(argv[++i], out);
    };
    auto consume_size = [&](size_t *out) -> bool {
      if (i + 1 >= argc) return false;
      return ParseSizeT(argv[++i], out);
    };

    if (arg == "--host") {
      if (!consume_value(&opts->host)) return false;
      continue;
    }
    if (arg == "--port") {
      if (!consume_uint(&opts->port)) return false;
      continue;
    }
    if (arg == "--user") {
      if (!consume_value(&opts->user)) return false;
      continue;
    }
    if (arg == "--password") {
      if (!consume_value(&opts->password)) return false;
      continue;
    }
    if (arg == "--socket") {
      if (!consume_value(&opts->socket)) return false;
      continue;
    }
    if (arg == "--schema") {
      if (!consume_value(&opts->schema)) return false;
      continue;
    }
    if (arg == "--table") {
      if (!consume_value(&opts->table)) return false;
      continue;
    }
    if (arg == "--duckdb-path") {
      if (!consume_value(&opts->duckdb_path)) return false;
      continue;
    }
    if (arg == "--duckdb-dir") {
      if (!consume_value(&opts->duckdb_dir)) return false;
      continue;
    }
    if (arg == "--batch") {
      if (!consume_size(&opts->batch_rows)) return false;
      continue;
    }
    if (arg == "--overwrite") {
      opts->overwrite = true;
      continue;
    }
    if (arg == "--verbose") {
      opts->verbose = true;
      continue;
    }
    if (arg == "--validate") {
      opts->validate = true;
      continue;
    }
    if (arg == "--validate-sum") {
      opts->validate = true;
      opts->validate_sum = true;
      continue;
    }

    if (StartsWith(arg, "--host=")) {
      opts->host = arg.substr(7);
      continue;
    }
    if (StartsWith(arg, "--port=")) {
      if (!ParseUInt(arg.substr(7), &opts->port)) return false;
      continue;
    }
    if (StartsWith(arg, "--user=")) {
      opts->user = arg.substr(7);
      continue;
    }
    if (StartsWith(arg, "--password=")) {
      opts->password = arg.substr(11);
      continue;
    }
    if (StartsWith(arg, "--socket=")) {
      opts->socket = arg.substr(9);
      continue;
    }
    if (StartsWith(arg, "--schema=")) {
      opts->schema = arg.substr(9);
      continue;
    }
    if (StartsWith(arg, "--table=")) {
      opts->table = arg.substr(8);
      continue;
    }
    if (StartsWith(arg, "--duckdb-path=")) {
      opts->duckdb_path = arg.substr(14);
      continue;
    }
    if (StartsWith(arg, "--duckdb-dir=")) {
      opts->duckdb_dir = arg.substr(13);
      continue;
    }
    if (StartsWith(arg, "--batch=")) {
      if (!ParseSizeT(arg.substr(8), &opts->batch_rows)) return false;
      continue;
    }

    std::cerr << "Unknown argument: " << arg << "\n";
    return false;
  }

  if (opts->show_help) return true;
  if (opts->schema.empty()) {
    std::cerr << "Missing required --schema\n";
    return false;
  }
  if (opts->duckdb_path.empty() && opts->duckdb_dir.empty()) {
    std::cerr << "Either --duckdb-path or --duckdb-dir is required\n";
    return false;
  }
  if (opts->batch_rows == 0) {
    std::cerr << "--batch must be > 0\n";
    return false;
  }

  return true;
}

std::string QuoteDuckdbIdent(const std::string &value) {
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('"');
  for (char ch : value) {
    if (ch == '"') out.push_back('"');
    out.push_back(ch);
  }
  out.push_back('"');
  return out;
}

std::string QuoteMySQLIdent(const std::string &value) {
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('`');
  for (char ch : value) {
    if (ch == '`') out.push_back('`');
    out.push_back(ch);
  }
  out.push_back('`');
  return out;
}

std::string QuoteMySQLLiteral(MYSQL *mysql, const std::string &value) {
  std::string escaped;
  escaped.resize(value.size() * 2 + 1);
  const unsigned long len = mysql_real_escape_string(
      mysql, &escaped[0], value.c_str(), static_cast<unsigned long>(value.size()));
  escaped.resize(len);
  return "'" + escaped + "'";
}

std::string QuoteDuckdbLiteral(const std::string &value) {
  std::string escaped;
  escaped.reserve(value.size() + 2);
  for (char ch : value) {
    if (ch == '\'') escaped.push_back('\'');
    escaped.push_back(ch);
  }
  return "'" + escaped + "'";
}

std::string MakeLoadingTableName(const std::string &base) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::ostringstream oss;
  oss << "__loading_" << base << "_" << GetPid() << "_" << now;
  return oss.str();
}

std::string QualifiedDuckdbName(const std::string &schema,
                                const std::string &table) {
  if (schema.empty()) return QuoteDuckdbIdent(table);
  return QuoteDuckdbIdent(schema) + "." + QuoteDuckdbIdent(table);
}

bool ExecQuery(MYSQL *mysql, const std::string &sql) {
  if (mysql_real_query(mysql, sql.c_str(), sql.size()) != 0) {
    std::cerr << "MySQL query failed: " << mysql_error(mysql) << "\n";
    return false;
  }
  if (mysql_field_count(mysql) == 0) return true;
  MYSQL_RES *res = mysql_store_result(mysql);
  if (!res) {
    std::cerr << "MySQL store_result failed: " << mysql_error(mysql) << "\n";
    return false;
  }
  mysql_free_result(res);
  return true;
}

bool DuckdbTableExists(DuckDBAdapter &adapter, const std::string &schema,
                       const std::string &table, bool *exists) {
  if (!exists) return false;
  const std::string sql =
      "SELECT 1 FROM information_schema.tables WHERE table_schema = " +
      QuoteDuckdbLiteral(schema) + " AND table_name = " +
      QuoteDuckdbLiteral(table) + " LIMIT 1";
  auto result = adapter.ExecuteQuery(sql, {});
  if (!result.ok) {
    std::cerr << "DuckDB table check failed: " << result.error << "\n";
    return false;
  }
  auto chunk = result.result->Fetch();
  *exists = (chunk && chunk->size() > 0);
  return true;
}

bool QuerySingleValue(MYSQL *mysql, const std::string &sql,
                      std::string *out) {
  if (!out) return false;
  if (mysql_real_query(mysql, sql.c_str(), sql.size()) != 0) {
    std::cerr << "MySQL query failed: " << mysql_error(mysql) << "\n";
    return false;
  }
  MYSQL_RES *res = mysql_store_result(mysql);
  if (!res) {
    std::cerr << "MySQL store_result failed: " << mysql_error(mysql) << "\n";
    return false;
  }
  MYSQL_ROW row = mysql_fetch_row(res);
  if (row && row[0]) {
    const unsigned long *lengths = mysql_fetch_lengths(res);
    *out = std::string(row[0], lengths ? lengths[0] : std::strlen(row[0]));
  } else {
    out->clear();
  }
  mysql_free_result(res);
  return true;
}

bool ParseLongLong(const std::string &value, long long *out) {
  if (!out) return false;
  if (value.empty()) return false;
  errno = 0;
  char *end = nullptr;
  long long parsed = std::strtoll(value.c_str(), &end, 10);
  if (errno != 0 || end == value.c_str() || *end != '\0') return false;
  *out = parsed;
  return true;
}

bool ParseLongDouble(const std::string &value, long double *out) {
  if (!out) return false;
  if (value.empty()) return false;
  errno = 0;
  char *end = nullptr;
  long double parsed = std::strtold(value.c_str(), &end);
  if (errno != 0 || end == value.c_str() || *end != '\0') return false;
  *out = parsed;
  return true;
}

bool DuckdbQuerySingleValue(DuckDBAdapter &adapter, const std::string &sql,
                            std::string *out) {
  if (!out) return false;
  auto result = adapter.ExecuteQuery(sql, {});
  if (!result.ok) {
    std::cerr << "DuckDB query failed: " << result.error << "\n";
    return false;
  }
  auto chunk = result.result->Fetch();
  if (!chunk || chunk->size() == 0) {
    out->clear();
    return true;
  }
  auto val = chunk->GetValue(0, 0);
  if (val.IsNull()) {
    out->clear();
  } else {
    *out = val.ToString();
  }
  return true;
}

bool ListTables(MYSQL *mysql, const std::string &schema,
                std::vector<std::string> *tables) {
  if (!tables) return false;
  tables->clear();
  const std::string sql =
      "SELECT TABLE_NAME FROM INFORMATION_SCHEMA.TABLES "
      "WHERE TABLE_SCHEMA = " +
      QuoteMySQLLiteral(mysql, schema) +
      " AND TABLE_TYPE = 'BASE TABLE' "
      "ORDER BY TABLE_NAME";
  if (mysql_real_query(mysql, sql.c_str(), sql.size()) != 0) {
    std::cerr << "MySQL query failed: " << mysql_error(mysql) << "\n";
    return false;
  }
  MYSQL_RES *res = mysql_store_result(mysql);
  if (!res) {
    std::cerr << "MySQL store_result failed: " << mysql_error(mysql) << "\n";
    return false;
  }
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    if (row[0]) tables->push_back(row[0]);
  }
  mysql_free_result(res);
  return true;
}

std::string UpperAscii(std::string input) {
  for (char &ch : input) {
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }
  return input;
}

std::string BaseType(const std::string &type) {
  std::string upper = UpperAscii(type);
  const size_t end = upper.find_first_of(" (");
  return end == std::string::npos ? upper : upper.substr(0, end);
}

bool IsNumericType(const std::string &type) {
  const std::string base = BaseType(type);
  return base == "TINYINT" || base == "SMALLINT" || base == "MEDIUMINT" ||
         base == "INT" || base == "INTEGER" || base == "BIGINT" ||
         base == "DECIMAL" || base == "NUMERIC" || base == "FLOAT" ||
         base == "DOUBLE" || base == "REAL" || base == "YEAR";
}

bool IsBlobTypeString(const std::string &type) {
  std::string upper = type;
  std::transform(upper.begin(), upper.end(), upper.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
  const size_t end = upper.find_first_of(" (");
  const std::string base =
      end == std::string::npos ? upper : upper.substr(0, end);
  if (base == "BLOB" || base == "TINYBLOB" || base == "MEDIUMBLOB" ||
      base == "LONGBLOB" || base == "BINARY" || base == "VARBINARY" ||
      base == "BIT" || base == "GEOMETRY") {
    return true;
  }
  return false;
}

bool IsBlobField(const MYSQL_FIELD &field) {
  switch (field.type) {
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BIT:
    case MYSQL_TYPE_GEOMETRY:
    case MYSQL_TYPE_JSON:
      return true;
    default:
      break;
  }
  return (field.flags & BINARY_FLAG) != 0;
}

bool FetchTableDef(MYSQL *mysql, const std::string &schema,
                   const std::string &table, MySQLTableDef *def,
                   std::vector<bool> *blob_flags) {
  if (!def || !blob_flags) return false;
  def->schema = schema;
  def->name = table;
  def->columns.clear();
  blob_flags->clear();

  const std::string sql =
      "SELECT COLUMN_NAME, COLUMN_TYPE, IS_NULLABLE "
      "FROM INFORMATION_SCHEMA.COLUMNS "
      "WHERE TABLE_SCHEMA = " +
      QuoteMySQLLiteral(mysql, schema) +
      " AND TABLE_NAME = " + QuoteMySQLLiteral(mysql, table) +
      " ORDER BY ORDINAL_POSITION";

  if (mysql_real_query(mysql, sql.c_str(), sql.size()) != 0) {
    std::cerr << "MySQL query failed: " << mysql_error(mysql) << "\n";
    return false;
  }
  MYSQL_RES *res = mysql_store_result(mysql);
  if (!res) {
    std::cerr << "MySQL store_result failed: " << mysql_error(mysql) << "\n";
    return false;
  }
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    if (!row[0] || !row[1] || !row[2]) continue;
    duckdb_se::ColumnDef col;
    col.name = row[0];
    col.type = row[1];
    col.not_null = std::strcmp(row[2], "NO") == 0;
    def->columns.push_back(col);
    blob_flags->push_back(IsBlobTypeString(col.type));
  }
  mysql_free_result(res);
  return !def->columns.empty();
}

bool EnsureDuckdbSchema(DuckDBAdapter &adapter, const std::string &schema) {
  if (schema.empty()) return true;
  const std::string sql =
      "CREATE SCHEMA IF NOT EXISTS " + QuoteDuckdbIdent(schema);
  auto result = adapter.ExecuteQuery(sql, {});
  if (!result.ok) {
    std::cerr << "DuckDB schema create failed: " << result.error << "\n";
    return false;
  }
  return true;
}

bool StoreSnapshotGtid(DuckDBAdapter &adapter, const std::string &gtid) {
  auto create = adapter.ExecuteQuery(
      "CREATE TABLE IF NOT EXISTS __repl_state ("
      "id INTEGER PRIMARY KEY, "
      "snapshot_gtid_set VARCHAR, "
      "updated_ts TIMESTAMP)",
      {});
  if (!create.ok) {
    std::cerr << "DuckDB __repl_state create failed: " << create.error << "\n";
    return false;
  }
  const std::string sql =
      "INSERT INTO __repl_state (id, snapshot_gtid_set, updated_ts) "
      "VALUES (1, " +
      QuoteDuckdbLiteral(gtid) + ", CURRENT_TIMESTAMP) "
      "ON CONFLICT(id) DO UPDATE SET "
      "snapshot_gtid_set = excluded.snapshot_gtid_set, "
      "updated_ts = excluded.updated_ts";
  auto upsert = adapter.ExecuteQuery(sql, {});
  if (!upsert.ok) {
    std::cerr << "DuckDB __repl_state upsert failed: " << upsert.error << "\n";
    return false;
  }
  return true;
}

bool ValidateRowCount(MYSQL *mysql, DuckDBAdapter &adapter,
                      const std::string &schema, const std::string &table,
                      bool *ok) {
  if (!ok) return false;
  *ok = false;
  const std::string mysql_sql =
      "SELECT COUNT(*) FROM " + QuoteMySQLIdent(schema) + "." +
      QuoteMySQLIdent(table);
  std::string mysql_val;
  if (!QuerySingleValue(mysql, mysql_sql, &mysql_val)) return false;
  const std::string duckdb_sql =
      "SELECT COUNT(*) FROM " + QualifiedDuckdbName(schema, table);
  std::string duckdb_val;
  if (!DuckdbQuerySingleValue(adapter, duckdb_sql, &duckdb_val)) return false;
  long long mysql_count = 0;
  long long duckdb_count = 0;
  if (!ParseLongLong(mysql_val, &mysql_count) ||
      !ParseLongLong(duckdb_val, &duckdb_count)) {
    std::cerr << "Validation count parse failed for " << schema << "." << table
              << "\n";
    return false;
  }
  if (mysql_count != duckdb_count) {
    std::cerr << "Validation mismatch for " << schema << "." << table
              << " count: mysql=" << mysql_count
              << " duckdb=" << duckdb_count << "\n";
    return true;
  }
  *ok = true;
  return true;
}

bool ValidateSums(MYSQL *mysql, DuckDBAdapter &adapter,
                  const std::string &schema, const MySQLTableDef &def,
                  bool *ok) {
  if (!ok) return false;
  *ok = true;
  const size_t max_cols = 3;
  size_t checked = 0;
  for (const auto &col : def.columns) {
    if (!IsNumericType(col.type)) continue;
    const std::string mysql_sql =
        "SELECT SUM(" + QuoteMySQLIdent(col.name) + ") FROM " +
        QuoteMySQLIdent(schema) + "." + QuoteMySQLIdent(def.name);
    const std::string duckdb_sql =
        "SELECT SUM(" + QuoteDuckdbIdent(col.name) + ") FROM " +
        QualifiedDuckdbName(schema, def.name);
    std::string mysql_val;
    std::string duckdb_val;
    if (!QuerySingleValue(mysql, mysql_sql, &mysql_val)) return false;
    if (!DuckdbQuerySingleValue(adapter, duckdb_sql, &duckdb_val)) return false;
    if (mysql_val.empty() && duckdb_val.empty()) {
      ++checked;
      continue;
    }
    if (mysql_val.empty() || duckdb_val.empty()) {
      std::cerr << "Validation mismatch for " << schema << "." << def.name
                << "." << col.name << " sum: mysql=" << mysql_val
                << " duckdb=" << duckdb_val << "\n";
      *ok = false;
      ++checked;
      continue;
    }
    long double mysql_sum = 0.0;
    long double duckdb_sum = 0.0;
    if (!ParseLongDouble(mysql_val, &mysql_sum) ||
        !ParseLongDouble(duckdb_val, &duckdb_sum)) {
      std::cerr << "Validation sum parse failed for " << schema << "."
                << def.name << "." << col.name << "\n";
      return false;
    }
    const long double diff = std::fabs(mysql_sum - duckdb_sum);
    const long double tol = 1e-6L;
    if (diff > tol) {
      std::cerr << "Validation mismatch for " << schema << "." << def.name
                << "." << col.name << " sum: mysql=" << mysql_val
                << " duckdb=" << duckdb_val << "\n";
      *ok = false;
    }
    ++checked;
    if (checked >= max_cols) break;
  }
  return true;
}

bool SwapInTable(DuckDBAdapter &adapter, const std::string &schema,
                 const std::string &temp_table,
                 const std::string &real_table) {
  const std::string qualified_temp = QualifiedDuckdbName(schema, temp_table);
  const std::string qualified_real = QualifiedDuckdbName(schema, real_table);

  auto begin = adapter.ExecuteQuery("BEGIN TRANSACTION", {});
  if (!begin.ok) {
    std::cerr << "DuckDB begin transaction failed: " << begin.error << "\n";
    return false;
  }
  auto drop = adapter.ExecuteQuery("DROP TABLE IF EXISTS " + qualified_real, {});
  if (!drop.ok) {
    std::cerr << "DuckDB drop failed: " << drop.error << "\n";
    adapter.ExecuteQuery("ROLLBACK", {});
    return false;
  }
  const std::string rename_sql =
      "ALTER TABLE " + qualified_temp + " RENAME TO " +
      QuoteDuckdbIdent(real_table);
  auto rename = adapter.ExecuteQuery(rename_sql, {});
  if (!rename.ok) {
    std::cerr << "DuckDB rename failed: " << rename.error << "\n";
    adapter.ExecuteQuery("ROLLBACK", {});
    return false;
  }
  auto commit = adapter.ExecuteQuery("COMMIT", {});
  if (!commit.ok) {
    std::cerr << "DuckDB commit failed: " << commit.error << "\n";
    adapter.ExecuteQuery("ROLLBACK", {});
    return false;
  }
  return true;
}

bool StreamTable(MYSQL *mysql, DuckDBAdapter &adapter,
                 const std::string &schema, const std::string &source_table,
                 const std::string &dest_table,
                 const std::vector<bool> &blob_flags, size_t batch_rows,
                 size_t *row_count) {
  if (row_count) *row_count = 0;
  const std::string sql = "SELECT * FROM " + QuoteMySQLIdent(schema) + "." +
                          QuoteMySQLIdent(source_table);
  if (mysql_real_query(mysql, sql.c_str(), sql.size()) != 0) {
    std::cerr << "MySQL query failed: " << mysql_error(mysql) << "\n";
    return false;
  }
  MYSQL_RES *res = mysql_use_result(mysql);
  if (!res) {
    std::cerr << "MySQL use_result failed: " << mysql_error(mysql) << "\n";
    return false;
  }

  const unsigned int num_fields = mysql_num_fields(res);
  std::vector<bool> blobs = blob_flags;
  if (blobs.size() != num_fields) {
    blobs.assign(num_fields, false);
    MYSQL_FIELD *fields = mysql_fetch_fields(res);
    for (unsigned int i = 0; i < num_fields; ++i) {
      blobs[i] = IsBlobField(fields[i]);
    }
  }

  ApplyTxn txn =
      adapter.BeginApplyTxn(Gtid{"snapshot:" + schema + "." + dest_table});
  if (!txn.status.ok()) {
    std::cerr << "DuckDB begin transaction failed: " << txn.status.message
              << "\n";
    mysql_free_result(res);
    return false;
  }

  TableId table_id{schema, dest_table};
  RowBatch batch;
  batch.table = table_id;
  batch.rows.reserve(batch_rows);

  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    const unsigned long *lengths = mysql_fetch_lengths(res);
    Row out;
    out.reserve(num_fields);
    for (unsigned int i = 0; i < num_fields; ++i) {
      Cell cell;
      if (!row[i]) {
        cell.is_null = true;
        cell.is_blob = false;
      } else {
        const unsigned long len = lengths ? lengths[i]
                                           : static_cast<unsigned long>(
                                                 std::strlen(row[i]));
        cell.is_null = false;
        cell.is_blob = blobs[i];
        cell.value.assign(row[i], len);
      }
      out.push_back(std::move(cell));
    }
    batch.rows.push_back(std::move(out));
    if (batch.rows.size() >= batch_rows) {
      Status st = adapter.AppendRows(txn, table_id, std::move(batch));
      if (!st.ok()) {
        std::cerr << "DuckDB append failed: " << st.message << "\n";
        adapter.RollbackApplyTxn(txn);
        mysql_free_result(res);
        return false;
      }
      if (row_count) *row_count += batch_rows;
      batch = RowBatch{};
      batch.table = table_id;
      batch.rows.reserve(batch_rows);
    }
  }

  if (mysql_errno(mysql) != 0) {
    std::cerr << "MySQL fetch failed: " << mysql_error(mysql) << "\n";
    adapter.RollbackApplyTxn(txn);
    mysql_free_result(res);
    return false;
  }

  if (!batch.rows.empty()) {
    const size_t chunk = batch.rows.size();
    Status st = adapter.AppendRows(txn, table_id, std::move(batch));
    if (!st.ok()) {
      std::cerr << "DuckDB append failed: " << st.message << "\n";
      adapter.RollbackApplyTxn(txn);
      mysql_free_result(res);
      return false;
    }
    if (row_count) *row_count += chunk;
  }

  mysql_free_result(res);
  Status st = adapter.CommitApplyTxn(txn);
  if (!st.ok()) {
    std::cerr << "DuckDB commit failed: " << st.message << "\n";
    return false;
  }
  return true;
}

std::string ResolveDuckdbPath(const Options &opts) {
  if (!opts.duckdb_path.empty()) return opts.duckdb_path;
  if (opts.duckdb_dir.empty()) return {};
  std::filesystem::path dir(opts.duckdb_dir);
  return (dir / (opts.schema + ".duckdb")).string();
}

}  // namespace

int main(int argc, char **argv) {
  Options opts;
  if (!ParseArgs(argc, argv, &opts)) return 1;
  if (opts.show_help) {
    PrintUsage(argv[0]);
    return 0;
  }

  const std::string duckdb_path = ResolveDuckdbPath(opts);
  if (duckdb_path.empty()) {
    std::cerr << "Unable to resolve DuckDB path\n";
    return 1;
  }
  std::filesystem::path duckdb_file(duckdb_path);
  std::error_code ec;
  std::filesystem::create_directories(duckdb_file.parent_path(), ec);
  if (ec) {
    std::cerr << "Failed to create DuckDB directory: " << ec.message() << "\n";
    return 1;
  }

  if (mysql_library_init(0, nullptr, nullptr) != 0) {
    std::cerr << "mysql_library_init failed\n";
    return 1;
  }

  MYSQL *mysql = mysql_init(nullptr);
  if (!mysql) {
    std::cerr << "mysql_init failed\n";
    mysql_library_end();
    return 1;
  }

  if (!mysql_real_connect(mysql, opts.host.c_str(), opts.user.c_str(),
                          opts.password.empty() ? nullptr
                                               : opts.password.c_str(),
                          nullptr, opts.port,
                          opts.socket.empty() ? nullptr : opts.socket.c_str(),
                          0)) {
    std::cerr << "mysql_real_connect failed: " << mysql_error(mysql) << "\n";
    mysql_close(mysql);
    mysql_library_end();
    return 1;
  }

  if (!ExecQuery(mysql,
                 "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ")) {
    mysql_close(mysql);
    mysql_library_end();
    return 1;
  }
  if (!ExecQuery(mysql, "START TRANSACTION WITH CONSISTENT SNAPSHOT")) {
    mysql_close(mysql);
    mysql_library_end();
    return 1;
  }

  std::string snapshot_gtid;
  if (!QuerySingleValue(mysql, "SELECT @@GLOBAL.GTID_EXECUTED",
                        &snapshot_gtid)) {
    mysql_close(mysql);
    mysql_library_end();
    return 1;
  }
  if (opts.verbose) {
    std::cerr << "Snapshot GTID set: " << snapshot_gtid << "\n";
  }

  std::vector<std::string> tables;
  if (!opts.table.empty()) {
    tables.push_back(opts.table);
  } else if (!ListTables(mysql, opts.schema, &tables)) {
    mysql_close(mysql);
    mysql_library_end();
    return 1;
  }

  if (tables.empty()) {
    std::cerr << "No tables found to load\n";
    mysql_close(mysql);
    mysql_library_end();
    return 1;
  }

  DuckDBAdapter adapter;
  DuckDBConfig cfg;
  cfg.read_only = false;
  Status st = adapter.Init(duckdb_path, cfg);
  if (!st.ok()) {
    std::cerr << "DuckDB init failed: " << st.message << "\n";
    mysql_close(mysql);
    mysql_library_end();
    return 1;
  }

  if (!EnsureDuckdbSchema(adapter, opts.schema)) {
    adapter.Shutdown();
    mysql_close(mysql);
    mysql_library_end();
    return 1;
  }

  if (!StoreSnapshotGtid(adapter, snapshot_gtid)) {
    adapter.Shutdown();
    mysql_close(mysql);
    mysql_library_end();
    return 1;
  }

  size_t total_rows = 0;
  bool validation_ok = true;
  for (const auto &table : tables) {
    if (opts.verbose) {
      std::cerr << "Loading " << opts.schema << "." << table << "...\n";
    }

    bool target_exists = false;
    if (!DuckdbTableExists(adapter, opts.schema, table, &target_exists)) {
      adapter.Shutdown();
      mysql_close(mysql);
      mysql_library_end();
      return 1;
    }
    if (target_exists && !opts.overwrite) {
      std::cerr << "DuckDB table " << opts.schema << "." << table
                << " already exists. Use --overwrite to replace.\n";
      adapter.Shutdown();
      mysql_close(mysql);
      mysql_library_end();
      return 1;
    }

    MySQLTableDef def;
    std::vector<bool> blob_flags;
    if (!FetchTableDef(mysql, opts.schema, table, &def, &blob_flags)) {
      std::cerr << "Failed to fetch columns for " << opts.schema << "." << table
                << "\n";
      adapter.Shutdown();
      mysql_close(mysql);
      mysql_library_end();
      return 1;
    }

    const std::string temp_table = MakeLoadingTableName(table);
    adapter.DropTable(opts.schema, temp_table);
    MySQLTableDef temp_def = def;
    temp_def.name = temp_table;
    st = adapter.CreateTable(temp_def);
    if (!st.ok()) {
      std::cerr << "DuckDB create table failed for " << opts.schema << "."
                << temp_table << ": " << st.message << "\n";
      adapter.Shutdown();
      mysql_close(mysql);
      mysql_library_end();
      return 1;
    }

    size_t rows = 0;
    if (!StreamTable(mysql, adapter, opts.schema, table, temp_table, blob_flags,
                     opts.batch_rows, &rows)) {
      adapter.Shutdown();
      mysql_close(mysql);
      mysql_library_end();
      return 1;
    }

    if (!SwapInTable(adapter, opts.schema, temp_table, table)) {
      adapter.Shutdown();
      mysql_close(mysql);
      mysql_library_end();
      return 1;
    }
    if (opts.validate) {
      bool count_ok = false;
      bool sums_ok = true;
      if (!ValidateRowCount(mysql, adapter, opts.schema, table, &count_ok)) {
        adapter.Shutdown();
        mysql_close(mysql);
        mysql_library_end();
        return 1;
      }
      if (!count_ok) validation_ok = false;
      if (opts.validate_sum) {
        sums_ok = false;
        if (!ValidateSums(mysql, adapter, opts.schema, def, &sums_ok)) {
          adapter.Shutdown();
          mysql_close(mysql);
          mysql_library_end();
          return 1;
        }
        if (!sums_ok) validation_ok = false;
      }
      const bool table_ok =
          count_ok && (!opts.validate_sum || sums_ok);
      if (opts.verbose && table_ok) {
        std::cerr << "Validation OK for " << opts.schema << "." << table << "\n";
      }
    }
    total_rows += rows;
    if (opts.verbose) {
      std::cerr << "Loaded " << rows << " rows from " << opts.schema << "."
                << table << "\n";
    }
  }

  ExecQuery(mysql, "COMMIT");
  if (opts.verbose) {
    std::cerr << "Total rows loaded: " << total_rows << "\n";
  }
  if (opts.validate) {
    if (validation_ok) {
      std::cerr << "Snapshot validation OK\n";
    } else {
      std::cerr << "Snapshot validation FAILED\n";
    }
  }

  adapter.Shutdown();
  mysql_close(mysql);
  mysql_library_end();
  if (opts.validate && !validation_ok) return 2;
  return 0;
}
