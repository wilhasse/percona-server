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

          if (upper == "IFNULL") {
            out.append("COALESCE");
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
            out.append("TO_TIMESTAMP");
            continue;
          }
          if (upper == "UNIX_TIMESTAMP") {
            out.append("EPOCH");
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
