# Copyright (c) 2006, 2024, Oracle and/or its affiliates.
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

# Add LMDB source files
SET(LMDB_SOURCES
  ha_lmdb.cc
  mdb.c
  midl.c
)

DISABLE_MISSING_PROFILE_WARNING()
ADD_DEFINITIONS(-DMYSQL_SERVER)

# Add include directory for LMDB headers
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR})

IF(WITH_LMDB_STORAGE_ENGINE AND NOT WITHOUT_LMDB_STORAGE_ENGINE)
  MYSQL_ADD_PLUGIN(lmdb ${LMDB_SOURCES}
    STORAGE_ENGINE
    DEFAULT
    LINK_LIBRARIES ext::zlib
    )
ELSEIF(NOT WITHOUT_LMDB_STORAGE_ENGINE)
  MYSQL_ADD_PLUGIN(lmdb ${LMDB_SOURCES}
    STORAGE_ENGINE
    MODULE_ONLY
    LINK_LIBRARIES ext::zlib
    )
ENDIF()

# Add LMDB compilation flags if necessary
TARGET_COMPILE_DEFINITIONS(lmdb PRIVATE -DMDB_USE_POSIX_SEM)

# Add these lines to your existing CMakeLists.txt
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-discarded-qualifiers -Wno-unused-parameter -Wno-jump-misses-init")

# For C++ files, we need different flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-parameter")

# If you're using target_compile_options, modify it like this:
target_compile_options(lmdb PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-Wno-discarded-qualifiers -Wno-unused-parameter -Wno-jump-misses-init>
  $<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-parameter>
)

