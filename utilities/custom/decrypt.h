// ---------------
// Declarations from decrypt.cc
// ---------------
#include <cstdint>
#include <string>
#include <vector>

/** This struct is presumably declared in your "ibd_enc_reader.h" or similar. */
struct Tablespace_key_iv {
  unsigned char key[32]; // 32-byte encryption key
  unsigned char iv[32];  // 32-byte IV
};

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