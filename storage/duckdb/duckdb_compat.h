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

#ifndef PLUGIN_DUCKDB_COMPAT_H_
#define PLUGIN_DUCKDB_COMPAT_H_

#include <string>
#include <vector>

namespace duckdb_se {

struct QualifiedTableRef {
  std::string schema;
  std::string table;
};

struct DuckdbRewriteResult {
  bool ok{true};
  std::string sql;
  std::string reason;
};

DuckdbRewriteResult RewriteForDuckdb(const std::string &sql);
std::string RewriteQualifiedTables(
    const std::string &sql, const std::vector<QualifiedTableRef> &tables);

}  // namespace duckdb_se

#endif  // PLUGIN_DUCKDB_COMPAT_H_
