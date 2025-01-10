// File: decrypt.cc

// Provide stubs for LogPluginErr / LogPluginErrV first:
#define LogPluginErr(level, errcode, message) do {} while (0)
#define LogPluginErrV(level, errcode, vl) do {} while (0)

// Keyring code (which indirectly includes logger.h)
#include "plugin/keyring/common/keyring.h"
#include "plugin/keyring/buffered_file_io.h"
#include "my_keyring_lookup.h"
#include "plugin/keyring/common/keys_container.h"

#include <openssl/evp.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <cstdint>         // for uint16_t, etc.

#include <my_sys.h>
#include <my_thread.h>
#include "mysys_err.h"     // for MY_AES_BAD_DATA constant, etc.
#include "my_aes.h"

// Custom
#include "ibd_enc_reader.h" // for decode_ibd_encryption_info()

// ----------------------------------------------------------------
// This is the simplified page size for InnoDB pages
// Typically it's 16KB, but adapt if your system differs
static const size_t PAGE_SIZE = 16384;
//static const size_t PAGE_SIZE = 8192;

/* Some MySQL/Innodb constants you’ll need: */
static const size_t FIL_PAGE_DATA     = 38;   // Typical offset for raw page data
static const size_t FIL_PAGE_TYPE     = 24;   // Offset of 2-byte page type
static const size_t FIL_PAGE_VERSION  = 26;
static const size_t FIL_PAGE_COMPRESS_SIZE_V1   = 64;
static const size_t FIL_PAGE_ORIGINAL_SIZE_V1   = 66;
static const size_t FIL_PAGE_ALGORITHM_V1       = FIL_PAGE_VERSION + 1;
static const uint32_t FIL_PAGE_ORIGINAL_TYPE_V1 = FIL_PAGE_ALGORITHM_V1 + 1;
static const uint16_t FIL_PAGE_COMPRESSED               = 14;
static const uint16_t FIL_PAGE_ENCRYPTED                = 15;
static const uint16_t FIL_PAGE_COMPRESSED_AND_ENCRYPTED = 16;
static const uint16_t FIL_PAGE_ENCRYPTED_RTREE          = 17;
static const uint16_t FIL_PAGE_RTREE = 17854;
// ^ In real code, confirm these offsets/types match your MySQL version!

// a minimal stand-in for reading from page, typical MySQL macros
inline uint8_t mach_read_from_1(const unsigned char* ptr) {
  return ptr[0];
}

inline uint16_t mach_read_from_2(const unsigned char* ptr) {
  return (ptr[0] << 8) | ptr[1];
}

inline void mach_write_to_2(unsigned char* ptr, uint16_t val) {
  ptr[0] = static_cast<unsigned char>((val >> 8) & 0xFF);
  ptr[1] = static_cast<unsigned char>( val       & 0xFF);
}

// Decrypts an obfuscated key using MySQL's keyring XOR method
void keyring_deobfuscate(unsigned char* key_data, size_t key_len) {
    const char* obfuscate_str = "*305=Ljt0*!@$Hnm(*-9-w;:";
    const size_t obfuscate_len = strlen(obfuscate_str);
    
    for (size_t i = 0, l = 0; i < key_len; i++, l = ((l + 1) % obfuscate_len)) {
        key_data[i] ^= obfuscate_str[l];
    }
}

// ----------------------------------------------------------------
// NEW: A function to retrieve + deobfuscate the InnoDB master key
//      from the keyring file, using your existing logic.
//
//      Returns true on success, false on error.
// ----------------------------------------------------------------
bool get_master_key(uint32_t       master_id,
                    const std::string &server_uuid,
                    const char*    keyring_path,
                    std::vector<unsigned char> &out_master_key)
{
  // Init MySQL structures if needed (or do this in main)
  // my_init();  // Typically done in main()
  // my_thread_init(); // Typically done in main()

  using keyring::Buffered_file_io;
  using keyring::Keys_container;
  using keyring::Logger;

  // 1) Load the keyring
  std::unique_ptr<Logger> logger(new Logger());
  std::unique_ptr<Keys_container> keys(new Keys_container(logger.get()));

  std::vector<std::string> allowedFileVersions{
    keyring::keyring_file_version_2_0,
    keyring::keyring_file_version_1_0
  };
  auto* keyring_io = new Buffered_file_io(logger.get(), &allowedFileVersions);

  if (keys->init(keyring_io, keyring_path)) {
    std::cerr << "Failed to load keyring from " << keyring_path << "\n";
    return false;
  }
  std::cout << "Loaded keyring from: " << keyring_path << "\n";

  // 2) Make MyKeyringLookup
  MyKeyringLookup lookup(keys.get());

  // 3) Attempt to get the InnoDB master key
  if (!lookup.get_innodb_master_key(server_uuid, master_id, out_master_key)) {
    std::cerr << "Could not find the master key in the container.\n";
    return false;
  }

  // 4) Master key is obfuscated
  keyring_deobfuscate(out_master_key.data(), out_master_key.size());

  std::cout << "Got master key length=" << out_master_key.size() << "\n"
            << "master_key = ";
  for (size_t i = 0; i < out_master_key.size(); i++) {
    std::printf("%02X", out_master_key[i]);
  }
  std::cout << "\n\n";

  return true;
}

// ----------------------------------------------------------------
// NEW: A function to read & decode the per-tablespace key/IV
//      from the .ibd file (the 80-100 byte header).
//      - 'offset' is the file offset where your encryption info starts
//      - 'master_key' is the 32-byte master key from get_master_key()
//      - writes result into 'ts_key_iv'
//      Returns true on success, false on error.
// ----------------------------------------------------------------
bool read_tablespace_key_iv(const char*           ibd_path,
                            long                 offset,
                            const std::vector<unsigned char> &master_key,
                            Tablespace_key_iv   &ts_key_iv)
{
  // 1) open .ibd
  FILE* f = std::fopen(ibd_path, "rb");
  if (!f) {
    std::cerr << "Cannot open .ibd: " << ibd_path << "\n";
    return false;
  }

  // 2) seek
  if (std::fseek(f, offset, SEEK_SET) != 0) {
    std::cerr << "Failed to fseek() to offset " << offset << " in .ibd file.\n";
    std::fclose(f);
    return false;
  }

  // 3) read 128 bytes
  unsigned char enc_info[128];
  std::memset(enc_info, 0, sizeof(enc_info));
  size_t n = std::fread(enc_info, 1, sizeof(enc_info), f);
  std::fclose(f);

  if (n < 80) {
    std::cerr << "Not enough data read for encryption info (got " << n << " bytes)\n";
    return false;
  }

  // 4) decode
  if (!decode_ibd_encryption_info(enc_info,
                                  /* decrypt_key */ true,
                                  master_key, // pass in the raw 32 bytes
                                  ts_key_iv))
  {
    std::cerr << "Failed to decode ibd encryption header.\n";
    return false;
  }

  std::cout << "Successfully read encryption info!\n";
  return true;
}

/**
 * @brief Checks whether a page is "encrypted" in the uncompressed sense.
 */
bool is_encrypted_page(const unsigned char* page_data) {
  const uint16_t page_type = mach_read_from_2(page_data + FIL_PAGE_TYPE);
  return (page_type == FIL_PAGE_ENCRYPTED ||
          page_type == FIL_PAGE_COMPRESSED_AND_ENCRYPTED ||
          page_type == FIL_PAGE_ENCRYPTED_RTREE);
}

/** Calculates the smallest multiple of m that is not smaller than n
    when m is a power of two. In other words, rounds n up to m*k.
    @param n  in: number to round up
    @param m  in: alignment, must be a power of two
    @return n rounded up to the smallest possible integer multiple of m */
#define ut_calc_align(n, m) (((n) + ((m) - 1)) & ~((m) - 1))

/** Example: A minimal structure to hold compression metadata */
namespace Compression {
struct meta_t {
  uint8_t  m_version;
  uint16_t m_original_type;
  uint16_t m_compressed_size;
  uint16_t m_original_size;
  uint8_t  m_algorithm;
};

/** Deserialise the page header compression meta-data */
inline void deserialize_header(const unsigned char* page, meta_t* control) {
  // For a real environment, ensure we have "is_compressed_page(page)" checks etc.
  control->m_version         = static_cast<uint8_t>(mach_read_from_1(page + FIL_PAGE_VERSION));
  control->m_original_type   = static_cast<uint16_t>(mach_read_from_2(page + FIL_PAGE_ORIGINAL_TYPE_V1));
  control->m_compressed_size = static_cast<uint16_t>(mach_read_from_2(page + FIL_PAGE_COMPRESS_SIZE_V1));
  control->m_original_size   = static_cast<uint16_t>(mach_read_from_2(page + FIL_PAGE_ORIGINAL_SIZE_V1));
  control->m_algorithm       = static_cast<uint8_t>(mach_read_from_1(page + FIL_PAGE_ALGORITHM_V1));
}
} // namespace Compression

/**
 * @brief Offline function that decrypts a single uncompressed page in-place
 *        using MySQL's partial-block approach, calling `my_aes_decrypt()`.
 *
 * @param page_data Pointer to the full page buffer.
 * @param page_len  Size of the full page (e.g. 16 KB).
 * @param key       32-byte AES key (AES-256) as used by MySQL (m_key).
 * @param key_len   e.g. 32 (for AES-256).
 * @param iv        32-byte IV from the tablespace header (m_iv).
 * @param size_t    e.g. OS block size: 512, 4096, etc.
 * @return true if successful, false if decryption failed.
 */
bool decrypt_page_inplace(
    unsigned char*       page_data,
    size_t               page_len,
    const unsigned char* key,
    size_t               key_len,
    const unsigned char* iv,
    size_t               block_size) 
{
    // 1) Check if it’s even an encrypted page
    if (!is_encrypted_page(page_data)) {
        // Nothing to decrypt (either not encrypted or not recognized as such)
        return true;
    }

    // 2) Read the page type and the original type
    const uint16_t page_type = mach_read_from_2(page_data + FIL_PAGE_TYPE);
    uint16_t       original_type
        = mach_read_from_2(page_data + FIL_PAGE_ORIGINAL_TYPE_V1);

  // 3) The actual data portion to decrypt is everything after FIL_PAGE_DATA
  //    (i.e., skip the InnoDB file header).
    if (page_len <= FIL_PAGE_DATA) {
    // Malformed / no space to decrypt
        return false;
    }
    
    // By default, decrypt everything from FIL_PAGE_DATA onward
    size_t src_len = page_len;

    // If it's compressed+encrypted, we might need to read the compression header
    if (page_type == FIL_PAGE_COMPRESSED_AND_ENCRYPTED) {
        // We'll use your "Compression::deserialize_header"
        Compression::meta_t header;
        Compression::deserialize_header(page_data, &header);

        uint16_t z_len = header.m_compressed_size;

        // The MySQL logic:
        //    src_len = z_len + FIL_PAGE_DATA
        src_len = z_len + FIL_PAGE_DATA;

        // For version 1, align to the OS block size
        if (header.m_version == 1 /* FIL_PAGE_VERSION_1 */) {
            src_len = ut_calc_align(src_len, block_size);
        }

        // If it’s smaller than some minimum, enforce that
        static const size_t MIN_ENCRYPTION_LEN = 64;
        if (src_len < MIN_ENCRYPTION_LEN) {
            src_len = MIN_ENCRYPTION_LEN;
        }
    }

    // Now the data portion to decrypt:
    unsigned char* ptr      = page_data + FIL_PAGE_DATA;
    size_t         data_len = src_len - FIL_PAGE_DATA;  // actual region to decrypt

    // For MySQL partial-block logic:
    size_t main_len   = (data_len / MY_AES_BLOCK_SIZE) * MY_AES_BLOCK_SIZE;
    size_t remain_len = data_len - main_len;

  // We need a temp buffer for partial-block decrypt
  // or for the entire chunk if we want to replicate MySQL exactly.
  // In real MySQL code, 'tmp' might be allocated from a block pool.
    unsigned char* tmp_buf = new unsigned char[data_len];
    unsigned char  remain_buf[MY_AES_BLOCK_SIZE * 2];

  // 4) If there's a remainder, MySQL decrypts the last 2 blocks first
  //    (remain_len != 0 => do a 2-block decrypt)
    if (remain_len != 0) {
    // MySQL logic: “remain_len = MY_AES_BLOCK_SIZE * 2;”
    // so effectively we always decrypt the last 2 blocks as a chunk.
        remain_len = MY_AES_BLOCK_SIZE * 2;

    // Copy last 2 blocks into remain_buf
    size_t offset_of_last_2 = data_len - remain_len; // from ptr
        memcpy(remain_buf, ptr + offset_of_last_2, remain_len);

    // my_aes_decrypt(src, src_len, dest, key, key_len, mode, iv, pad)
    // using mode = my_aes_256_cbc, pad=false
    // Decrypt the 2-block chunk into tmp_buf + offset_of_last_2
        int elen = my_aes_decrypt(
            remain_buf,
        static_cast<uint32>(remain_len),
            tmp_buf + offset_of_last_2,
            key,
        static_cast<uint32>(key_len),
        my_aes_256_cbc,
            iv,
            false /* no padding */);

        if (elen == MY_AES_BAD_DATA) {
            delete[] tmp_buf;
            return false;
        }

    // Copy everything *before* those last 2 blocks into tmp_buf unchanged
        memcpy(tmp_buf, ptr, offset_of_last_2);
    } else {
    // If data_len is a multiple of 16, MySQL just copies it all to tmp_buf
        memcpy(tmp_buf, ptr, data_len);
    }

  // 5) Decrypt the “main” portion from tmp_buf => ptr
  //    (which is the portion except those last 2 blocks, if any)
    {
        int elen = my_aes_decrypt(
            tmp_buf,
        static_cast<uint32>(main_len), // only main_len portion
        ptr,                           // output in-place in the page
            key,
        static_cast<uint32>(key_len),
        my_aes_256_cbc,
            iv,
            false /* no padding */);

        if (elen == MY_AES_BAD_DATA) {
            delete[] tmp_buf;
            return false;
        }
    }

  // 6) If remain_len != 0, copy the decrypted tail from tmp_buf to ptr
  //    (beyond main_len)
    if (data_len > main_len) {
        memcpy(ptr + main_len, tmp_buf + main_len, data_len - main_len);
    }

    // 7) Restore the original page type
    //    For compressed+encrypted, reset to FIL_PAGE_COMPRESSED, etc.
    if (page_type == FIL_PAGE_ENCRYPTED) {
        mach_write_to_2(page_data + FIL_PAGE_TYPE, original_type);
        mach_write_to_2(page_data + FIL_PAGE_ORIGINAL_TYPE_V1, 0);
    } else if (page_type == FIL_PAGE_ENCRYPTED_RTREE) {
        mach_write_to_2(page_data + FIL_PAGE_TYPE, FIL_PAGE_RTREE);
    } else if (page_type == FIL_PAGE_COMPRESSED_AND_ENCRYPTED) {
        mach_write_to_2(page_data + FIL_PAGE_TYPE, FIL_PAGE_COMPRESSED);
        // We might also want to clear FIL_PAGE_ORIGINAL_TYPE_V1, etc.
    }

  // Cleanup
    delete[] tmp_buf;
    return true;
}

// ----------------------------------------------------------------
// decrypt_ibd_file(): read the entire .ibd, decrypt, write out
// ----------------------------------------------------------------
bool decrypt_ibd_file(const char* src_ibd_path,
                      const char* dst_path,
                      const Tablespace_key_iv& ts_key_iv)
{
  // 1) Open source file
  FILE* f_in = std::fopen(src_ibd_path, "rb");
  if (!f_in) {
    std::cerr << "Cannot open source .ibd for reading: " << src_ibd_path << "\n";
    return false;
  }

  // 2) Open destination file
  FILE* f_out = std::fopen(dst_path, "wb");
  if (!f_out) {
    std::cerr << "Cannot open destination file for writing: " << dst_path << "\n";
    std::fclose(f_in);
    return false;
  }

  // 3) Read page by page, decrypt, write
  unsigned char page_buf[PAGE_SIZE];

  uint64_t page_number = 0;
  while (true) {
    size_t read_bytes = fread(page_buf, 1, PAGE_SIZE, f_in);
    if (read_bytes == 0) {
      // Probably end of file
      break;
    }
    if (read_bytes < PAGE_SIZE) {
      // If we got partial page, that's up to your logic whether to handle it
      std::cerr << "Warning: partial page read! offset=" << (page_number * PAGE_SIZE)
                << " read=" << read_bytes << "\n";
      // We'll just decrypt the partial chunk for demonstration, though
      // that might fail if your encryption logic expects a full block.
    }

    // Decrypt in place
    if (!decrypt_page_inplace(page_buf, PAGE_SIZE,
                              ts_key_iv.key, 32,
                              ts_key_iv.iv, 8*1024)) {
      std::cerr << "Failed to decrypt page #" << page_number << "\n";
      std::fclose(f_in);
      std::fclose(f_out);
      return false;
    }

    // Write the decrypted page
    size_t written = fwrite(page_buf, 1, PAGE_SIZE, f_out);
    if (written < PAGE_SIZE) {
      std::cerr << "Failed to write page #" << page_number << "\n";
      std::fclose(f_in);
      std::fclose(f_out);
      return false;
    }

    page_number++;
  }

  std::fclose(f_in);
  std::fclose(f_out);

  std::cout << "Successfully decrypted .ibd -> " << dst_path << "\n";
  return true;
}