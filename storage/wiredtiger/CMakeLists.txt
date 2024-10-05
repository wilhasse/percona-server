# Add WIREDTIGER source files
SET(WIREDTIGER_SOURCES
  ha_wt.cc
)

DISABLE_MISSING_PROFILE_WARNING()
ADD_DEFINITIONS(-DMYSQL_SERVER)

# Add include directory for WIREDTIGER headers
INCLUDE_DIRECTORIES(
  ${CMAKE_CURRENT_SOURCE_DIR}
  /usr/local/include
)

# Add library directory for WIREDTIGER
LINK_DIRECTORIES(/usr/local/lib)

# Find the WiredTiger library
FIND_LIBRARY(WIREDTIGER_LIB wiredtiger HINTS /usr/local/lib)

IF(NOT WIREDTIGER_LIB)
  MESSAGE(FATAL_ERROR "Could not find WiredTiger library.")
ENDIF()

IF(WITH_WIREDTIGER_STORAGE_ENGINE AND NOT WITHOUT_WIREDTIGER_STORAGE_ENGINE)
  MYSQL_ADD_PLUGIN(wt ${WIREDTIGER_SOURCES}
    STORAGE_ENGINE
    DEFAULT
    LINK_LIBRARIES ${WIREDTIGER_LIB}
    )
ELSEIF(NOT WITHOUT_WIREDTIGER_STORAGE_ENGINE)
  MYSQL_ADD_PLUGIN(wt ${WIREDTIGER_SOURCES}
    STORAGE_ENGINE
    MODULE_ONLY
    LINK_LIBRARIES ${WIREDTIGER_LIB}
    )
ENDIF()

# Add WT compilation flags if necessary
TARGET_COMPILE_DEFINITIONS(wt PRIVATE -DMDB_USE_POSIX_SEM)

# Add these lines to your existing CMakeLists.txt
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-discarded-qualifiers -Wno-unused-parameter -Wno-jump-misses-init")

# For C++ files, we need different flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-parameter")

# If you're using target_compile_options, modify it like this:
target_compile_options(wt PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-Wno-discarded-qualifiers -Wno-unused-parameter -Wno-jump-misses-init>
  $<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-parameter>
)
