# CMakeLists.txt for building a single "ib_parser" utility

cmake_minimum_required(VERSION 3.10)
project(MyIbParser LANGUAGES CXX)

# Example: C++17
set(CMAKE_CXX_STANDARD 17)
add_compile_options(-Wno-cast-qual -Wall -Wextra -Wno-unused-parameter)

# If you have a "GenError" or "COMPONENT Server" reference from MySQL macros,
# you can adapt them or remove them if you're not actually using them.
# We'll do a straightforward "add_executable" example.

# -- Provide a list of source files for decryption logic (excluding the old main)
set(DECRYPT_LIB_SOURCES
  decrypt.cc        # <--- This file has e.g. decrypt_page_inplace(), get_master_key(), etc.
  ibd_enc_reader.cc
  my_keyring_lookup.cc
  mysql_crc32c.cc
  keyring_stubs.cc

  # plus all your plugin/keyring .cc if needed
  ${CMAKE_SOURCE_DIR}/plugin/keyring/buffered_file_io.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/common/keyring_key.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/common/keyring_impl.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/common/keys_container.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/digest.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/converter.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/file_io.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/buffer.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/checker/checker_factory.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/checker/checker.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/checker/checker_ver_1_0.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/checker/checker_ver_2_0.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/common/keys_iterator.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/common/i_keys_container.h
  ${CMAKE_SOURCE_DIR}/plugin/keyring/hash_to_buffer_serializer.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/common/i_keyring_key.h
  ${CMAKE_SOURCE_DIR}/plugin/keyring/common/i_serializer.h
  ${CMAKE_SOURCE_DIR}/plugin/keyring/common/logger.h
  ${CMAKE_SOURCE_DIR}/strings/ctype-latin1.cc
)

# -- Provide a list of source files for decompress logic (excluding the old main)
set(DECOMPRESS_LIB_SOURCES
  decompress.cc     # <--- This has decompress_page_inplace(), decompress_ibd(), etc.
)

# -- The main entry point: "ib_parser.cc"
set(IB_PARSER_MAIN
  ib_parser.cc
  parser.cc
  tables_dict.cc
  undrop_for_innodb.cc
)

# Combine them all for building a single binary "ib_parser"
MYSQL_ADD_EXECUTABLE(ib_parser
  ${IB_PARSER_MAIN}
  ${DECRYPT_LIB_SOURCES}
  ${DECOMPRESS_LIB_SOURCES}
  COMPONENT Server
  DEPENDENCIES GenError
  LINK_LIBRARIES mysys innodb_zipdecompress ext::lz4
)

# Add RapidJSON include path
# Adjust the path below to point to your downloaded rapidjson include directory
set(RAPIDJSON_INCLUDE_DIR "/home/cslog/rapidjson/include")

# Verify that the path exists
if(NOT EXISTS "${RAPIDJSON_INCLUDE_DIR}")
    message(FATAL_ERROR "RapidJSON include directory not found: ${RAPIDJSON_INCLUDE_DIR}")
endif()

# Example: include directories
target_include_directories(ib_parser
  PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/storage/innobase/include
    ${CMAKE_SOURCE_DIR}/include
    ${RAPIDJSON_INCLUDE_DIR}
)

# If you need compile definitions
target_compile_definitions(ib_parser
  PRIVATE
    UNIV_NO_ERR_MSGS
    UNIV_LIBRARY
    DISABLE_PSI_MEMORY
)

# If you want to disable warnings:
set_source_files_properties(
  ${IB_PARSER_MAIN}
  ${DECRYPT_LIB_SOURCES}
  ${DECOMPRESS_LIB_SOURCES}
  PROPERTIES
    COMPILE_FLAGS "-Wno-unused-parameter"
)

# This must be separate flags, typically with escaped parentheses:
set_source_files_properties(
  ${CMAKE_SOURCE_DIR}/plugin/keyring/buffered_file_io.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/common/keyring_key.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/common/keyring_impl.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/common/keys_container.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/file_io.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/buffer.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/checker/checker_factory.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/checker/checker.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/checker/checker_ver_1_0.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/checker/checker_ver_2_0.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/common/keys_iterator.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/hash_to_buffer_serializer.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/ib_parser.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/ibd_enc_reader.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/ibd_enc_reader.h
  ${CMAKE_CURRENT_SOURCE_DIR}/my_keyring_lookup.cc
  PROPERTIES
    COMPILE_FLAGS
      "-DLogPluginErr\\(level,errcode,message\\)=do{}while\\(0\\) -DLogPluginErrV\\(level,errcode,vl\\)=do{}while\\(0\\)"
)

# Done. "make" => builds "ib_parser".
