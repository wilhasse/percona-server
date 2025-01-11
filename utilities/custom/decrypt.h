// ---------------
// Declarations from decrypt.cc
// ---------------
#include <cstdint>
#include <string>
#include <vector>
#include "ibd_enc_reader.h"

// ----------------------------------------------------------------
// This is the simplified page size for InnoDB pages
// Typically it's 16KB, but adapt if your system differs
//static const size_t PAGE_SIZE = 16384;
static const size_t PAGE_SIZE = 8192;

extern bool get_master_key(uint32_t              master_id,
                           const std::string    &server_uuid,
                           const char*           keyring_path,
                           std::vector<unsigned char> &out_master_key);

extern bool read_tablespace_key_iv(const char*                      ibd_path,
                                   long                             offset,
                                   const std::vector<unsigned char> &master_key,
                                   Tablespace_key_iv                &ts_key_iv);

/** Decrypt a single page in memory (page_data). */
extern bool decrypt_page_inplace(
    unsigned char*       page_data,
    size_t               page_len,
    const unsigned char* key,
    size_t               key_len,
    const unsigned char* iv,
    size_t               block_size
);

// Decrypt file
bool decrypt_ibd_file(const char* src_ibd_path,
                      const char* dst_path,
                      const Tablespace_key_iv &ts_key_iv);