#ifndef MINIMAL_IBD_ENC_READER_H
#define MINIMAL_IBD_ENC_READER_H

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <iostream>
#include "plugin/keyring/common/keyring.h"

// Minimal: Magic size is always 3
static constexpr size_t MAGIC_SIZE = 3;

// The known magic strings
static constexpr char KEY_MAGIC_V1[] = "lCA";
static constexpr char KEY_MAGIC_V2[] = "lCB";
static constexpr char KEY_MAGIC_V3[] = "lCC";

enum class EncVersion { V1, V2, V3 };

// This struct will hold the "decrypted" tablespace key & IV.
struct Tablespace_key_iv {
  unsigned char key[32];
  unsigned char iv[32];
};

/// A minimal function to read a 4-byte *big-endian* integer
static inline uint32_t read_u32_be(const unsigned char* ptr) {
    return ((uint32_t)ptr[0] << 24) |
           ((uint32_t)ptr[1] << 16) |
           ((uint32_t)ptr[2] << 8)  |
            (uint32_t)ptr[3];
}

// A placeholder for a simple or partial CRC32 if needed:
uint32_t calc_crc32(const unsigned char* data, size_t len);

bool decode_ibd_encryption_info(const unsigned char *enc_info,
                                bool decrypt_key,
                                const std::vector<unsigned char> &master_key,
                                Tablespace_key_iv &out_ts_key_iv);

// A placeholder for calling your keyring fetch code:
bool fetch_master_key(
  uint32_t master_key_id,
  const std::string& srv_uuid,
  std::vector<unsigned char>& out_master_key);

#endif // MINIMAL_IBD_ENC_READER_H
