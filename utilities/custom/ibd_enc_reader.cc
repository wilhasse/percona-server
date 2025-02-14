#include <cstdint>
#include <vector>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <sstream>

#include "ibd_enc_reader.h"
#include "plugin/keyring/common/keys_container.h"
#include "my_aes.h"
#include "my_sys.h"
#include "mysql_crc32c.h"

/**
 * This utility function provides several ways to view your Tablespace_key_iv structure
 * 
 * A traditional hex dump format with offset, hexadecimal values, and ASCII representation
 * Separate displays of the key and IV as continuous hex strings
 */
void print_hex_dump(const Tablespace_key_iv& data, bool include_ascii = true) {
    std::stringstream ss;
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(&data);
    const size_t total_size = sizeof(Tablespace_key_iv);
    
    // Print header
    ss << "Tablespace Key and IV\n";
    ss << "Offset    | Hexadecimal                                                | ASCII\n";
    ss << "----------|------------------------------------------------------------|-----------------\n";
    
    // Process 16 bytes per line
    for (size_t i = 0; i < total_size; i += 16) {
        // Print offset
        ss << std::setw(8) << std::setfill('0') << std::hex << i << " | ";
        
        // Print hex values
        for (size_t j = 0; j < 16; j++) {
            if (i + j < total_size) {
                ss << std::setw(2) << std::setfill('0') << std::hex 
                   << static_cast<int>(ptr[i + j]) << " ";
            } else {
                ss << "   "; // Padding for incomplete last line
            }
        }
        
        if (include_ascii) {
            // Print ASCII representation
            ss << "| ";
            for (size_t j = 0; j < 16; j++) {
                if (i + j < total_size) {
                    char c = ptr[i + j];
                    ss << (isprint(c) ? c : '.');
                }
            }
        }
        ss << "\n";
    }
    
    // Print key and IV separately
    ss << "\nKey (32 bytes):\n";
    for (size_t i = 0; i < 32; i++) {
        ss << std::setw(2) << std::setfill('0') << std::hex 
           << static_cast<int>(data.key[i]);
    }
    
    ss << "\n\nIV (32 bytes):\n";
    for (size_t i = 0; i < 32; i++) {
        ss << std::setw(2) << std::setfill('0') << std::hex 
           << static_cast<int>(data.iv[i]);
    }
    
    std::cout << ss.str() << std::endl;
}

/**
 * Decrypt the tablespace key + IV from the encryption info
 * using the (plaintext) master key from above.
 *
 * We now use MySQL's my_aes_decrypt(...) in AES-256-ECB mode,
 * with no padding, matching how InnoDB encrypts the chunk.
 */
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

  // 2) master_key_id,if 0 old 5.7 skip it
  uint32_t key_id = read_u32_be(enc_info);
  if (key_id == 0) {
    enc_info += 8;
  } else {
    enc_info += 4;
  }

  // 3) Read server uuid if needed
  std::string srv_uuid;
  if (version == EncVersion::V2 || version == EncVersion::V3) {
    char uuid_buf[37];
    std::memcpy(uuid_buf, enc_info, 36);
    uuid_buf[36] = '\0';
    srv_uuid = uuid_buf;
    enc_info += 36;
  }

  // 4) read 64 bytes of possibly-encrypted (tablespace key+IV)
  unsigned char key_info[64];
  std::memcpy(key_info, enc_info, 64);
  enc_info += 64;

  // 5) read 4-byte checksum
  uint32_t stored_crc = read_u32_be(enc_info);

  // 6) If decrypt:
  if (decrypt_key) {

    // Use MySQL’s my_aes_decrypt for AES-256-ECB, no padding
    unsigned char decrypted[64];
    std::memset(decrypted, 0, sizeof(decrypted));
    // Call MySQL's my_aes_decrypt with mode = my_aes_256_ecb, no IV, no padding
    int ret = my_aes_decrypt(
        /* source        = */ key_info,
        /* source_length = */ 64,
        /* dest          = */ decrypted,
        /* key           = */ master_key.data(),
        /* key_length    = */ static_cast<uint32>(master_key.size()),
        /* mode          = */ my_aes_256_ecb,   // AES-256-ECB
        /* iv            = */ nullptr,          // no IV in ECB
        /* padding       = */ false,            // InnoDB uses no padding
        /* kdf_options   = */ nullptr);         // not used for basic decrypt

    if (ret == MY_AES_BAD_DATA) {
      std::cerr << "my_aes_decrypt returned MY_AES_BAD_DATA.\n";
      return false;
    }
    // copy plaintext back
    std::memcpy(key_info, decrypted, sizeof(key_info));
  } else {
    // interpret key_info as plaintext
    //W ...
  }

  // 7) check crc
  mysql_crc32c_init();
  uint32_t calc = mysql_crc32c(key_info, 64);
  if (calc != stored_crc) {
    std::cerr << "Checksum mismatch! Calculated CRC: " << calc
              << ", Stored CRC: " << stored_crc << "\n";
    return false;
  } else {
    std::cerr << "Checksum CRC: " << calc << "\n";
  }

  // 8) first 32 bytes => tablespace key, next 32 => IV
  std::memcpy(out_ts_key_iv.key, key_info,     32);
  std::memcpy(out_ts_key_iv.iv,  key_info + 32, 32);

  // Print for debugging
  print_hex_dump(out_ts_key_iv);

  return true;
}