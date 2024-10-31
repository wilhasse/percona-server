# storage/cslog/CMakeLists.txt
# Define source files
SET(CSLOG_SOURCES
  ha_cslog.cc
)

# Check if RocksDB is enabled
IF(NOT WITH_ROCKSDB)
  MESSAGE(FATAL_ERROR "RocksDB is required for CSLOG storage engine")
ENDIF()

# Define plugin directory with absolute path
GET_FILENAME_COMPONENT(PLUGIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/plugin_output_directory" ABSOLUTE)

# Create shared library target
MYSQL_ADD_PLUGIN(cslog
  ${CSLOG_SOURCES}
  STORAGE_ENGINE
  MODULE_ONLY
  LINK_LIBRARIES rocksdb
)

# Add RocksDB dependencies
ADD_DEPENDENCIES(cslog rocksdb)
IF(TARGET rocksdb_se)
  ADD_DEPENDENCIES(cslog rocksdb_se)
ENDIF()

# Add necessary include directories
TARGET_INCLUDE_DIRECTORIES(cslog
  PRIVATE
  ${CMAKE_SOURCE_DIR}/storage/heap
  ${CMAKE_SOURCE_DIR}/storage/rocksdb
  ${CMAKE_SOURCE_DIR}/storage/rocksdb/rocksdb
  ${CMAKE_SOURCE_DIR}/storage/rocksdb/rocksdb/include
  ${CMAKE_SOURCE_DIR}/storage/rocksdb/rdb_utils
  ${CMAKE_SOURCE_DIR}/sql
  ${CMAKE_SOURCE_DIR}/include
)

# Set compile definitions
TARGET_COMPILE_DEFINITIONS(cslog
  PRIVATE
  MYSQL_SERVER
  HAVE_ROCKSDB=1
)

# Set proper linking flags with absolute path to build directory
SET_TARGET_PROPERTIES(cslog
  PROPERTIES
    SKIP_BUILD_RPATH FALSE
    BUILD_WITH_INSTALL_RPATH FALSE
    INSTALL_RPATH "${PLUGIN_OUTPUT_DIR}"
    LINK_FLAGS "-Wl,-rpath,${PLUGIN_OUTPUT_DIR} -Wl,-z,nodelete"
)

# Create minimal script just to fix library name
FILE(WRITE ${CMAKE_BINARY_DIR}/fix_lib.sh
"#!/bin/bash
cd \"${PLUGIN_OUTPUT_DIR}\"
patchelf --replace-needed \"../../plugin_output_directory/ha_rocksdb.so\" \"ha_rocksdb.so\" ha_cslog.so
")

# Make script executable
FILE(CHMOD ${CMAKE_BINARY_DIR}/fix_lib.sh
     FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
)

# Run minimal script after build
ADD_CUSTOM_COMMAND(TARGET cslog POST_BUILD
  COMMAND bash ${CMAKE_BINARY_DIR}/fix_lib.sh
)

# Set installation directory
INSTALL(TARGETS cslog
  LIBRARY 
    DESTINATION ${INSTALL_PLUGINDIR}
  COMPONENT 
    Server
)

# Also install RocksDB plugin
INSTALL(FILES ${PLUGIN_OUTPUT_DIR}/ha_rocksdb.so
  DESTINATION ${INSTALL_PLUGINDIR}
  COMPONENT Server
)