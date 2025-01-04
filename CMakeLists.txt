MYSQL_ADD_EXECUTABLE(decompress
  decompress.cc
  COMPONENT Server
  DEPENDENCIES GenError
  LINK_LIBRARIES mysys innodb_zipdecompress ext::lz4 extra::rapidjson
)

TARGET_COMPILE_DEFINITIONS(decompress
  PRIVATE UNIV_NO_ERR_MSGS UNIV_LIBRARY DISABLE_PSI_MEMORY
)

IF(MY_COMPILER_IS_GNU_OR_CLANG)
  ADD_COMPILE_FLAGS(decompress.cc
    COMPILE_FLAGS "-Wno-unused-parameter -Wno-cast-qual"
  )
ENDIF()

set(DECRYPT_SOURCES
  decrypt.cc
  decrypt_aes.cc
  # If you need these for reading the keyring file:
  ${CMAKE_SOURCE_DIR}/plugin/keyring/buffered_file_io.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/common/keyring_impl.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/common/keys_container.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/digest.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/converter.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/file_io.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/checker/checker_factory.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/checker/checker.cc

  # Our stubs:
  ${CMAKE_CURRENT_SOURCE_DIR}/keyring_stubs.cc
)

MYSQL_ADD_EXECUTABLE(decrypt
  ${DECRYPT_SOURCES}
  COMPONENT Server
  DEPENDENCIES GenError
  LINK_LIBRARIES
    mysys
    ssl
    crypto
    # ext::lz4, extra::rapidjson, etc if you really need them
)

target_include_directories(decrypt
  PRIVATE
    ${CMAKE_SOURCE_DIR}/plugin/keyring
    ${CMAKE_SOURCE_DIR}/plugin/keyring/common
    # ...
)

target_compile_definitions(decrypt
  PRIVATE
    UNIV_NO_ERR_MSGS
    UNIV_LIBRARY
    DISABLE_PSI_MEMORY
)

# If you want to disable the warning only for logger.h code, you can do:
set_source_files_properties(  
  ${CMAKE_CURRENT_SOURCE_DIR}/decrypt.cc
    PROPERTIES
    COMPILE_FLAGS "-Wno-unused-parameter"
)

# Example: gather all .cc in plugin/keyring/ + subdirs
file(GLOB KEYRING_SOURCES
  "${CMAKE_SOURCE_DIR}/plugin/keyring/*.cc"
  "${CMAKE_SOURCE_DIR}/plugin/keyring/*/*.cc"
)

# Now disable -Wunused-parameter for all these sources:
set_source_files_properties(
  ${KEYRING_SOURCES}
  PROPERTIES
    COMPILE_FLAGS "-Wno-unused-parameter"
)

set_source_files_properties(
  ${KEYRING_SOURCES}
  PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-parameter"
)


# This must be separate flags, typically with escaped parentheses:
set_source_files_properties(
  ${CMAKE_SOURCE_DIR}/plugin/keyring/buffered_file_io.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/common/keyring_impl.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/common/keys_container.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/file_io.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/checker/checker_factory.cc
  ${CMAKE_SOURCE_DIR}/plugin/keyring/checker/checker.cc
  PROPERTIES
    COMPILE_FLAGS
      "-DLogPluginErr\\(level,errcode,message\\)=do{}while\\(0\\) -DLogPluginErrV\\(level,errcode,vl\\)=do{}while\\(0\\)"
)
