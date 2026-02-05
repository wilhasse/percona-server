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

#include "storage/duckdb/duckdb_compat.h"

#include <cctype>
#include <string_view>
#include <vector>

namespace duckdb_se {
namespace {

bool IsIdentChar(char ch) {
  return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

bool MatchCI(const std::string &sql, size_t pos, const std::string &token) {
  if (pos + token.size() > sql.size()) return false;
  for (size_t i = 0; i < token.size(); ++i) {
    const char lhs = static_cast<char>(
        std::toupper(static_cast<unsigned char>(sql[pos + i])));
    const char rhs = static_cast<char>(
        std::toupper(static_cast<unsigned char>(token[i])));
    if (lhs != rhs) return false;
  }
  return true;
}

bool IsSpace(char ch) {
  return std::isspace(static_cast<unsigned char>(ch));
}

std::string ToUpper(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (char ch : input) {
    out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
  }
  return out;
}

size_t SkipSpaces(const std::string &sql, size_t pos) {
  while (pos < sql.size() && IsSpace(sql[pos])) ++pos;
  return pos;
}

bool ParseArguments(const std::string &sql, size_t open_paren,
                    size_t *close_paren, std::vector<std::string> *args,
                    std::string *reason) {
  if (open_paren >= sql.size() || sql[open_paren] != '(') {
    if (reason) *reason = "Expected '(' for function call";
    return false;
  }

  size_t pos = open_paren + 1;
  int depth = 0;
  bool in_single = false;
  bool in_double = false;
  bool in_backtick = false;
  size_t arg_start = pos;

  while (pos < sql.size()) {
    const char ch = sql[pos];
    if (in_single) {
      if (ch == '\'') {
        ++pos;
        if (pos < sql.size() && sql[pos] == '\'') {
          ++pos;
          continue;
        }
        in_single = false;
        continue;
      }
      ++pos;
      continue;
    }
    if (in_double) {
      if (ch == '"') {
        in_double = false;
      }
      ++pos;
      continue;
    }
    if (in_backtick) {
      if (ch == '`') {
        in_backtick = false;
      }
      ++pos;
      continue;
    }

    if (ch == '\'') {
      in_single = true;
      ++pos;
      continue;
    }
    if (ch == '"') {
      in_double = true;
      ++pos;
      continue;
    }
    if (ch == '`') {
      in_backtick = true;
      ++pos;
      continue;
    }
    if (ch == '(') {
      ++depth;
      ++pos;
      continue;
    }
    if (ch == ')') {
      if (depth == 0) {
        if (args) {
          args->push_back(sql.substr(arg_start, pos - arg_start));
        }
        *close_paren = pos;
        return true;
      }
      --depth;
      ++pos;
      continue;
    }
    if (ch == ',' && depth == 0) {
      if (args) {
        args->push_back(sql.substr(arg_start, pos - arg_start));
      }
      arg_start = pos + 1;
      ++pos;
      continue;
    }
    ++pos;
  }

  if (reason) *reason = "Unbalanced parentheses in function call";
  return false;
}

std::string Trim(std::string_view input) {
  size_t start = 0;
  while (start < input.size() && IsSpace(input[start])) ++start;
  size_t end = input.size();
  while (end > start && IsSpace(input[end - 1])) --end;
  return std::string(input.substr(start, end - start));
}

std::string QuoteIdent(std::string_view input) {
  std::string out;
  out.reserve(input.size() + 2);
  out.push_back('"');
  for (char ch : input) {
    if (ch == '"') out.push_back('"');
    out.push_back(ch);
  }
  out.push_back('"');
  return out;
}

std::string NormalizeBackticks(const std::string &input) {
  std::string out;
  out.reserve(input.size());
  enum class State { kNormal, kSingle, kDouble, kBacktick };
  State state = State::kNormal;

  for (size_t i = 0; i < input.size(); ++i) {
    const char ch = input[i];
    if (state == State::kNormal) {
      if (ch == '\'') {
        state = State::kSingle;
        out.push_back(ch);
        continue;
      }
      if (ch == '\"') {
        state = State::kDouble;
        out.push_back(ch);
        continue;
      }
      if (ch == '`') {
        state = State::kBacktick;
        out.push_back('\"');
        continue;
      }
      out.push_back(ch);
      continue;
    }
    if (state == State::kSingle) {
      out.push_back(ch);
      if (ch == '\'') {
        if (i + 1 < input.size() && input[i + 1] == '\'') {
          out.push_back(input[i + 1]);
          ++i;
        } else {
          state = State::kNormal;
        }
      }
      continue;
    }
    if (state == State::kDouble) {
      out.push_back(ch);
      if (ch == '\"') state = State::kNormal;
      continue;
    }
    if (state == State::kBacktick) {
      if (ch == '`') {
        out.push_back('\"');
        state = State::kNormal;
      } else {
        out.push_back(ch);
      }
      continue;
    }
  }
  return out;
}

bool RewriteLimitClause(const std::string &sql, std::string *out) {
  out->clear();
  enum class State { kNormal, kSingle, kDouble, kBacktick };
  State state = State::kNormal;
  int paren_depth = 0;

  size_t i = 0;
  while (i < sql.size()) {
    char ch = sql[i];
    if (state == State::kNormal) {
      if (ch == '\'') {
        state = State::kSingle;
        out->push_back(ch);
        ++i;
        continue;
      }
      if (ch == '"') {
        state = State::kDouble;
        out->push_back(ch);
        ++i;
        continue;
      }
      if (ch == '`') {
        state = State::kBacktick;
        out->push_back(ch);
        ++i;
        continue;
      }
      if (ch == '(') {
        ++paren_depth;
        out->push_back(ch);
        ++i;
        continue;
      }
      if (ch == ')') {
        if (paren_depth > 0) --paren_depth;
        out->push_back(ch);
        ++i;
        continue;
      }
      if (paren_depth == 0 && IsIdentChar(ch)) {
        size_t start = i;
        while (i < sql.size() && IsIdentChar(sql[i])) ++i;
        std::string token = ToUpper(std::string_view(sql.data() + start, i - start));
        if (token == "LIMIT") {
          size_t pos = SkipSpaces(sql, i);
          size_t offset_start = pos;
          while (pos < sql.size() && std::isdigit(static_cast<unsigned char>(sql[pos]))) ++pos;
          if (offset_start == pos) {
            out->append(sql.substr(start, i - start));
            continue;
          }
          std::string offset = sql.substr(offset_start, pos - offset_start);
          pos = SkipSpaces(sql, pos);
          if (pos < sql.size() && sql[pos] == ',') {
            ++pos;
            pos = SkipSpaces(sql, pos);
            size_t count_start = pos;
            while (pos < sql.size() && std::isdigit(static_cast<unsigned char>(sql[pos]))) ++pos;
            if (count_start == pos) {
              out->append(sql.substr(start, i - start));
              continue;
            }
            std::string count = sql.substr(count_start, pos - count_start);
            out->append("LIMIT ");
            out->append(count);
            out->append(" OFFSET ");
            out->append(offset);
            i = pos;
            continue;
          }
          out->append(sql.substr(start, i - start));
          continue;
        }
        out->append(sql.substr(start, i - start));
        continue;
      }
      out->push_back(ch);
      ++i;
      continue;
    }

    if (state == State::kSingle) {
      out->push_back(ch);
      ++i;
      if (ch == '\'') {
        if (i < sql.size() && sql[i] == '\'') {
          out->push_back(sql[i]);
          ++i;
        } else {
          state = State::kNormal;
        }
      }
      continue;
    }
    if (state == State::kDouble) {
      out->push_back(ch);
      ++i;
      if (ch == '"') state = State::kNormal;
      continue;
    }
    if (state == State::kBacktick) {
      out->push_back(ch);
      ++i;
      if (ch == '`') state = State::kNormal;
      continue;
    }
  }

  return true;
}

}  // namespace

std::string RewriteQualifiedTables(
    const std::string &sql, const std::vector<QualifiedTableRef> &tables) {
  if (tables.empty()) return sql;

  struct Pattern {
    std::string db;
    std::string table;
    std::string quoted_schema;
    std::string quoted_table;
    std::string quoted_pattern;
  };

  std::vector<Pattern> patterns;
  patterns.reserve(tables.size());
  for (const auto &entry : tables) {
    if (entry.schema.empty() || entry.table.empty()) continue;
    Pattern pattern;
    pattern.db = entry.schema;
    pattern.table = entry.table;
    pattern.quoted_schema = QuoteIdent(entry.schema);
    pattern.quoted_table = QuoteIdent(entry.table);
    pattern.quoted_pattern =
        pattern.quoted_schema + "." + pattern.quoted_table;
    patterns.emplace_back(std::move(pattern));
  }
  if (patterns.empty()) return sql;

  std::string out;
  out.reserve(sql.size());
  const size_t len = sql.size();
  enum class State { kNormal, kSingle, kDouble, kBacktick };
  State state = State::kNormal;
  for (size_t i = 0; i < len;) {
    const char ch = sql[i];
    if (state != State::kNormal) {
      out.push_back(ch);
      ++i;
      if (state == State::kSingle) {
        if (ch == '\'') {
          if (i < len && sql[i] == '\'') {
            out.push_back(sql[i]);
            ++i;
          } else {
            state = State::kNormal;
          }
        }
      } else if (state == State::kDouble) {
        if (ch == '"') state = State::kNormal;
      } else if (state == State::kBacktick) {
        if (ch == '`') state = State::kNormal;
      }
      continue;
    }

    bool matched = false;
    for (const auto &pattern : patterns) {
      if (MatchCI(sql, i, pattern.quoted_pattern)) {
        out.append(pattern.quoted_schema);
        out.push_back('.');
        out.append(pattern.quoted_table);
        i += pattern.quoted_pattern.size();
        matched = true;
        break;
      }

      if (MatchCI(sql, i, pattern.quoted_schema)) {
        const size_t db_end = i + pattern.quoted_schema.size();
        if (db_end < len && sql[db_end] == '.') {
          const size_t table_start = db_end + 1;
          if (MatchCI(sql, table_start, pattern.quoted_table)) {
            const size_t table_end =
                table_start + pattern.quoted_table.size();
            out.append(pattern.quoted_schema);
            out.push_back('.');
            out.append(pattern.quoted_table);
            i = table_end;
            matched = true;
            break;
          }
          if (MatchCI(sql, table_start, pattern.table)) {
            const size_t table_end = table_start + pattern.table.size();
            out.append(pattern.quoted_schema);
            out.push_back('.');
            out.append(pattern.quoted_table);
            i = table_end;
            matched = true;
            break;
          }
        }
      }

      if (MatchCI(sql, i, pattern.db)) {
        const size_t db_end = i + pattern.db.size();
        if (db_end < len && sql[db_end] == '.') {
          const size_t table_start = db_end + 1;
          if (MatchCI(sql, table_start, pattern.quoted_table)) {
            const size_t table_end =
                table_start + pattern.quoted_table.size();
            const bool left_ok = (i == 0) || !IsIdentChar(sql[i - 1]);
            const bool right_ok =
                (table_end >= len) || !IsIdentChar(sql[table_end]);
            if (left_ok && right_ok) {
              out.append(pattern.quoted_schema);
              out.push_back('.');
              out.append(pattern.quoted_table);
              i = table_end;
              matched = true;
              break;
            }
          }
          if (MatchCI(sql, table_start, pattern.table)) {
            const size_t table_end = table_start + pattern.table.size();
            const bool left_ok = (i == 0) || !IsIdentChar(sql[i - 1]);
            const bool right_ok =
                (table_end >= len) || !IsIdentChar(sql[table_end]);
            if (left_ok && right_ok) {
              out.append(pattern.quoted_schema);
              out.push_back('.');
              out.append(pattern.quoted_table);
              i = table_end;
              matched = true;
              break;
            }
          }
        }
      }

      if (MatchCI(sql, i, pattern.table)) {
        const size_t table_end = i + pattern.table.size();
        const bool left_ok = (i == 0) || !IsIdentChar(sql[i - 1]);
        const bool right_ok =
            (table_end >= len) || !IsIdentChar(sql[table_end]);
        const bool not_qualified = (i == 0) || sql[i - 1] != '.';
        if (left_ok && right_ok && not_qualified) {
          out.append(pattern.quoted_schema);
          out.push_back('.');
          out.append(pattern.quoted_table);
          i = table_end;
          matched = true;
          break;
        }
      }
    }
    if (matched) continue;

    if (ch == '\'') {
      state = State::kSingle;
      out.push_back(ch);
      ++i;
      continue;
    }
    if (ch == '"') {
      state = State::kDouble;
      out.push_back(ch);
      ++i;
      continue;
    }
    if (ch == '`') {
      state = State::kBacktick;
      out.push_back(ch);
      ++i;
      continue;
    }

    out.push_back(ch);
    ++i;
  }
  return out;
}

DuckdbRewriteResult RewriteForDuckdb(const std::string &sql) {
  DuckdbRewriteResult result;

  enum class State { kNormal, kSingle, kDouble, kBacktick };
  State state = State::kNormal;
  std::string out;
  out.reserve(sql.size());

  for (size_t i = 0; i < sql.size();) {
    char ch = sql[i];
    if (state == State::kNormal) {
      if (ch == '\'') {
        state = State::kSingle;
        out.push_back(ch);
        ++i;
        continue;
      }
      if (ch == '"') {
        state = State::kDouble;
        out.push_back(ch);
        ++i;
        continue;
      }
      if (ch == '`') {
        state = State::kBacktick;
        out.push_back('"');
        ++i;
        continue;
      }
      if (IsIdentChar(ch)) {
        size_t start = i;
        while (i < sql.size() && IsIdentChar(sql[i])) ++i;
        std::string token = std::string(sql.data() + start, i - start);
        std::string upper = ToUpper(token);

        size_t lookahead = SkipSpaces(sql, i);
        bool is_func = (lookahead < sql.size() && sql[lookahead] == '(');

        if (is_func) {
          if (upper == "CURDATE" || upper == "CURTIME" || upper == "NOW") {
            size_t close_paren = 0;
            std::vector<std::string> args;
            std::string reason;
            if (!ParseArguments(sql, lookahead, &close_paren, &args, &reason)) {
              result.ok = false;
              result.reason = reason;
              return result;
            }
            bool has_arg = false;
            for (const auto &arg : args) {
              if (!Trim(arg).empty()) {
                has_arg = true;
                break;
              }
            }
            if (has_arg) {
              result.ok = false;
              result.reason = token + " does not accept arguments";
              return result;
            }
            if (upper == "CURDATE") {
              out.append("CURRENT_DATE");
            } else if (upper == "CURTIME") {
              out.append("CURRENT_TIME");
            } else {
              out.append("CURRENT_TIMESTAMP");
            }
            i = close_paren + 1;
            continue;
          }

          if (upper == "LOCATE") {
            size_t close_paren = 0;
            std::vector<std::string> args;
            std::string reason;
            if (!ParseArguments(sql, lookahead, &close_paren, &args, &reason)) {
              result.ok = false;
              result.reason = reason;
              return result;
            }
            if (args.size() == 2) {
              out.append("STRPOS(");
              out.append(NormalizeBackticks(Trim(args[1])));
              out.append(", ");
              out.append(NormalizeBackticks(Trim(args[0])));
              out.append(")");
              i = close_paren + 1;
              continue;
            }
            result.ok = false;
            result.reason = "LOCATE with start position is not supported";
            return result;
          }

          if (upper == "IF") {
            size_t close_paren = 0;
            std::vector<std::string> args;
            std::string reason;
            if (!ParseArguments(sql, lookahead, &close_paren, &args, &reason)) {
              result.ok = false;
              result.reason = reason;
              return result;
            }
            if (args.size() != 3) {
              result.ok = false;
              result.reason = "IF() requires exactly 3 arguments";
              return result;
            }
            out.append("CASE WHEN ");
            out.append(NormalizeBackticks(Trim(args[0])));
            out.append(" THEN ");
            out.append(NormalizeBackticks(Trim(args[1])));
            out.append(" ELSE ");
            out.append(NormalizeBackticks(Trim(args[2])));
            out.append(" END");
            i = close_paren + 1;
            continue;
          }

          if (upper == "IFNULL") {
            out.append("COALESCE");
            continue;
          }
          if (upper == "LCASE") {
            out.append("LOWER");
            continue;
          }
          if (upper == "UCASE") {
            out.append("UPPER");
            continue;
          }
          if (upper == "MID") {
            out.append("SUBSTR");
            continue;
          }
          if (upper == "DATE_FORMAT") {
            out.append("STRFTIME");
            continue;
          }
          if (upper == "STR_TO_DATE") {
            out.append("STRPTIME");
            continue;
          }
          if (upper == "FROM_UNIXTIME") {
            size_t close_paren = 0;
            std::vector<std::string> args;
            std::string reason;
            if (!ParseArguments(sql, lookahead, &close_paren, &args,
                                &reason)) {
              result.ok = false;
              result.reason = reason;
              return result;
            }
            if (args.size() != 1) {
              result.ok = false;
              result.reason = "FROM_UNIXTIME requires exactly 1 argument";
              return result;
            }
            out.append("CAST(TO_TIMESTAMP(");
            out.append(Trim(args[0]));
            out.append(") AS TIMESTAMP)");
            i = close_paren + 1;
            continue;
          }
          if (upper == "UNIX_TIMESTAMP") {
            size_t close_paren = 0;
            std::vector<std::string> args;
            std::string reason;
            if (!ParseArguments(sql, lookahead, &close_paren, &args,
                                &reason)) {
              result.ok = false;
              result.reason = reason;
              return result;
            }
            if (args.size() != 1) {
              result.ok = false;
              result.reason = "UNIX_TIMESTAMP requires exactly 1 argument";
              return result;
            }
            out.append("EPOCH(CAST(");
            out.append(Trim(args[0]));
            out.append(" AS TIMESTAMP))");
            i = close_paren + 1;
            continue;
          }
        }

        out.append(token);
        continue;
      }

      out.push_back(ch);
      ++i;
      continue;
    }

    if (state == State::kSingle) {
      out.push_back(ch);
      ++i;
      if (ch == '\'') {
        if (i < sql.size() && sql[i] == '\'') {
          out.push_back(sql[i]);
          ++i;
        } else {
          state = State::kNormal;
        }
      }
      continue;
    }
    if (state == State::kDouble) {
      out.push_back(ch);
      ++i;
      if (ch == '"') state = State::kNormal;
      continue;
    }
    if (state == State::kBacktick) {
      if (ch == '`') {
        out.push_back('"');
        state = State::kNormal;
      } else {
        out.push_back(ch);
      }
      ++i;
      continue;
    }
  }

  std::string with_limit;
  RewriteLimitClause(out, &with_limit);
  result.sql = std::move(with_limit);
  return result;
}

}  // namespace duckdb_se
