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

// InnoDB
#include "page0size.h"  // Add this for page_size_t
#include "fil0fil.h"      // for fsp_header_get_flags()
#include "fsp0fsp.h"      // for fsp_flags_is_valid(), etc.


// Your headers that declare "decrypt_page_inplace()", "decompress_page_inplace()",
// "get_master_key()", "read_tablespace_key_iv()", etc.
#include "decrypt.h"     // Contains e.g. decrypt_page_inplace(), get_master_key() ...
#include "decompress.h"  // Contains e.g. decompress_page_inplace(), etc.
#include "parser.h"      // Contains parser logic

// We assume a fixed 16K buffer for reading page 0, 
// since the maximum InnoDB page size is 16K in many setups.
// (If you allow 32K or 64K pages, you’ll need a bigger buffer.)
static bool is_table_compressed(File in_fd)
{
    // Step 1: Seek to 0
    if (my_seek(in_fd, 0, MY_SEEK_SET, MYF(0)) == MY_FILEPOS_ERROR) {
        fprintf(stderr, "Error: cannot seek to start.\n");
        return false; // default
    }

    // Step 2: Read up to 16 KB for page 0
    unsigned char page0[UNIV_PAGE_SIZE_ORIG]; 
    memset(page0, 0, sizeof(page0));

    size_t r = my_read(in_fd, page0, sizeof(page0), MYF(0));
    if (r < FIL_PAGE_DATA) {
        fprintf(stderr, "Error: cannot read page 0.\n");
        // For safety, return false => 'not compressed' as fallback
        return false;
    }

    // Step 3: Extract the fsp flags from page 0
    uint32_t flags = fsp_header_get_flags(page0);
    if (!fsp_flags_is_valid(flags)) {
        fprintf(stderr, "Error: fsp flags not valid on page 0.\n");
        return false;
    }

    // Depending on MySQL version, the "space is compressed" bit
    // is often tested by something like "FSP_FLAGS_GET_ZIP_SSIZE(flags) != 0"
    // or "fsp_flags_is_compressed(flags)" if you have that helper.
    // We'll demonstrate one possible check:
    ulint zip_ssize = FSP_FLAGS_GET_ZIP_SSIZE(flags);
    bool compressed = (zip_ssize != 0);

    return compressed;
}

/** 
 * Minimal usage print 
 */
static void usage() {
  std::cerr << "Usage:\n"
            << "  ib_parser <mode> [decrypt/decompress args...]\n\n"
            << "Where <mode> is:\n"
            << "  1 = Decrypt only\n"
            << "  2 = Decompress only\n"
            << "  3 = Parser only\n"
            << "  4 = Decrypt then Decompress in a single pass\n\n"
            << "Examples:\n"
            << "  ib_parser 1 <master_key_id> <server_uuid> <keyring_file> <ibd_path> <dest_path>\n"
            << "  ib_parser 2 <in_file.ibd> <out_file>\n"
            << "  ib_parser 3 <in_file.ibd> <table_def.json>\n"
            << "  ib_parser 4 <master_key_id> <server_uuid> <keyring_file> <ibd_path> <dest_path>\n"
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

  // 3) Open .ibd for reading so we can see if it's compressed
  File in_fd = my_open(ibd_path, O_RDONLY, MYF(0));
  if (in_fd < 0) {
    std::cerr << "Cannot open file " << ibd_path << std::endl;
    return 1;
  }
  bool compressed = is_table_compressed(in_fd);
  my_close(in_fd, MYF(0));

  // -----------------------------
  // 3a) read the tablespace key/IV
  // Compressed Page: Seek to offset 5270 (0x1496)
  // Uncompressed Page: Offset 10390 (0x2896)
  // -----------------------------
  long offset = compressed ? 5270 : 10390;
  Tablespace_key_iv ts_key_iv;
  if (!read_tablespace_key_iv(ibd_path, offset, master_key, ts_key_iv)) {
    std::cerr << "Could not read tablespace key\n";
    return 1;
  }

  // 4) Decrypt the entire .ibd
  if (!decrypt_ibd_file(ibd_path, dest_path, ts_key_iv, compressed)) {
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
 * (C) The "parse only" routine (unencrypted + uncompressed).
 *     Illustrative minimal example based on undrop-for-innodb code.
 */
static int do_parse_main(int argc, char** argv)
{
  if (argc < 2) {
    std::cerr << "Usage for mode=3 (parse-only):\n"
              << "  ib_parser 3 <in_file.ibd> <table_def.json>\n";
    return 1;
  }

  const char* in_file = argv[1];

  // 0) Load table definition
  load_ib2sdi_table_columns(argv[2]);

  // Build a table_def_t from g_columns
  static table_def_t my_table;
  if (build_table_def_from_json(&my_table, "HISTORICO") != 0) {
    std::cerr << "Failed to build table_def_t from JSON.\n";
    return 1;
  }

  // If your code uses the n_nullable field in ibrec_init_offsets_new():
  my_table.n_nullable = 0;
  for (int i = 0; i < my_table.fields_count; i++) {
    if (my_table.fields[i].can_be_null) my_table.n_nullable++;
  }
    // 1) MySQL init
  my_init();
  my_thread_init();

  // 2) *** DISCOVER PRIMARY INDEX ID *** using system "open + pread" approach
  {
    int sys_fd = ::open(in_file, O_RDONLY);
    if (sys_fd < 0) {
      perror("open");
      return 1;
    }
    if (discover_primary_index_id(sys_fd) != 0) {
      std::cerr << "Could not discover primary index from " << in_file << std::endl;
      ::close(sys_fd);
      my_thread_end();
      my_end(0);
      return 1;
    }
    ::close(sys_fd);
  }

  // 3) Now open the file with MySQL's my_open
  File in_fd = my_open(in_file, O_RDONLY, MYF(0));
  if (in_fd < 0) {
    std::cerr << "Cannot open file " << in_file << std::endl;
    my_thread_end();
    my_end(0);
    return 1;
  }

  // 4) Determine page size (for 8KB,16KB, etc.)
  page_size_t pg_sz(0, 0, false);
  if (!determine_page_size(in_fd, pg_sz)) {
    std::cerr << "Could not determine page size from " << in_file << "\n";
    my_close(in_fd, MYF(0));
    my_thread_end();
    my_end(0);
    return 1;
  }
  const size_t physical_page_size = pg_sz.physical();

  // 5) Rewind
  if (my_seek(in_fd, 0, MY_SEEK_SET, MYF(0)) == MY_FILEPOS_ERROR) {
    std::cerr << "Cannot seek to start of " << in_file << "\n";
    my_close(in_fd, MYF(0));
    my_thread_end();
    my_end(0);
    return 1;
  }

  // 6) Overwrite table_definitions[] with your new table and init table defs
  table_definitions[0] = my_table;
  table_definitions_cnt = 1;
  init_table_defs(1);

  // 6) Allocate a buffer
  std::unique_ptr<unsigned char[]> page_buf(new unsigned char[physical_page_size]);

  // 7) Page-by-page loop
  uint64_t page_no = 0;
  while (true) {
    size_t rd = my_read(in_fd, page_buf.get(), physical_page_size, MYF(0));
    if (rd == 0) {
      // EOF
      break;
    }
    if (rd < physical_page_size) {
      std::cerr << "Warning: partial page read at page " << page_no << "\n";
      break;
    }

    // Check if page's index ID == primary
    if (is_primary_index(page_buf.get())) {
      // Now also check if it's a LEAF page
      ulint page_level = mach_read_from_2(page_buf.get() + PAGE_HEADER + PAGE_LEVEL);
      if (page_level == 0) {
        // parse
        parse_records_on_page(page_buf.get(), physical_page_size, page_no);
      }
    }

    page_no++;
  }

  my_close(in_fd, MYF(0));
  my_thread_end();
  my_end(0);

  std::cout << "Parse-only complete. Pages read: " << page_no << "\n";
  return 0;
}

/**
 * (D) The "decrypt + decompress" combined logic in a single pass,
 *     adapted from what we did in "combined_decrypt_decompress.cc".
 *     Page by page => decrypt_page_inplace => decompress_page_inplace => write final.
 */
static int do_decrypt_then_decompress_main(int argc, char** argv)
{
  if (argc < 6) {
    std::cerr << "Usage for mode=4 (decrypt+decompress):\n"
              << "  ib_parser 4 <master_key_id> <server_uuid> <keyring_file> "
              << "<ibd_path> <dest_path>\n";
    return 1;
  }

  uint32_t master_id       = static_cast<uint32_t>(std::atoi(argv[1]));
  std::string srv_uuid     = argv[2];
  const char* keyring_path = argv[3];
  const char* ibd_path     = argv[4];
  const char* out_file     = argv[5];

  // -----------------------------
  // (A) Global MySQL / OpenSSL init
  // -----------------------------
  my_init();
  my_thread_init();
  OpenSSL_add_all_algorithms();

  // -----------------------------
  // (B) Retrieve the master key
  // -----------------------------
  std::vector<unsigned char> master_key;
  if (!get_master_key(master_id, srv_uuid, keyring_path, master_key)) {
    std::cerr << "Could not get master key\n";
    return 1;
  }

  // (C) Open .ibd for reading so we can see if it's compressed
  File in_fd = my_open(ibd_path, O_RDONLY, MYF(0));
  if (in_fd < 0) {
    std::cerr << "Cannot open file " << ibd_path << std::endl;
    return 1;
  }

  bool compressed = is_table_compressed(in_fd);
  my_close(in_fd, MYF(0));

  // -----------------------------
  // (C.1) Read the tablespace key/IV
  //     If you already know the file is "encrypted & compressed" => offset=5270.
  //     Or if you know it's "encrypted & uncompressed" => offset=10390.
  //  5270 (0x1496) => For "encrypted & compressed"  (8 KB physical, etc.)
  // 10390 (0x2896) => For "encrypted & uncompressed" (16 KB pages).
  // -----------------------------
  long offset = compressed ? 5270 : 10390;
  Tablespace_key_iv ts_key_iv;
  if (!read_tablespace_key_iv(ibd_path, offset, master_key, ts_key_iv)) {
    std::cerr << "Could not read tablespace key\n";
    return 1;
  }

  // -----------------------------
  // (D) Open input file with MySQL I/O or system I/O
  //     so we can run "determine_page_size()"
  // -----------------------------
  in_fd = my_open(ibd_path, O_RDONLY, MYF(0));
  if (in_fd < 0) {
    std::cerr << "Cannot open input file " << ibd_path << "\n";
    return 1;
  }

  // (D.1) Figure out the actual page size by reading the FSP header from page 0
  page_size_t pg_sz(0, 0, false);
  if (!determine_page_size(in_fd, pg_sz)) {
    std::cerr << "Could not determine page size from " << ibd_path << "\n";
    my_close(in_fd, MYF(0));
    return 1;
  }

  // We now know the physical page size => e.g. 8192 or 16384, etc.
  const size_t physical_page_size = pg_sz.physical();
  const size_t logical_page_size  = pg_sz.logical();

  // -----------------------------
  // (E) Reopen with FILE* APIs, or rewind using my_seek()
  //     For clarity, let's just close the File handle and use std::fopen.
  // -----------------------------
  my_close(in_fd, MYF(0)); // close
  FILE* fin = std::fopen(ibd_path, "rb");
  if (!fin) {
    std::cerr << "Cannot reopen input " << ibd_path << "\n";
    return 1;
  }
  FILE* fout = std::fopen(out_file, "wb");
  if (!fout) {
    std::cerr << "Cannot open output " << out_file << "\n";
    std::fclose(fin);
    return 1;
  }

  // -----------------------------
  // (F) Allocate buffers based on actual page size
  // -----------------------------
  std::unique_ptr<unsigned char[]> page_buf(new unsigned char[physical_page_size]);
  std::unique_ptr<unsigned char[]> final_buf(new unsigned char[logical_page_size]);

  // -----------------------------
  // (G) Page-by-page loop
  // -----------------------------
  uint64_t page_number = 0;
  while (true) {
    // Read exactly 'physical_page_size' from the file
    size_t rd = std::fread(page_buf.get(), 1, physical_page_size, fin);
    if (rd == 0) {
      // EOF
      break;
    }
    if (rd < physical_page_size) {
      std::cerr << "Warning: partial page read at page " 
                << page_number << "\n";
      // We can either break or continue with partial data
      // But usually we break because it's incomplete
      break;
    }

    // 1) Decrypt in-place
    bool dec_ok = decrypt_page_inplace(
        page_buf.get(), 
        physical_page_size,   // or logical_page_size, depends on your encryption
        ts_key_iv.key, 
        32, 
        ts_key_iv.iv, 
        8 * 1024);
    if (!dec_ok) {
      std::cerr << "Decrypt failed on page " << page_number << "\n";
      std::fclose(fin);
      std::fclose(fout);
      return 1;
    }

    // 2) Decompress in-place (if needed)
    bool page_is_compressed =
        is_page_compressed(page_buf.get(), physical_page_size, logical_page_size);

    bool cmp_ok = decompress_page_inplace(
        page_buf.get(),          /* src data        */
        physical_page_size,      /* physical_size   */
        page_is_compressed,      /* is_compressed?  */
        final_buf.get(),         /* output buffer   */
        logical_page_size,       /* out_buf_len     */
        logical_page_size        /* "logical size"  */
    );
    if (!cmp_ok) {
      std::cerr << "Decompress failed on page " << page_number << "\n";
      std::fclose(fin);
      std::fclose(fout);
      return 1;
    }

    // 3) Write out the final uncompressed page
    size_t wr = std::fwrite(final_buf.get(), 1, logical_page_size, fout);
    if (wr < logical_page_size) {
      std::cerr << "Failed to write final page " << page_number << "\n";
      std::fclose(fin);
      std::fclose(fout);
      return 1;
    }

    page_number++;
  }

  std::fclose(fin);
  std::fclose(fout);

  std::cout << "Decrypt+Decompress done. " << page_number 
            << " pages written.\n";
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

  int mode = std::atoi(argv[1]);
  switch (mode) {
    case 1:  // decrypt only
      return do_decrypt_main(argc - 1, &argv[1]);
    case 2:  // decompress only
      return do_decompress_main(argc - 1, &argv[1]);
    case 3:  // parse-only
      return do_parse_main(argc - 1, &argv[1]);
    case 4:  // decrypt + decompress
      return do_decrypt_then_decompress_main(argc - 1, &argv[1]);
    default:
      std::cerr << "Error: invalid mode '" << mode << "'\n";
      usage();
      return 1;
  }
}
