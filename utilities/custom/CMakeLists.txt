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
