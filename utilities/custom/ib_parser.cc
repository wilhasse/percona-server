/*****************************************************************************
  ib_parser.cc

  Demonstrates how to:
    - parse a "mode" (1=decrypt, 2=decompress, 3=both),
    - run the appropriate logic for either decrypt, decompress, or both.

  This merges the old "main" from decrypt.cc and decompress.cc into
  one single program.
*****************************************************************************/

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>
#include <memory>
#include <stdexcept>

// MySQL/Percona, OpenSSL, etc.
#include <my_sys.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <mysql/plugin.h>
#include <my_sys.h>
#include <my_thread.h>
#include <m_string.h>
#include <mysys_err.h>
#include <mysqld_error.h>
#include <fcntl.h>  // For O_RDONLY, O_WRONLY, O_CREAT, O_TRUNC

// Your headers that declare "decrypt_page_inplace()", "decompress_page_inplace()",
// "get_master_key()", "read_tablespace_key_iv()", etc.
#include "decrypt.h"     // Contains e.g. decrypt_page_inplace(), get_master_key() ...
#include "decompress.h"  // Contains e.g. decompress_page_inplace(), etc.

// Some defines or constants if needed
// (like PAGE_SIZE, or command-line usage messages, etc.)
static const size_t PAGE_SIZE = 16384;

/** 
 * Minimal usage print 
 */
static void usage() {
  std::cerr << "Usage:\n"
            << "  ib_parser <mode> [decrypt/decompress args...]\n\n"
            << "Where <mode> is:\n"
            << "  1 = Decrypt only\n"
            << "  2 = Decompress only\n"
            << "  3 = Decrypt then Decompress in a single pass\n\n"
            << "Examples:\n"
            << "  ib_parser 1 <master_key_id> <server_uuid> <keyring_file> <ibd_path> <dest_path>\n"
            << "  ib_parser 2 <in_file.ibd> <out_file>\n"
            << "  ib_parser 3 <master_key_id> <server_uuid> <keyring_file> <ibd_path> <dest_path>\n"
            << std::endl;
}

/**
 * (A) The "decrypt only" routine, adapted from your old decrypt main().
 *     We assume you have a function: 
 *         bool decrypt_ibd_file(const char* src, const char* dst, 
 *                               const Tablespace_key_iv &ts_key_iv);
 */
static int do_decrypt_main(int argc, char** argv)
{
  if (argc < 6) {
    std::cerr << "Usage for mode=1 (decrypt):\n"
              << "  ib_parser 1 <master_key_id> <server_uuid> <keyring_file> <ibd_path> <dest_path>\n";
    return 1;
  }

  uint32_t master_id       = static_cast<uint32_t>(std::atoi(argv[1]));
  std::string srv_uuid     = argv[2];
  const char* keyring_path = argv[3];
  const char* ibd_path     = argv[4];
  const char* dest_path    = argv[5];

  // 1) Global MySQL init
  my_init();
  my_thread_init();
  OpenSSL_add_all_algorithms();

  // 2) get the master key
  std::vector<unsigned char> master_key;
  if (!get_master_key(master_id, srv_uuid, keyring_path, master_key)) {
    std::cerr << "Could not get master key\n";
    return 1;
  }

  // 3) read the tablespace key/IV
  //    pick offset=10390 or 5270 or whichever is appropriate
  long offset = 10390; 
  Tablespace_key_iv ts_key_iv;
  if (!read_tablespace_key_iv(ibd_path, offset, master_key, ts_key_iv)) {
    std::cerr << "Could not read tablespace key\n";
    return 1;
  }

  // 4) Decrypt the entire .ibd
  if (!decrypt_ibd_file(ibd_path, dest_path, ts_key_iv)) {
    std::cerr << "Decrypt failed.\n";
    return 1;
  }

  my_thread_end();
  my_end(0);
  return 0;  // success
}

/**
 * (B) The "decompress only" routine, adapted from your old decompress main().
 *     We assume you have: bool decompress_ibd(File in_fd, File out_fd);
 */
static int do_decompress_main(int argc, char** argv)
{
  if (argc < 3) {
    std::cerr << "Usage for mode=2 (decompress):\n"
              << "  ib_parser 2 <in_file> <out_file>\n";
    return 1;
  }

  MY_INIT(argv[0]);
  DBUG_TRACE;
  DBUG_PROCESS(argv[0]);

  const char* in_file  = argv[1];
  const char* out_file = argv[2];

  // open input
  File in_fd = my_open(in_file, O_RDONLY, MYF(0));
  if (in_fd < 0) {
    fprintf(stderr, "Cannot open input '%s'.\n", in_file);
    return 1;
  }
  // open output
  File out_fd = my_open(out_file, O_CREAT | O_WRONLY | O_TRUNC, MYF(0));
  if (out_fd < 0) {
    fprintf(stderr, "Cannot open/create output '%s'.\n", out_file);
    my_close(in_fd, MYF(0));
    return 1;
  }

  bool ok = decompress_ibd(in_fd, out_fd); 
  my_close(in_fd, MYF(0));
  my_close(out_fd, MYF(0));
  return ok ? 0 : 1;
}

/**
 * (C) The "decrypt + decompress" combined logic in a single pass,
 *     adapted from what we did in "combined_decrypt_decompress.cc".
 *     Page by page => decrypt_page_inplace => decompress_page_inplace => write final.
 */
static int do_decrypt_then_decompress_main(int argc, char** argv)
{
  if (argc < 6) {
    std::cerr << "Usage for mode=3 (decrypt+decompress):\n"
              << "  ib_parser 3 <master_key_id> <server_uuid> <keyring_file> <ibd_path> <dest_path>\n";
    return 1;
  }

  uint32_t master_id       = static_cast<uint32_t>(std::atoi(argv[1]));
  std::string srv_uuid     = argv[2];
  const char* keyring_path = argv[3];
  const char* ibd_path     = argv[4];
  const char* out_file     = argv[5];

  // (A) get master key
  my_init();
  my_thread_init();
  OpenSSL_add_all_algorithms();

  std::vector<unsigned char> master_key;
  if (!get_master_key(master_id, srv_uuid, keyring_path, master_key)) {
    std::cerr << "Could not get master key\n";
    return 1;
  }

  // (B) read tablespace key/IV
  long offset = 10390;
  Tablespace_key_iv ts_key_iv;
  if (!read_tablespace_key_iv(ibd_path, offset, master_key, ts_key_iv)) {
    std::cerr << "Could not read tablespace key\n";
    return 1;
  }

  // (C) open input
  FILE* fin = std::fopen(ibd_path, "rb");
  if (!fin) {
    std::cerr << "Cannot open input .ibd for reading.\n";
    return 1;
  }
  FILE* fout = std::fopen(out_file, "wb");
  if (!fout) {
    std::cerr << "Cannot open output for writing.\n";
    std::fclose(fin);
    return 1;
  }

  // temp buffers
  unsigned char page_buf[PAGE_SIZE];
  unsigned char final_buf[PAGE_SIZE];
  uint64_t page_number = 0;

  while (true) {
    size_t rd = std::fread(page_buf, 1, PAGE_SIZE, fin);
    if (rd == 0) {
      // EOF
      break;
    }
    if (rd < PAGE_SIZE) {
      std::cerr << "Warning: partial page read at page " << page_number << "\n";
    }

    // 1) decrypt in place
    bool dec_ok = decrypt_page_inplace(
        page_buf, PAGE_SIZE,
        ts_key_iv.key, 32,
        ts_key_iv.iv, 8 * 1024);
    if (!dec_ok) {
      std::cerr << "Decrypt failed on page " << page_number << "\n";
      std::fclose(fin);
      std::fclose(fout);
      return 1;
    }

    // 2) decompress in place
    size_t physical_size = (rd < PAGE_SIZE) ? rd : PAGE_SIZE;
    bool page_is_compressed = is_page_compressed(page_buf, physical_size, PAGE_SIZE);
    bool cmp_ok = decompress_page_inplace(
        page_buf,
        physical_size,
        page_is_compressed,
        final_buf,
        PAGE_SIZE,
        PAGE_SIZE
    );
    if (!cmp_ok) {
      std::cerr << "Decompress failed on page " << page_number << "\n";
      std::fclose(fin);
      std::fclose(fout);
      return 1;
    }

    // 3) write final
    size_t wr = std::fwrite(final_buf, 1, PAGE_SIZE, fout);
    if (wr < PAGE_SIZE) {
      std::cerr << "Failed to write final page " << page_number << "\n";
      std::fclose(fin);
      std::fclose(fout);
      return 1;
    }

    page_number++;
  }

  std::fclose(fin);
  std::fclose(fout);

  std::cout << "Decrypt+Decompress done. " << page_number << " pages written.\n";
  my_thread_end();
  my_end(0);
  return 0;
}

/**
 * The single main() that decides which path to use based on "mode".
 */
int main(int argc, char** argv)
{
  if (argc < 2) {
    usage();
    return 1;
  }

  // parse "mode" from argv[1]
  int mode = std::atoi(argv[1]);
  switch (mode) {
  case 1:  // decrypt only
    return do_decrypt_main(argc - 1, &argv[1]);
  case 2:  // decompress only
    return do_decompress_main(argc - 1, &argv[1]);
  case 3:  // decrypt + decompress
    return do_decrypt_then_decompress_main(argc - 1, &argv[1]);
  default:
    std::cerr << "Error: invalid mode '" << mode << "'\n";
    usage();
    return 1;
  }
}
