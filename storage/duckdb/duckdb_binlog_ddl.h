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

#ifndef PLUGIN_DUCKDB_BINLOG_DDL_H_
#define PLUGIN_DUCKDB_BINLOG_DDL_H_

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "storage/duckdb/duckdb_adapter.h"

namespace duckdb_se {

struct ParsedDdl {
  DDLChange change;
  std::string schema;
  bool if_not_exists{false};
};

namespace detail {

inline bool IsIdentChar(char ch) {
  unsigned char uch = static_cast<unsigned char>(ch);
  return std::isalnum(uch) != 0 || ch == '_' || ch == '$';
}

inline size_t SkipSpace(const std::string &sql, size_t pos) {
  while (pos < sql.size() &&
         std::isspace(static_cast<unsigned char>(sql[pos])) != 0) {
    ++pos;
  }
  return pos;
}

inline bool ParseBareToken(const std::string &sql, size_t *pos,
                           std::string *token) {
  if (!pos || !token) return false;
  size_t i = SkipSpace(sql, *pos);
  if (i >= sql.size()) return false;
  if (!std::isalpha(static_cast<unsigned char>(sql[i]))) return false;
  size_t start = i;
  while (i < sql.size() && IsIdentChar(sql[i])) ++i;
  *token = sql.substr(start, i - start);
  *pos = i;
  return true;
}

inline bool ConsumeKeyword(const std::string &sql, size_t *pos,
                           const char *keyword) {
  std::string token;
  size_t tmp = *pos;
  if (!ParseBareToken(sql, &tmp, &token)) return false;
  if (token.size() != std::strlen(keyword)) return false;
  for (size_t i = 0; i < token.size(); ++i) {
    if (std::toupper(static_cast<unsigned char>(token[i])) !=
        std::toupper(static_cast<unsigned char>(keyword[i]))) {
      return false;
    }
  }
  *pos = tmp;
  return true;
}

inline bool ParseIdentifier(const std::string &sql, size_t *pos,
                            std::string *value, size_t *start,
                            size_t *end) {
  if (!pos || !value || !start || !end) return false;
  size_t i = SkipSpace(sql, *pos);
  if (i >= sql.size()) return false;
  const char quote = sql[i];
  if (quote == '`' || quote == '"') {
    size_t j = i + 1;
    std::string out;
    while (j < sql.size()) {
      if (sql[j] == quote) {
        if (j + 1 < sql.size() && sql[j + 1] == quote) {
          out.push_back(quote);
          j += 2;
          continue;
        }
        break;
      }
      out.push_back(sql[j]);
      ++j;
    }
    if (j >= sql.size() || sql[j] != quote) return false;
    *value = out;
    *start = i;
    *end = j + 1;
    *pos = j + 1;
    return true;
  }

  if (!IsIdentChar(sql[i])) return false;
  size_t start_pos = i;
  while (i < sql.size() && IsIdentChar(sql[i])) ++i;
  *value = sql.substr(start_pos, i - start_pos);
  *start = start_pos;
  *end = i;
  *pos = i;
  return true;
}

struct TableRef {
  std::string schema;
  std::string table;
  size_t start{0};
  size_t end{0};
  size_t table_start{0};
  size_t table_end{0};
  bool has_schema{false};
};

inline bool ParseTableRef(const std::string &sql, size_t *pos, TableRef *ref) {
  if (!pos || !ref) return false;
  std::string first;
  size_t first_start = 0;
  size_t first_end = 0;
  if (!ParseIdentifier(sql, pos, &first, &first_start, &first_end)) return false;
  size_t i = SkipSpace(sql, *pos);
  if (i < sql.size() && sql[i] == '.') {
    size_t after_dot = i + 1;
    std::string second;
    size_t second_start = 0;
    size_t second_end = 0;
    size_t tmp = after_dot;
    if (!ParseIdentifier(sql, &tmp, &second, &second_start, &second_end)) {
      return false;
    }
    ref->schema = first;
    ref->table = second;
    ref->start = first_start;
    ref->end = second_end;
    ref->table_start = second_start;
    ref->table_end = second_end;
    ref->has_schema = true;
    *pos = tmp;
    return true;
  }

  ref->schema.clear();
  ref->table = first;
  ref->start = first_start;
  ref->end = first_end;
  ref->table_start = first_start;
  ref->table_end = first_end;
  ref->has_schema = false;
  *pos = i;
  return true;
}

inline std::string RewriteSql(const std::string &sql,
                              const std::vector<TableRef> &refs) {
  if (refs.empty()) return sql;
  std::string out = sql;
  std::vector<TableRef> ordered = refs;
  std::sort(ordered.begin(), ordered.end(),
            [](const TableRef &a, const TableRef &b) {
              return a.start > b.start;
            });
  for (const auto &ref : ordered) {
    if (!ref.has_schema) continue;
    const std::string table_token =
        sql.substr(ref.table_start, ref.table_end - ref.table_start);
    out.replace(ref.start, ref.end - ref.start, table_token);
  }
  return out;
}

}  // namespace detail

inline bool ParseDdlQuery(const std::string &sql,
                          const std::string &default_schema, ParsedDdl *out,
                          std::string *error) {
  if (!out) return false;
  out->change = DDLChange{};
  out->schema.clear();
  out->if_not_exists = false;

  size_t pos = 0;
  if (detail::ConsumeKeyword(sql, &pos, "CREATE")) {
    (void)detail::ConsumeKeyword(sql, &pos, "TEMPORARY");
    if (!detail::ConsumeKeyword(sql, &pos, "TABLE")) return false;
    if (detail::ConsumeKeyword(sql, &pos, "IF")) {
      if (detail::ConsumeKeyword(sql, &pos, "NOT") &&
          detail::ConsumeKeyword(sql, &pos, "EXISTS")) {
        out->if_not_exists = true;
      }
    }
    detail::TableRef ref;
    if (!detail::ParseTableRef(sql, &pos, &ref)) {
      if (error) *error = "Failed to parse CREATE TABLE name";
      return false;
    }
    out->schema = ref.has_schema ? ref.schema : default_schema;
    if (out->schema.empty()) {
      if (error) *error = "Missing schema for CREATE TABLE";
      return false;
    }
    out->change.type = DDLChange::Type::kCreate;
    out->change.table = TableId{out->schema, ref.table};
    out->change.sql = detail::RewriteSql(sql, {ref});
    return true;
  }

  if (detail::ConsumeKeyword(sql, &pos, "DROP")) {
    (void)detail::ConsumeKeyword(sql, &pos, "TEMPORARY");
    if (!detail::ConsumeKeyword(sql, &pos, "TABLE")) return false;
    if (detail::ConsumeKeyword(sql, &pos, "IF")) {
      (void)detail::ConsumeKeyword(sql, &pos, "EXISTS");
    }
    detail::TableRef ref;
    if (!detail::ParseTableRef(sql, &pos, &ref)) {
      if (error) *error = "Failed to parse DROP TABLE name";
      return false;
    }
    size_t after = detail::SkipSpace(sql, pos);
    if (after < sql.size() && sql[after] == ',') {
      if (error) *error = "Multi-table DROP TABLE not supported";
      return false;
    }
    out->schema = ref.has_schema ? ref.schema : default_schema;
    if (out->schema.empty()) {
      if (error) *error = "Missing schema for DROP TABLE";
      return false;
    }
    out->change.type = DDLChange::Type::kDrop;
    out->change.table = TableId{out->schema, ref.table};
    out->change.sql = detail::RewriteSql(sql, {ref});
    return true;
  }

  if (detail::ConsumeKeyword(sql, &pos, "ALTER")) {
    if (!detail::ConsumeKeyword(sql, &pos, "TABLE")) return false;
    detail::TableRef ref;
    if (!detail::ParseTableRef(sql, &pos, &ref)) {
      if (error) *error = "Failed to parse ALTER TABLE name";
      return false;
    }
    out->schema = ref.has_schema ? ref.schema : default_schema;
    if (out->schema.empty()) {
      if (error) *error = "Missing schema for ALTER TABLE";
      return false;
    }
    out->change.type = DDLChange::Type::kAlter;
    out->change.table = TableId{out->schema, ref.table};
    out->change.sql = detail::RewriteSql(sql, {ref});
    return true;
  }

  if (detail::ConsumeKeyword(sql, &pos, "RENAME")) {
    if (!detail::ConsumeKeyword(sql, &pos, "TABLE")) return false;
    detail::TableRef from;
    if (!detail::ParseTableRef(sql, &pos, &from)) {
      if (error) *error = "Failed to parse RENAME TABLE source";
      return false;
    }
    if (!detail::ConsumeKeyword(sql, &pos, "TO")) {
      if (error) *error = "Missing TO in RENAME TABLE";
      return false;
    }
    detail::TableRef to;
    if (!detail::ParseTableRef(sql, &pos, &to)) {
      if (error) *error = "Failed to parse RENAME TABLE target";
      return false;
    }
    size_t after = detail::SkipSpace(sql, pos);
    if (after < sql.size() && sql[after] == ',') {
      if (error) *error = "Multi-table RENAME not supported";
      return false;
    }
    std::string schema = from.has_schema ? from.schema : default_schema;
    std::string new_schema = to.has_schema ? to.schema : default_schema;
    if (schema.empty() || new_schema.empty()) {
      if (error) *error = "Missing schema for RENAME TABLE";
      return false;
    }
    if (schema != new_schema) {
      if (error) *error = "Cross-schema rename not supported";
      return false;
    }
    out->schema = schema;
    out->change.type = DDLChange::Type::kRename;
    out->change.table = TableId{schema, from.table};
    out->change.new_table = TableId{schema, to.table};
    out->change.sql = detail::RewriteSql(sql, {from, to});
    return true;
  }

  if (detail::ConsumeKeyword(sql, &pos, "TRUNCATE")) {
    (void)detail::ConsumeKeyword(sql, &pos, "TABLE");
    detail::TableRef ref;
    if (!detail::ParseTableRef(sql, &pos, &ref)) {
      if (error) *error = "Failed to parse TRUNCATE TABLE name";
      return false;
    }
    out->schema = ref.has_schema ? ref.schema : default_schema;
    if (out->schema.empty()) {
      if (error) *error = "Missing schema for TRUNCATE TABLE";
      return false;
    }
    out->change.type = DDLChange::Type::kTruncate;
    out->change.table = TableId{out->schema, ref.table};
    out->change.sql = detail::RewriteSql(sql, {ref});
    return true;
  }

  return false;
}

}  // namespace duckdb_se

#endif  // PLUGIN_DUCKDB_BINLOG_DDL_H_
