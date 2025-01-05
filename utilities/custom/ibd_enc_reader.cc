#include "ibd_enc_reader.h"
#include "plugin/keyring/common/keys_container.h"
#include "my_aes.h"

#include "keyring_operations_helper.h"  // for keyring_operations_helper::read_secret
#include "my_sys.h"                    // for my_free(...)
#include <vector>
#include <cstring>
#include <iostream>

bool decode_ibd_encryption_info(const unsigned char *enc_info,
                                bool decrypt_key,
                                const std::vector<unsigned char> &master_key,
                                Tablespace_key_iv &out_ts_key_iv)

{
  // 1) Check magic
  EncVersion version;
  if (std::memcmp(enc_info, "lCA", 3) == 0) {
    version = EncVersion::V1;
  } else if (std::memcmp(enc_info, "lCB", 3) == 0) {
    version = EncVersion::V2;
  } else if (std::memcmp(enc_info, "lCC", 3) == 0) {
    version = EncVersion::V3;
  } else {
    std::cerr << "Unexpected encryption magic bytes\n";
    return false;
  }
  enc_info += 3;

  // 2) Read server uuid if needed
  std::string srv_uuid;
  if (version == EncVersion::V2 || version == EncVersion::V3) {
    char uuid_buf[37];
    std::memcpy(uuid_buf, enc_info, 36);
    uuid_buf[36] = '\0';
    srv_uuid = uuid_buf;
    enc_info += 36;
  }

  // 3) read 64 bytes of possibly-encrypted (tablespace key+IV)
  unsigned char key_info[64];
  std::memcpy(key_info, enc_info, 64);
  enc_info += 64;

  // 4) read 4-byte checksum
  uint32_t stored_crc = read_u32_le(enc_info);
  // enc_info += 4; // optional

  // 5) If decrypt:
  if (decrypt_key) {

    // Use MySQL’s my_aes_decrypt for AES-256-ECB, no padding
    unsigned char decrypted[64];
    std::memset(decrypted, 0, sizeof(decrypted));
    int ret = my_aes_decrypt(
        key_info, 64,
        decrypted,
        master_key.data(),
        static_cast<uint32>(master_key.size()),
        my_aes_256_ecb,
        /*iv=*/nullptr,
        /*padding=*/false,
        /*kdf_options=*/nullptr);

    if (ret == MY_AES_BAD_DATA) {
      std::cerr << "my_aes_decrypt returned MY_AES_BAD_DATA.\n";
      return false;
    }
    // copy plaintext back
    std::memcpy(key_info, decrypted, 64);
  } else {
    // interpret key_info as plaintext
    //W ...
  }

  // 6) check crc
  uint32_t calc = calc_crc32(key_info, 64);
  if (calc != stored_crc) {
    std::cerr << "Checksum mismatch!\n";
    //return false;
  }

  // 7) first 32 bytes => tablespace key, next 32 => IV
  std::memcpy(out_ts_key_iv.key, key_info,     32);
  std::memcpy(out_ts_key_iv.iv,  key_info + 32, 32);

  return true;
}

#include <zlib.h>
#include <cstdint>

uint32_t calc_crc32(const unsigned char* data, size_t len)
{
    // The second parameter is the current CRC, which is typically
    // initialized to 0 (or Z_NULL for the “initial”).
    uLong crc = crc32(0L, Z_NULL, 0);

    // Now update the CRC with your data
    crc = crc32(crc, reinterpret_cast<const Bytef*>(data), static_cast<uInt>(len));

    return static_cast<uint32_t>(crc);
}
