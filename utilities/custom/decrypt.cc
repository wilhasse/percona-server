// File: offline_decrypt_tool.cc

// 1) Provide stubs for LogPluginErr / LogPluginErrV first:
#define LogPluginErr(level, errcode, message) do {} while (0)
#define LogPluginErrV(level, errcode, vl) do {} while (0)

// 2) Now include the keyring code (which indirectly includes logger.h)
#include "plugin/keyring/common/keyring.h"
#include "plugin/keyring/buffered_file_io.h"
#include "my_keyring_lookup.h"
#include "plugin/keyring/common/keys_container.h"

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
#include "my_aes.h"

// Then your custom AES / other definitions, etc.
#include "ibd_enc_reader.h"

// Some constants for InnoDB page offsets
static const size_t FIL_PAGE_DATA = 38; // or 56 if FIL_PAGE_VERSION_2
static const size_t PAGE_SIZE     = 16384; // example

#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>

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
 * using the (plaintext) master key from above.
 *
 * We now use MySQL's my_aes_decrypt(...) in AES-256-ECB mode,
 * with no padding, matching how InnoDB encrypts the chunk.
 */
bool decrypt_tablespace_key(const unsigned char *info,
                            const std::vector<unsigned char> &master_key,
                            unsigned char *tablespace_key, // out
                            unsigned char *tablespace_iv)   // out
{
  // In real InnoDB, the chunk that has the tablespace key + IV is
  // typically 64 bytes (32 bytes key + 32 bytes IV), encrypted with
  // AES-256-ECB using the master key. We'll assume it's located
  // starting at offset 32 within `info`, but adjust if your format differs.
  const unsigned char *encrypted_key_iv = info + 32;
  const int ENCRYPTED_LEN = 64; // 32 bytes key + 32 bytes IV, all encrypted

  // We'll decrypt into this temporary buffer:
  unsigned char outbuf[ENCRYPTED_LEN];
  std::memset(outbuf, 0, sizeof(outbuf));

  // Call MySQL's my_aes_decrypt with mode = my_aes_256_ecb, no IV, no padding
  int ret = my_aes_decrypt(
      /* source        = */ encrypted_key_iv,
      /* source_length = */ ENCRYPTED_LEN,
      /* dest          = */ outbuf,
      /* key           = */ master_key.data(),
      /* key_length    = */ static_cast<uint32>(master_key.size()),
      /* mode          = */ my_aes_256_ecb,   // AES-256-ECB
      /* iv            = */ nullptr,          // no IV in ECB
      /* padding       = */ false,            // InnoDB uses no padding
      /* kdf_options   = */ nullptr);         // not used for basic decrypt

  // Check for error
  if (ret == MY_AES_BAD_DATA) {
    std::cerr << "Failed to decrypt tablespace key/IV with master key (ECB). "
              << "my_aes_decrypt() returned MY_AES_BAD_DATA.\n";
    return false;
  }

  // The first 32 bytes of outbuf = the plaintext tablespace key
  // The next 32 bytes = the plaintext IV
  std::memcpy(tablespace_key, outbuf,     32);
  std::memcpy(tablespace_iv,  outbuf + 32, 32);

  std::cout << "Successfully decrypted tablespace key+IV using AES-256-ECB.\n";
  return true;
}

/************************************************************
 *  The main() function: tie it all together.
 ************************************************************/

int main(int argc, char** argv) {
  if (argc < 5) {
    std::cerr << "Usage: " << argv[0]
              << " <master_key_id> <server_uuid> <keyring_file> <ibd_path>\n";
    return 1;
  }
  uint32_t master_id       = static_cast<uint32_t>(std::atoi(argv[1]));
  std::string srv_uuid     = argv[2];
  const char* keyring_path = argv[3];
  const char* ibd_path     = argv[4];

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
  auto *keyring_io = new Buffered_file_io(logger.get(), &allowedFileVersions);

  if (keys->init(keyring_io, keyring_path)) {
    std::cerr << "Failed to load keyring from " << keyring_path << "\n";
    return 1;
  }
  std::cout << "Loaded keyring from: " << keyring_path << "\n";

  // 2) Now make MyKeyringLookup
  MyKeyringLookup lookup(keys.get());

  // 3) Attempt to get the InnoDB master key
  std::vector<unsigned char> master_key;
  if (!lookup.get_innodb_master_key(srv_uuid, master_id, master_key)) {
    std::cerr << "Could not find the master key in the container.\n";
    return 1;
  }

  std::cout << "Got master key length=" << master_key.size() << "\n";

  // 3) Read ~100 bytes from the start of the .ibd file
  FILE* f = std::fopen(ibd_path, "rb");
  if (!f) {
    std::cerr << "Cannot open .ibd\n";
    return 1;
  }

  // 4) <-- Key change: Seek to offset 5270 (0x1496 or 0x149D, whichever you confirmed)
  long offset = 5270; // or 0x1496, or 0x149D, depending on your exact find
  if (std::fseek(f, offset, SEEK_SET) != 0) {
    std::cerr << "Failed to fseek() to offset " << offset << " in .ibd file.\n";
    std::fclose(f);
    return 1;
  }

  // 5) Read key and other info
  unsigned char enc_info[128];
  std::memset(enc_info, 0, sizeof(enc_info));
  size_t n = std::fread(enc_info, 1, sizeof(enc_info), f);
  std::fclose(f);

  if (n < 80) {
    std::cerr << "Not enough data read for encryption info\n";
    return 1;
  }

  // 6) decode the 80-100 bytes, using the master_key we already have
  Tablespace_key_iv ts_key_iv;
  if (!decode_ibd_encryption_info(enc_info, /* decrypt_key */true,
                                  master_key, // pass in the raw 32 bytes
                                  ts_key_iv))
  {
    std::cerr << "Failed to decode ibd encryption header.\n";
    return 1;
  }

  // 7) print info
  std::cout << "Successfully read encryption info!\n"
            << "master_key = "
            << "Tablespace key = ";
  for (int i = 0; i < 32; i++) {
    std::printf("%02X", ts_key_iv.key[i]);
  }
  std::cout << "\nIV = ";
  for (int i = 0; i < 32; i++) {
    std::printf("%02X", ts_key_iv.iv[i]);
  }
  std::cout << std::endl;

  // 4) Parse out the master_key_id + fetch from keys container
  std::string master_key_id_str;
  if (!get_master_key_from_info(enc_info, master_key_id_str, master_key)) {
    std::cerr << "Failed to parse master key id or load from keyring.\n";
    return 1;
  }
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

  f = fopen(ibd_path, "rb");
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
  if (!decrypt_tablespace_key(enc_info, master_key, tbl_key, tbl_iv)) {
    std::cerr << "Failed to decrypt the tablespace key.\n";
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
