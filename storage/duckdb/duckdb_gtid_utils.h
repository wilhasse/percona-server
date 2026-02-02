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

#ifndef PLUGIN_DUCKDB_GTID_UTILS_H_
#define PLUGIN_DUCKDB_GTID_UTILS_H_

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "libbinlogevents/include/gtids/gtid.h"
#include "libbinlogevents/include/gtids/gtidset.h"

namespace duckdb_se {
namespace detail {

inline std::string TrimGtidString(std::string value) {
  auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(),
                           [&](unsigned char ch) { return !is_space(ch); }));
  value.erase(std::find_if(value.rbegin(), value.rend(),
                           [&](unsigned char ch) { return !is_space(ch); })
                  .base(),
              value.end());
  return value;
}

inline bool ParseIntervalToken(const std::string &token, long long *start_out,
                               long long *end_out) {
  if (!start_out || !end_out) return false;
  const auto dash = token.find('-');
  const std::string start_str =
      dash == std::string::npos ? token : token.substr(0, dash);
  const std::string end_str =
      dash == std::string::npos ? token : token.substr(dash + 1);
  if (start_str.empty() || end_str.empty()) return false;
  char *endptr = nullptr;
  errno = 0;
  long long start = std::strtoll(start_str.c_str(), &endptr, 10);
  if (errno != 0 || endptr == start_str.c_str() || *endptr != '\0') {
    return false;
  }
  errno = 0;
  endptr = nullptr;
  long long end = std::strtoll(end_str.c_str(), &endptr, 10);
  if (errno != 0 || endptr == end_str.c_str() || *endptr != '\0') {
    return false;
  }
  if (start <= 0 || end < start) return false;
  *start_out = start;
  *end_out = end;
  return true;
}

}  // namespace detail

inline bool ParseGtidSetString(const std::string &input,
                               binary_log::gtids::Gtid_set *out,
                               std::string *error) {
  if (!out) return false;
  if (input.empty()) return true;
  std::string gtid_set = detail::TrimGtidString(input);
  if (gtid_set.empty()) return true;

  size_t pos = 0;
  while (pos < gtid_set.size()) {
    const size_t comma = gtid_set.find(',', pos);
    const std::string entry = detail::TrimGtidString(
        gtid_set.substr(pos, comma == std::string::npos ? std::string::npos
                                                        : comma - pos));
    if (!entry.empty()) {
      const size_t first_colon = entry.find(':');
      if (first_colon == std::string::npos) {
        if (error) *error = "Missing ':' in GTID set entry";
        return false;
      }
      const std::string uuid_str = entry.substr(0, first_colon);
      binary_log::gtids::Uuid uuid;
      if (uuid.parse(uuid_str.c_str(), uuid_str.size()) != 0) {
        if (error) *error = "Invalid UUID in GTID set";
        return false;
      }
      size_t interval_pos = first_colon + 1;
      while (interval_pos < entry.size()) {
        const size_t next_colon = entry.find(':', interval_pos);
        const std::string token = entry.substr(
            interval_pos, next_colon == std::string::npos
                              ? std::string::npos
                              : next_colon - interval_pos);
        long long start = 0;
        long long end = 0;
        if (!detail::ParseIntervalToken(token, &start, &end)) {
          if (error) *error = "Invalid interval in GTID set";
          return false;
        }
        if (out->add(uuid, binary_log::gtids::Gno_interval(start, end))) {
          if (error) *error = "Failed to add interval to GTID set";
          return false;
        }
        if (next_colon == std::string::npos) break;
        interval_pos = next_colon + 1;
      }
    }
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return true;
}

inline bool ParseSingleGtid(const std::string &input,
                            binary_log::gtids::Gtid *out,
                            std::string *error) {
  if (!out) return false;
  std::string gtid = detail::TrimGtidString(input);
  if (gtid.empty()) {
    if (error) *error = "Empty GTID";
    return false;
  }
  const size_t colon = gtid.find(':');
  if (colon == std::string::npos) {
    if (error) *error = "Missing ':' in GTID";
    return false;
  }
  const std::string uuid_str = gtid.substr(0, colon);
  const std::string gno_str = gtid.substr(colon + 1);
  if (gno_str.empty()) {
    if (error) *error = "Missing GTID sequence";
    return false;
  }
  binary_log::gtids::Uuid uuid;
  if (uuid.parse(uuid_str.c_str(), uuid_str.size()) != 0) {
    if (error) *error = "Invalid UUID in GTID";
    return false;
  }
  char *endptr = nullptr;
  errno = 0;
  long long gno = std::strtoll(gno_str.c_str(), &endptr, 10);
  if (errno != 0 || endptr == gno_str.c_str() || *endptr != '\0') {
    if (error) *error = "Invalid GTID sequence";
    return false;
  }
  if (gno <= 0) {
    if (error) *error = "GTID sequence must be positive";
    return false;
  }
  *out = binary_log::gtids::Gtid(
      uuid, static_cast<binary_log::gtids::gno_t>(gno));
  return true;
}

inline bool BuildGtidSetFromList(const std::vector<std::string> &gtids,
                                 std::string *out_set,
                                 std::string *error) {
  if (!out_set) return false;
  binary_log::gtids::Gtid_set set;
  for (const auto &gtid_str : gtids) {
    binary_log::gtids::Gtid gtid(binary_log::gtids::Uuid(),
                                 static_cast<binary_log::gtids::gno_t>(0));
    if (!ParseSingleGtid(gtid_str, &gtid, error)) return false;
    if (set.add(gtid)) {
      if (error) *error = "Failed to add GTID to set";
      return false;
    }
  }
  *out_set = set.to_string();
  return true;
}

inline bool MergeGtidIntoSet(const std::string &gtid_set,
                             const std::string &gtid,
                             std::string *out_set, std::string *error) {
  if (!out_set) return false;
  binary_log::gtids::Gtid_set set;
  if (!ParseGtidSetString(gtid_set, &set, error)) return false;
  binary_log::gtids::Gtid parsed(binary_log::gtids::Uuid(),
                                 static_cast<binary_log::gtids::gno_t>(0));
  if (!ParseSingleGtid(gtid, &parsed, error)) return false;
  if (set.add(parsed)) {
    if (error) *error = "Failed to merge GTID into set";
    return false;
  }
  *out_set = set.to_string();
  return true;
}

inline bool GtidSetContains(const std::string &gtid_set,
                            const std::string &gtid, bool *contains,
                            std::string *error) {
  if (!contains) return false;
  *contains = false;
  if (gtid_set.empty()) return true;
  binary_log::gtids::Gtid_set set;
  if (!ParseGtidSetString(gtid_set, &set, error)) return false;
  binary_log::gtids::Gtid parsed(binary_log::gtids::Uuid(),
                                 static_cast<binary_log::gtids::gno_t>(0));
  if (!ParseSingleGtid(gtid, &parsed, error)) return false;
  *contains = set.contains(parsed);
  return true;
}

}  // namespace duckdb_se

#endif  // PLUGIN_DUCKDB_GTID_UTILS_H_
