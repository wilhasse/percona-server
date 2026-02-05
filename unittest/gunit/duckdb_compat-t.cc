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

#include <utility>
#include <vector>

#include "storage/duckdb/duckdb_compat.h"

using duckdb_se::QualifiedTableRef;
using duckdb_se::RewriteForDuckdb;
using duckdb_se::RewriteQualifiedTables;

namespace {

std::vector<QualifiedTableRef> MakeTables(
    std::initializer_list<std::pair<const char *, const char *>> items) {
  std::vector<QualifiedTableRef> tables;
  tables.reserve(items.size());
  for (const auto &entry : items) {
    QualifiedTableRef ref;
    ref.schema = entry.first;
    ref.table = entry.second;
    tables.push_back(std::move(ref));
  }
  return tables;
}

}  // namespace

TEST(DuckDBCompatTest, RewriteQualifiedJoinWithBackticks) {
  const std::string sql =
      "SELECT `db`.`t1`.`id`, `db`.`t2`.`val` "
      "FROM `db`.`t1` t1 "
      "JOIN `db`.`t2` t2 ON `db`.`t1`.`id` = `db`.`t2`.`id`";

  auto rewrite = RewriteForDuckdb(sql);
  ASSERT_TRUE(rewrite.ok);

  const auto tables = MakeTables({{"db", "t1"}, {"db", "t2"}});
  const std::string rewritten = RewriteQualifiedTables(rewrite.sql, tables);

  EXPECT_EQ(
      "SELECT \"db\".\"t1\".\"id\", \"db\".\"t2\".\"val\" FROM \"db\".\"t1\" "
      "\"t1\" JOIN \"db\".\"t2\" \"t2\" ON \"db\".\"t1\".\"id\" = "
      "\"db\".\"t2\".\"id\"",
      rewritten);
}

TEST(DuckDBCompatTest, RewriteSkipsStringLiterals) {
  const std::string sql = "SELECT 'db.t1' AS s FROM db.t1";

  auto rewrite = RewriteForDuckdb(sql);
  ASSERT_TRUE(rewrite.ok);

  const auto tables = MakeTables({{"db", "t1"}});
  const std::string rewritten = RewriteQualifiedTables(rewrite.sql, tables);

  EXPECT_EQ("SELECT 'db.t1' AS s FROM \"db\".\"t1\"", rewritten);
}

TEST(DuckDBCompatTest, RewriteOnlyMappedTables) {
  const std::string sql =
      "SELECT * FROM other.t2 JOIN db.t1 ON other.t2.id = db.t1.id";

  auto rewrite = RewriteForDuckdb(sql);
  ASSERT_TRUE(rewrite.ok);

  const auto tables = MakeTables({{"db", "t1"}});
  const std::string rewritten = RewriteQualifiedTables(rewrite.sql, tables);

  EXPECT_EQ(
      "SELECT * FROM other.t2 JOIN \"db\".\"t1\" ON other.t2.id = "
      "\"db\".\"t1\".id",
      rewritten);
}

TEST(DuckDBCompatTest, RewriteIfFunction) {
  const std::string sql = "SELECT IF(`a` > 0, 'y', 'n') FROM `t`";

  auto rewrite = RewriteForDuckdb(sql);
  ASSERT_TRUE(rewrite.ok);

  EXPECT_EQ("SELECT CASE WHEN \"a\" > 0 THEN 'y' ELSE 'n' END FROM \"t\"",
            rewrite.sql);
}

TEST(DuckDBCompatTest, RewriteFunctionAliases) {
  const std::string sql =
      "SELECT LCASE(name), UCASE(name), MID(name, 2, 3) FROM t";

  auto rewrite = RewriteForDuckdb(sql);
  ASSERT_TRUE(rewrite.ok);

  EXPECT_EQ("SELECT LOWER(name), UPPER(name), SUBSTR(name, 2, 3) FROM t",
            rewrite.sql);
}
