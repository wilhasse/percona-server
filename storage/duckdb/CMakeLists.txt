# Copyright (c) 2026, Percona LLC and/or its affiliates.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# This program is designed to work with certain software (including
# but not limited to OpenSSL) that is licensed under separate terms,
# as designated in a particular file or component or in included license
# documentation.  The authors of MySQL hereby grant you an additional
# permission to link the program and your derivative works with the
# separately licensed software that they have either included with
# the program or referenced in the documentation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

DISABLE_MISSING_PROFILE_WARNING()
ADD_DEFINITIONS(-DMYSQL_SERVER)

if(DEFINED WITH_DUCKDB_STORAGE_ENGINE AND NOT DEFINED WITH_DUCKDB_SE_STORAGE_ENGINE)
  set(WITH_DUCKDB_SE_STORAGE_ENGINE ${WITH_DUCKDB_STORAGE_ENGINE} CACHE BOOL "" FORCE)
endif()
if(DEFINED WITHOUT_DUCKDB_STORAGE_ENGINE AND NOT DEFINED WITHOUT_DUCKDB_SE_STORAGE_ENGINE)
  set(WITHOUT_DUCKDB_SE_STORAGE_ENGINE ${WITHOUT_DUCKDB_STORAGE_ENGINE} CACHE BOOL "" FORCE)
endif()

if(NOT WITHOUT_DUCKDB_SE_STORAGE_ENGINE)
  set(DUCKDB_SOURCE_DIR ${CMAKE_SOURCE_DIR}/extra/duckdb)
  if(EXISTS ${DUCKDB_SOURCE_DIR}/CMakeLists.txt)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(BUILD_MAIN_DUCKDB_LIBRARY ON CACHE BOOL "" FORCE)
    set(BUILD_UNITTESTS OFF CACHE BOOL "" FORCE)
    set(ENABLE_UNITTEST_CPP_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHELL OFF CACHE BOOL "" FORCE)
    set(DISABLE_EXTENSION_LOAD ON CACHE BOOL "" FORCE)
    set(DISABLE_BUILTIN_EXTENSIONS ON CACHE BOOL "" FORCE)
    set(ENABLE_EXTENSION_AUTOINSTALL OFF CACHE BOOL "" FORCE)
    set(ENABLE_EXTENSION_AUTOLOADING OFF CACHE BOOL "" FORCE)
    set(ENABLE_SANITIZER OFF CACHE BOOL "" FORCE)
    set(ENABLE_UBSAN OFF CACHE BOOL "" FORCE)
    set(ENABLE_THREAD_SANITIZER OFF CACHE BOOL "" FORCE)
    # Force C++17 to override DuckDB's default of C++11
    set(CMAKE_CXX_STANDARD 17 CACHE STRING "" FORCE)
    set(CMAKE_CXX_STANDARD_REQUIRED ON CACHE BOOL "" FORCE)
    # Avoid static TLS allocation failures when the plugin is loaded via dlopen.
    # Using global-dynamic TLS removes the need for LD_PRELOAD on glibc systems.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      set(DUCKDB_TLS_MODEL_FLAG "-ftls-model=global-dynamic")
      set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${DUCKDB_TLS_MODEL_FLAG}")
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${DUCKDB_TLS_MODEL_FLAG}")
    endif()
    # Strip -Werror from flags before entering the DuckDB subdirectory.
    # DuckDB's third-party code triggers warnings that Percona's global -Werror
    # would turn into build failures (-Wundef, -Wextra-semi, etc.).
    set(_saved_C_FLAGS "${CMAKE_C_FLAGS}")
    set(_saved_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REPLACE "-Werror" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
    string(REPLACE "-Werror" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    add_subdirectory(${DUCKDB_SOURCE_DIR} ${CMAKE_BINARY_DIR}/duckdb EXCLUDE_FROM_ALL)
    set(CMAKE_C_FLAGS "${_saved_C_FLAGS}")
    set(CMAKE_CXX_FLAGS "${_saved_CXX_FLAGS}")
  else()
    message(FATAL_ERROR "DuckDB submodule not found at ${DUCKDB_SOURCE_DIR}")
  endif()

  if(WITH_DUCKDB_SE_STORAGE_ENGINE)
    MYSQL_ADD_PLUGIN(duckdb_se ha_duckdb.cc duckdb_adapter.cc duckdb_binlog_applier.cc duckdb_binlog_apply_thread.cc duckdb_binlog_streamer.cc duckdb_row_decoder.cc duckdb_compat.cc
      STORAGE_ENGINE
      DEFAULT
      MODULE_OUTPUT_NAME ha_duckdb
      LINK_LIBRARIES duckdb_static duckdb_generated_extension_loader core_functions_extension extra::rapidjson binlogevents_static perconaserverclient
      )
  else()
    MYSQL_ADD_PLUGIN(duckdb_se ha_duckdb.cc duckdb_adapter.cc duckdb_binlog_applier.cc duckdb_binlog_apply_thread.cc duckdb_binlog_streamer.cc duckdb_row_decoder.cc duckdb_compat.cc
      STORAGE_ENGINE
      MODULE_ONLY
      MODULE_OUTPUT_NAME ha_duckdb
      LINK_LIBRARIES duckdb_static duckdb_generated_extension_loader core_functions_extension extra::rapidjson binlogevents_static perconaserverclient
      )
  endif()

  if(TARGET duckdb_se)
    # Use SYSTEM include to suppress warnings from DuckDB headers that would
    # otherwise be promoted to errors by Percona's global -Werror.
    target_include_directories(duckdb_se SYSTEM PRIVATE
      ${DUCKDB_SOURCE_DIR}/src/include
      ${DUCKDB_SOURCE_DIR}/third_party/fmt/include
      )
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      # Enforce TLS model on the plugin target, even if the subdir was cached.
      target_compile_options(duckdb_se PRIVATE ${DUCKDB_TLS_MODEL_FLAG})
    endif()
  endif()

  if(TARGET duckdb_static)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      # Enforce TLS model on DuckDB static libs and extensions.
      foreach(duckdb_tls_target duckdb_static duckdb_generated_extension_loader core_functions_extension)
        if(TARGET ${duckdb_tls_target})
          target_compile_options(${duckdb_tls_target} PRIVATE ${DUCKDB_TLS_MODEL_FLAG})
        endif()
      endforeach()
    endif()
    MYSQL_ADD_EXECUTABLE(duckdb_bench
      duckdb_bench.cc
      duckdb_adapter.cc
      duckdb_binlog_applier.cc
      ENABLE_EXPORTS
      EXCLUDE_FROM_ALL
      LINK_LIBRARIES duckdb_static dummy_static_extension_loader extra::rapidjson
    )
    TARGET_INCLUDE_DIRECTORIES(duckdb_bench PRIVATE
      ${DUCKDB_SOURCE_DIR}/src/include
    )

    MYSQL_ADD_EXECUTABLE(duckdb_snapshot_loader
      duckdb_snapshot_loader.cc
      duckdb_adapter.cc
      duckdb_binlog_applier.cc
      duckdb_binlog_streamer.cc
      duckdb_row_decoder.cc
      ENABLE_EXPORTS
      EXCLUDE_FROM_ALL
      LINK_LIBRARIES duckdb_static dummy_static_extension_loader perconaserverclient binlogevents_static
    )
    TARGET_INCLUDE_DIRECTORIES(duckdb_snapshot_loader PRIVATE
      ${DUCKDB_SOURCE_DIR}/src/include
    )
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      target_compile_options(duckdb_bench PRIVATE ${DUCKDB_TLS_MODEL_FLAG})
      target_compile_options(duckdb_snapshot_loader PRIVATE ${DUCKDB_TLS_MODEL_FLAG})
    endif()
  endif()

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Ensure MySQL client/static deps that are linked into the plugin also use
    # global-dynamic TLS, so we don't inherit static TLS from initial-exec builds.
    foreach(duckdb_tls_target mysys_objlib mytime_objlib strings_objlib vio_objlib clientlib_objlib perconaserverclient)
      if(TARGET ${duckdb_tls_target})
        target_compile_options(${duckdb_tls_target} PRIVATE ${DUCKDB_TLS_MODEL_FLAG})
      endif()
    endforeach()
  endif()
endif()
