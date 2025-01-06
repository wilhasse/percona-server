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
#include <my_sys.h>
#include <my_thread.h>
#include "my_aes.h"

// Custom
#include "ibd_enc_reader.h"

// Decrypts an obfuscated key using MySQL's keyring XOR method
void keyring_deobfuscate(unsigned char* key_data, size_t key_len) {
    const char* obfuscate_str = "*305=Ljt0*!@$Hnm(*-9-w;:";
    const size_t obfuscate_len = strlen(obfuscate_str);
    
    for (size_t i = 0, l = 0; i < key_len; i++, l = ((l + 1) % obfuscate_len)) {
        key_data[i] ^= obfuscate_str[l];
    }
}

// This is the simplified page size for InnoDB pages
// Typically it's 16KB, but adapt if your system differs
static const size_t PAGE_SIZE = 16384;

// Decrypt based on ...
#include <cstdio>
#include <cstring>
#include <openssl/evp.h>

#include <cstring>         // for memcpy()
#include <cstdint>         // for uint16_t, etc.
#include "my_aes.h"        // or wherever my_aes_decrypt() is declared
#include "mysys_err.h"     // for MY_AES_BAD_DATA constant, etc.

/* Some MySQL/Innodb constants you’ll need: */
static const size_t FIL_PAGE_DATA     = 38;   // Typical offset for raw page data
static const size_t FIL_PAGE_TYPE     = 24;   // Offset of 2-byte page type
static const size_t FIL_PAGE_ORIGINAL_TYPE_V1  = 26; // Where original type is stored
static const uint16_t FIL_PAGE_ENCRYPTED                = 15;
static const uint16_t FIL_PAGE_COMPRESSED_AND_ENCRYPTED = 16;
static const uint16_t FIL_PAGE_ENCRYPTED_RTREE          = 17;
static const uint16_t FIL_PAGE_RTREE           = 0x000B; // normal RTREE page type
// ^ In real code, confirm these offsets/types match your MySQL version!

/* Helper to read/write big-endian 2-byte fields (mach read/write). */
inline uint16_t mach_read_from_2(const unsigned char* ptr) {
  return (ptr[0] << 8) | ptr[1];
}
inline void mach_write_to_2(unsigned char* ptr, uint16_t val) {
  ptr[0] = static_cast<unsigned char>((val >> 8) & 0xFF);
  ptr[1] = static_cast<unsigned char>(val & 0xFF);
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

/**
 * @brief Offline function that decrypts a single uncompressed page in-place
 *        using MySQL's partial-block approach, calling `my_aes_decrypt()`.
 *
 * @param page_data Pointer to the full page buffer.
 * @param page_len  Size of the full page (e.g. 16 KB).
 * @param key       32-byte AES key (AES-256) as used by MySQL (m_key).
 * @param key_len   e.g. 32 (for AES-256).
 * @param iv        32-byte IV from the tablespace header (m_iv).
 * @return true if successful, false if decryption failed.
 */
bool decrypt_page_uncompressed(
    unsigned char*  page_data,
    size_t          page_len,
    const unsigned char* key,
    size_t          key_len,
    const unsigned char* iv)
{
  // 1) Check if it’s even encrypted and uncompressed
  if (!is_encrypted_page(page_data)) {
    // Not encrypted, or maybe it’s compressed => skip
    return true;
  }

  // 2) Read the page type and original type
  const uint16_t page_type = mach_read_from_2(page_data + FIL_PAGE_TYPE);
  uint16_t original_type    = mach_read_from_2(page_data + FIL_PAGE_ORIGINAL_TYPE_V1);

  // 3) The actual data portion to decrypt is everything after FIL_PAGE_DATA
  //    (i.e., skip the InnoDB file header).
  if (page_len <= FIL_PAGE_DATA) {
    // Malformed / no space to decrypt
    return false;
  }

  // For uncompressed pages, the “encrypted length” is simply:
  size_t src_len  = page_len;           // we consider the entire page
  unsigned char* ptr = page_data + FIL_PAGE_DATA;
  size_t data_len = src_len - FIL_PAGE_DATA;

  // MySQL aligns “main_len” to a multiple of AES block size
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
  if (page_type == FIL_PAGE_ENCRYPTED) {
    // Typically, put the original type back in FIL_PAGE_TYPE
    mach_write_to_2(page_data + FIL_PAGE_TYPE, original_type);
    // Clear out the original type field
    mach_write_to_2(page_data + FIL_PAGE_ORIGINAL_TYPE_V1, 0);
  } else if (page_type == FIL_PAGE_ENCRYPTED_RTREE) {
    // MySQL sets it back to FIL_PAGE_RTREE (0x000B)
    mach_write_to_2(page_data + FIL_PAGE_TYPE, FIL_PAGE_RTREE);
  }

  // Cleanup
  delete[] tmp_buf;
  return true;
}

/************************************************************
 *  decrypt_ibd_file: read the entire .ibd, decrypt, write out.
 ************************************************************/
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
    if (!decrypt_page_uncompressed(page_buf, PAGE_SIZE, ts_key_iv.key, 32, ts_key_iv.iv)) {
      std::cerr << "Failed to decrypt page #" << page_number << "\n";
      std::fclose(f_in);
      std::fclose(f_out);
      return false;
    }

    // Write the decrypted page
    size_t written = fwrite(page_buf, 1, PAGE_SIZE, f_out);
    if (written < PAGE_SIZE) {
      std::cerr << "Failed to write page #" << page_number
                << " to destination file.\n";
      std::fclose(f_in);
      std::fclose(f_out);
      return false;
    }

    page_number++;
  }

  std::fclose(f_in);
  std::fclose(f_out);

  std::cout << "Successfully decrypted the entire .ibd file to: " << dst_path << "\n";
  return true;
}

/************************************************************
 *  The main() function: tie it all together.
 ************************************************************/
int main(int argc, char** argv) {

  if (argc < 6) {
    std::cerr << "Usage: " << argv[0]
              << " <master_key_id> <server_uuid> <keyring_file> <ibd_path> <dest_path>\n";
    return 1;
  }
  uint32_t master_id       = static_cast<uint32_t>(std::atoi(argv[1]));
  std::string srv_uuid     = argv[2];
  const char* keyring_path = argv[3];
  const char* ibd_path     = argv[4];
  const char* dest_path    = argv[5];

  // Global MySQL library init
  my_init();

  // Per-thread init
  my_thread_init();    
  
  // 1) Initialize OpenSSL, etc. (some of this done in the plugin)
  OpenSSL_add_all_algorithms();

  // 2) Load the keyring file into a Keys_container
  using keyring::Buffered_file_io;
  using keyring::Keys_container;
  using keyring::Logger;

  std::unique_ptr<Logger> logger(new Logger());
  std::unique_ptr<Keys_container> keys(new Keys_container(logger.get()));

  std::vector<std::string> allowedFileVersions{
    keyring::keyring_file_version_2_0,
    keyring::keyring_file_version_1_0
  };
  auto* keyring_io = new Buffered_file_io(logger.get(), &allowedFileVersions);

  if (keys->init(keyring_io, keyring_path)) {
    std::cerr << "Failed to load keyring from " << keyring_path << "\n";
    return 1;
  }
  std::cout << "Loaded keyring from: " << keyring_path << "\n";

  // 3) Now make MyKeyringLookup
  MyKeyringLookup lookup(keys.get());

  // 4) Attempt to get the InnoDB master key
  std::vector<unsigned char> master_key;
  if (!lookup.get_innodb_master_key(srv_uuid, master_id, master_key)) {
    std::cerr << "Could not find the master key in the container.\n";
    return 1;
  }

  // 5) Master key is obfuscated
  keyring_deobfuscate(master_key.data(), master_key.size());

  std::cout << "Got master key length=" << master_key.size() << "\n"
            << "master_key = ";
  for (size_t i = 0; i < master_key.size(); i++) {
    std::printf("%02X", master_key[i]);
  }
  std::cout << "\n";
  std::cout << std::endl;

  // 6) Read ~100 bytes from the start of the .ibd file
  FILE* f = std::fopen(ibd_path, "rb");
  if (!f) {
    std::cerr << "Cannot open .ibd\n";
    return 1;
  }

  // 7) <-- Key change: 
  // Compressed Page: Seek to offset 5270 (0x1496)
  // Uncompressed Page: Offset 10390 (0x2896)
  long offset = 10390; 
  if (std::fseek(f, offset, SEEK_SET) != 0) {
    std::cerr << "Failed to fseek() to offset " << offset << " in .ibd file.\n";
    std::fclose(f);
    return 1;
  }

  // 8) Read key and other info
  unsigned char enc_info[128];
  std::memset(enc_info, 0, sizeof(enc_info));
  size_t n = std::fread(enc_info, 1, sizeof(enc_info), f);
  std::fclose(f);

  if (n < 80) {
    std::cerr << "Not enough data read for encryption info\n";
    return 1;
  }

  // 9) decode the 80-100 bytes, using the master_key we already have
  Tablespace_key_iv ts_key_iv;
  if (!decode_ibd_encryption_info(enc_info, /* decrypt_key */true,
                                  master_key, // pass in the raw 32 bytes
                                  ts_key_iv))
  {
    std::cerr << "Failed to decode ibd encryption header.\n";
    return 1;
  }

  // OK
  std::cout << "Successfully read encryption info!\n";

  // 10) Now decrypt the entire .ibd into the destination file
  if (!decrypt_ibd_file(ibd_path, dest_path, ts_key_iv)) {
    std::cerr << "Failed to fully decrypt the .ibd file.\n";
    return 1;
  }

  // Global MySQL thread finalization
  my_thread_end();
  my_end(0);
  return 0;
}