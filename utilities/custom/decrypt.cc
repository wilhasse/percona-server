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

// Some constants for InnoDB page offsets
static const size_t FIL_PAGE_DATA = 38; // or 56 if FIL_PAGE_VERSION_2
static const size_t PAGE_SIZE     = 16384; // example

// Decrypts an obfuscated key using MySQL's keyring XOR method
void keyring_deobfuscate(unsigned char* key_data, size_t key_len) {
    const char* obfuscate_str = "*305=Ljt0*!@$Hnm(*-9-w;:";
    const size_t obfuscate_len = strlen(obfuscate_str);
    
    for (size_t i = 0, l = 0; i < key_len; i++, l = ((l + 1) % obfuscate_len)) {
        key_data[i] ^= obfuscate_str[l];
    }
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
  std::cout << "Got master key length=" << master_key.size() << "\n";

  // 6) Read ~100 bytes from the start of the .ibd file
  FILE* f = std::fopen(ibd_path, "rb");
  if (!f) {
    std::cerr << "Cannot open .ibd\n";
    return 1;
  }

  // 7) <-- Key change: Seek to offset 5270 (0x1496)
  long offset = 5270; 
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

  // 10) print info
  std::cout << "Successfully read encryption info!\n"
            << "master_key = ";
  for (size_t i = 0; i < master_key.size(); i++) {
    std::printf("%02X", master_key[i]);
  }
  std::cout << "\nTablespace key = ";
  for (int i = 0; i < 32; i++) {
    std::printf("%02X", ts_key_iv.key[i]);
  }
  std::cout << "\nIV = ";
  for (int i = 0; i < 32; i++) {
    std::printf("%02X", ts_key_iv.iv[i]);
  }
  std::cout << std::endl;

  // 11) Now let's decrypt an *actual page* from the .ibd offline as a demo
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

  // If success, 'page_buf' now holds plaintext for page 0
  // std::cout << "Successfully decrypted page 0 offline!\n";

  // 10) ... at this point, you can parse the page data or dump it

  // Global MySQL thread finalization
  my_thread_end();
  my_end(0);
  return 0;
}

