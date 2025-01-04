// File: offline_decrypt_tool.cc

// 1) Provide stubs for LogPluginErr / LogPluginErrV first:
#define LogPluginErr(level, errcode, message) do {} while (0)
#define LogPluginErrV(level, errcode, vl) do {} while (0)

// 2) Now include the keyring code (which indirectly includes logger.h)
#include "plugin/keyring/common/keyring.h"
#include "plugin/keyring/buffered_file_io.h"

// 3) The rest of your includes
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

// Then your custom AES / other definitions, etc.
#include "decrypt_aes.h"

// (Include your custom AES code: my_custom_aes_decrypt)
bool my_custom_aes_decrypt(const unsigned char *in, int in_len,
                           unsigned char *out,
                           const unsigned char *key, int key_len,
                           const unsigned char *iv);

// Some constants for InnoDB page offsets
static const size_t FIL_PAGE_DATA = 38; // or 56 if FIL_PAGE_VERSION_2
static const size_t PAGE_SIZE     = 16384; // example

// Fake function to read the "encryption info" from .ibd
// Normally you'd parse the .ibd header
bool read_ibd_encryption_info(const char *ibd_file_path,
                              unsigned char *encrypted_info_buffer)
{
  // For a real tool, read from the .ibd's header area
  // Enough to store the 64 bytes or so that contain the
  // Master Key ID, server_uuid, encrypted key, iv, etc.

  FILE *f = fopen(ibd_file_path, "rb");
  if (!f) {
    std::cerr << "Cannot open .ibd file" << std::endl;
    return false;
  }

  // Suppose the encryption info is at offset 0 or  page 0 + ...
  // This depends on MySQL version. We'll just do an example:
  fseek(f, 0, SEEK_SET);
  // read 96 or so bytes
  size_t n = fread(encrypted_info_buffer, 1, 96, f);
  fclose(f);

  if (n < 96) {
    std::cerr << "Failed to read encryption info from .ibd\n";
    return false;
  }

  return true;
}

// Parse out the master key ID & retrieve the key from keyring
bool get_master_key_from_info(const unsigned char *info,
                              std::string &master_key_id_str,
                              std::vector<unsigned char> &master_key)
{

  (void)info; // Avoid -Wunused-parameter

  // This is just an example. The real MySQL code looks for the magic bytes
  // ("ENCR", "MKEY", etc.), reads 4 bytes for master_key_id, reads the
  // server uuid, etc. We'll just pretend there's an ASCII master_key_id.

  // Suppose bytes [4..7] is the numeric master key ID, or we have to parse
  // a string. This is custom logic.

  // For demonstration, let's just say the ID is 1:
  master_key_id_str = "1";

  // Now let's fetch from the keyring plugin. Something like:
  // But since keyring uses the Key/Keys_container approach,
  // we first must have loaded the keyring file.

  // We'll skip the real plugin_keyring API calls here, but show how you'd do it
  // if you store the "master.1" key in the keyring for instance:

  // master_key = retrieve_key_from_keys_container("master.1");
  // where retrieve_key_from_keys_container is your custom function
  // that finds the Key object by key_id = "master.1" in the container
  // and returns the raw key bytes.

  // Hard-code or mock:
  master_key.resize(32);
  memset(master_key.data(), 0x11, 32); // just a dummy filler

  return true;
}

/**
 * Decrypt the tablespace key + IV from the encryption info
 * using the (plaintext) master key from above
 */
bool decrypt_tablespace_key(const unsigned char *info,
                            const std::vector<unsigned char> &master_key,
                            unsigned char *tablespace_key, // out
                            unsigned char *tablespace_iv)   // out
{
  // In real MySQL, the chunk that has the tablespace key+IV is typically
  // encrypted with AES-256-ECB (not CBC) using the master key.
  // So you'd either replicate that or just do what MySQL does:
  //    my_aes_decrypt(..., my_aes_256_ecb, master_key, ...)

  // For illustration, let's do an ECB decrypt with OpenSSL:

  // Let's say the chunk is 64 bytes at offset 32 in `info`:
  const unsigned char *encrypted_key_iv = info + 32;
  const int ENCRYPTED_LEN = 64; // 32 bytes key + 32 bytes IV, encrypted

  unsigned char outbuf[64];
  memset(outbuf, 0, sizeof(outbuf));

  // We'll do a separate function for ECB:
  if (!my_custom_aes_decrypt_ecb(encrypted_key_iv, ENCRYPTED_LEN,
                                 outbuf, master_key.data(), 32)) {
    std::cerr << "Failed to decrypt tablespace key/IV with master key (ECB)\n";
    return false;
  }

  // Then split the output into 32 bytes key, 32 bytes IV
  memcpy(tablespace_key, outbuf,    32);
  memcpy(tablespace_iv,  outbuf+32, 32);

  return true;
}

// Minimal example of an ECB decryption for the tablespace key
// A simplified version based on OpenSSL:
bool my_custom_aes_decrypt_ecb(const unsigned char *in, int in_len,
                               unsigned char *out,
                               const unsigned char *key, int key_len)
{
  if (key_len != 32) return false; // for AES-256
  const EVP_CIPHER *cipher = EVP_aes_256_ecb();

  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;

  bool success = true;
  int out_len1 = 0, out_len2 = 0;
  do {
    if (EVP_DecryptInit_ex(ctx, cipher, NULL, key, NULL) != 1) {
      success = false; break;
    }
    // No IV for ECB, so pass null
    if (EVP_CIPHER_CTX_set_padding(ctx, 0) != 1) {
      success = false; break;
    }

    if (EVP_DecryptUpdate(ctx, out, &out_len1, in, in_len) != 1) {
      success = false; break;
    }
    if (EVP_DecryptFinal_ex(ctx, out + out_len1, &out_len2) != 1) {
      success = false; break;
    }
  } while (0);

  EVP_CIPHER_CTX_free(ctx);
  if (!success) return false;

  return true;
}

// Now let's do the page decryption with AES-256-CBC, like InnoDB does
bool decrypt_innodb_page(unsigned char *page_data,
                         size_t page_data_len,
                         const unsigned char *tablespace_key,
                         const unsigned char *tablespace_iv)
{
  // InnoDB encrypts data from offset FIL_PAGE_DATA to the end (or to the
  // compressed length). For a 16K page, that's from 38..16384, etc.

  if (page_data_len < FIL_PAGE_DATA) {
    // malformed
    return false;
  }

  unsigned char *cipher_area = page_data + FIL_PAGE_DATA;
  int cipher_len = (int)(page_data_len - FIL_PAGE_DATA);

  // InnoDB has some nuances about the "last 2 blocks" if not aligned, etc.
  // For a simple example, we just do a single AES-256-CBC decrypt pass:
  // (Production code mimics exactly what os0enc.cc does with the final blocks.)

  // We'll store the plaintext in-place (CBC can do in-place if careful).
  if (!my_custom_aes_decrypt(cipher_area, cipher_len, 
                             cipher_area,
                             tablespace_key, 32,
                             tablespace_iv)) {
    std::cerr << "Failed to decrypt page data.\n";
    return false;
  }

  // Also fix up the FIL_PAGE_TYPE, etc. if needed
  // In real code: if the page was FIL_PAGE_ENCRYPTED, you read the
  // original type from FIL_PAGE_ORIGINAL_TYPE_V1, etc.

  return true;
}

/************************************************************
 *  The main() function: tie it all together.
 ************************************************************/

int main(int argc, char **argv)
{
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0]
              << " <keyring_file> <ibd_file>\n";
    return 1;
  }

  const char *keyring_path = argv[1];
  const char *ibd_path     = argv[2];

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
  // The plugin code uses a pointer to a file IO object
  Buffered_file_io *keyring_io = new Buffered_file_io(logger.get(),
                                                      &allowedFileVersions);

  // keys->init(...) will parse the keyring file
  if (keys->init(keyring_io, keyring_path)) {
    std::cerr << "Failed to load keyring from " << keyring_path << "\n";
    return 1;
  }

  std::cout << "Loaded keyring from: " << keyring_path << "\n";

  // 3) Read the "encryption info" from the .ibd
  unsigned char enc_info[128];
  memset(enc_info, 0, sizeof(enc_info));
  if (!read_ibd_encryption_info(ibd_path, enc_info)) {
    std::cerr << "Failed to read encryption info.\n";
    return 1;
  }

  // 4) Parse out the master_key_id + fetch from keys container
  std::string master_key_id_str;
  std::vector<unsigned char> master_key;
  if (!get_master_key_from_info(enc_info, master_key_id_str, master_key)) {
    std::cerr << "Failed to parse master key id or load from keyring.\n";
    return 1;
  }

  // In real code, you'd do something like:
  //   auto masterKeyObj = keys->get_key_by_id(master_key_id_str, ...);
  //   if (!masterKeyObj) { ... }
  //   master_key.assign(masterKeyObj->get_key(), masterKeyObj->get_key() + masterKeyObj->get_key_length());

  std::cout << "Got master key id=" << master_key_id_str
            << " size=" << master_key.size() << "\n";

  // 5) Decrypt the tablespace key + IV from the .ibd encryption info
  unsigned char tbl_key[32], tbl_iv[32];
  memset(tbl_key, 0, 32);
  memset(tbl_iv,  0, 32);
  if (!decrypt_tablespace_key(enc_info, master_key, tbl_key, tbl_iv)) {
    std::cerr << "Failed to decrypt the tablespace key.\n";
    return 1;
  }

  std::cout << "Decrypted tablespace key & IV.\n";

  // 6) Now let's decrypt an *actual page* from the .ibd offline as a demo
  // We'll just read page 0 for example
  unsigned char page_buf[PAGE_SIZE];
  memset(page_buf, 0, PAGE_SIZE);

  FILE *f = fopen(ibd_path, "rb");
  if (!f) {
    std::cerr << "Failed to open .ibd again.\n";
    return 1;
  }
  size_t read_bytes = fread(page_buf, 1, PAGE_SIZE, f);
  fclose(f);

  if (read_bytes < PAGE_SIZE) {
    std::cerr << "Could not read a full page.\n";
    return 1;
  }

  // decrypt the page
  if (!decrypt_innodb_page(page_buf, PAGE_SIZE, tbl_key, tbl_iv)) {
    std::cerr << "Page decryption failed.\n";
    return 1;
  }

  // If success, 'page_buf' now holds plaintext for page 0
  std::cout << "Successfully decrypted page 0 offline!\n";

  // ... at this point, you can parse the page data or dump it

  // Global MySQL thread finalization
  my_thread_end();
  my_end(0);
  return 0;
}
