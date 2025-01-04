/*****************************************************************************
  Minimal example that reads a possibly compressed .ibd (or ibdata*)
  and writes out an "uncompressed" copy of every page to a new output file.

  Includes STUBS for references like:
    - ib::logger, ib::warn, ib::error, ib::fatal

  So that linking won't fail. Real InnoDB logic is not performed.
*****************************************************************************/

#include "my_config.h"

// Standard headers
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <errno.h>
#include <string.h>
#include <limits>

// MySQL/Percona headers
#include "my_dbug.h"
#include "my_dir.h"
#include "my_getopt.h"
#include "my_io.h"
//#include "mysys.h"
#include "print_version.h"
#include "welcome_copyright_notice.h"

// InnoDB headers needed for decompress, page size, etc.
#include "fil0fil.h"
#include "fsp0fsp.h"
#include "mach0data.h"
#include "page0page.h"
#include "page0size.h"
#include "page0types.h"
#include "univ.i"
#include "ut0byte.h"
#include "ut0crc32.h"
//#include "page/zipdecompress.h" // Has page_zip_decompress_low()

/*
  In real InnoDB code, these are declared in e.g. "srv0srv.h" or "univ.i"
  but for our demo, we define them here:
*/
ulong srv_page_size       = 0;
ulong srv_page_size_shift = 0;
page_size_t univ_page_size(0, 0, false);

// Provide minimal stubs for the ib::logger family, so vtables are satisfied.
/** Error logging classes. */
namespace ib {

logger::~logger() = default;

info::~info() {
  std::cerr << "[INFO] ibd2sdi: " << m_oss.str() << "." << std::endl;
}

warn::~warn() {
  std::cerr << "[WARNING] ibd2sdi: " << m_oss.str() << "." << std::endl;
}

error::~error() {
  std::cerr << "[ERROR] ibd2sdi: " << m_oss.str() << "." << std::endl;
}

/*
MSVS complains: Warning C4722: destructor never returns, potential memory leak.
But, the whole point of using ib::fatal temporary object is to cause an abort.
*/
MY_COMPILER_DIAGNOSTIC_PUSH()
MY_COMPILER_MSVC_DIAGNOSTIC_IGNORE(4722)

fatal::~fatal() {
  std::cerr << "[FATAL] ibd2sdi: " << m_oss.str() << "." << std::endl;
  ut_error;
}

// Restore the MSVS checks for Warning C4722, silenced for ib::fatal::~fatal().
MY_COMPILER_DIAGNOSTIC_POP()

/* TODO: Improve Object creation & destruction on NDEBUG */
class dbug : public logger {
 public:
  ~dbug() override { DBUG_PRINT("ibd2sdi", ("%s", m_oss.str().c_str())); }
};
}  // namespace ib

/** Report a failed assertion.
@param[in]	expr	the failed assertion if not NULL
@param[in]	file	source file containing the assertion
@param[in]	line	line number of the assertion */
[[noreturn]] void ut_dbg_assertion_failed(const char *expr, const char *file,
                                          uint64_t line) {
  fprintf(stderr, "ibd2sdi: Assertion failure in file %s line " UINT64PF "\n",
          file, line);

  if (expr != nullptr) {
    fprintf(stderr, "ibd2sdi: Failing assertion: %s\n", expr);
  }

  fflush(stderr);
  fflush(stdout);
  abort();
}

// ----------------------------------------------------------------
// Minimal usage/option flags
// ----------------------------------------------------------------
static bool opt_version = false;
static bool opt_help    = false;

// For simplicity, we only define minimal options: e.g. --help, --version
static struct my_option decompress_opts[] = {
    {"help", 'h', "Display this help and exit.", &opt_help, &opt_help, 0,
     GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
    {"version", 'v', "Display version information and exit.",
     &opt_version, &opt_version, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
    {nullptr, 0, nullptr, nullptr, nullptr, nullptr, GET_NO_ARG, NO_ARG,
     0, 0, 0, nullptr, 0, nullptr}
};

// Minimal usage print
static void usage() {
#ifdef NDEBUG
  print_version();
#else
  print_version_debug();
#endif
  puts(ORACLE_WELCOME_COPYRIGHT_NOTICE("2024"));
  printf("Usage: decompress [options] <in_file> <out_file>\n\n");
}

// Callback for the standard MySQL option parser
extern "C" bool decompress_get_one_option(int optid, const struct my_option *,
                                          char *argument) {
  switch (optid) {
    case 'h':
      opt_help = true;
      break;
    case 'v':
      opt_version = true;
      break;
    default:
      break;
  }
  return false;
}

// ----------------------------------------------------------------
// A small block to read entire page or return SIZE_MAX on error
// ----------------------------------------------------------------
static bool seek_page(File file_in, const page_size_t &page_sz, page_no_t page_no) {
  my_off_t offset = page_no * page_sz.physical();
  if (my_seek(file_in, offset, MY_SEEK_SET, MYF(0)) == MY_FILEPOS_ERROR) {
    fprintf(stderr, "Error: my_seek failed for page %u. Errno=%d (%s)\n",
            page_no, errno, strerror(errno));
    return false;
  }
  return true;
}

// ----------------------------------------------------------------
// Minimal “determine page size” logic (like in ibd2sdi).
// Reads page 0, parse fsp header, etc.
// ----------------------------------------------------------------
static bool determine_page_size(File file_in, page_size_t &page_sz)
{
  // Temporarily read smallest page
  unsigned char buf[UNIV_ZIP_SIZE_MIN];
  memset(buf, 0, UNIV_ZIP_SIZE_MIN);

  // read the first 1KB from file
  if (my_seek(file_in, 0, MY_SEEK_SET, MYF(0)) == MY_FILEPOS_ERROR) {
    fprintf(stderr, "Error: cannot seek to start. %s\n", strerror(errno));
    return false;
  }
  size_t r = my_read(file_in, buf, UNIV_ZIP_SIZE_MIN, MYF(0));
  if (r != UNIV_ZIP_SIZE_MIN) {
    fprintf(stderr, "Cannot read first %u bytes from file.\n",
            UNIV_ZIP_SIZE_MIN);
    return false;
  }

  // parse fsp header
  uint32_t flags = fsp_header_get_flags(buf);
  bool valid = fsp_flags_is_valid(flags);
  if (!valid) {
    fprintf(stderr, "Page 0 is corrupted or invalid fsp flags\n");
    return false;
  }

  // set page size from flags
  ulint ssize = FSP_FLAGS_GET_PAGE_SSIZE(flags);
  if (ssize == 0) {
    srv_page_size = UNIV_PAGE_SIZE_ORIG; // 16k
  } else {
    // e.g. if ssize=4 => (UNIV_ZIP_SIZE_MIN >> 1) << 4 => 16k
    srv_page_size = ((UNIV_ZIP_SIZE_MIN >> 1) << ssize);
  }
  srv_page_size_shift = page_size_validate(srv_page_size);
  if (srv_page_size_shift == 0) {
    fprintf(stderr, "Detected invalid page size shift.\n");
    return false;
  }

  // store in univ_page_size
  univ_page_size.copy_from(page_size_t(srv_page_size, srv_page_size, false));

  // Actually set "page_sz"
  page_sz.copy_from(page_size_t(flags));

  // we also reset file pointer to 0
  my_seek(file_in, 0, MY_SEEK_SET, MYF(0));
  return true;
}

// ----------------------------------------------------------------
// The core “read and optionally decompress page” logic
// (similar to fetch_page() in ibd2sdi).
// If the tablespace is compressed, calls page_zip_decompress_low().
// ----------------------------------------------------------------
static bool fetch_page(
    File file_in,
    page_no_t page_no,
    const page_size_t &page_sz,
    unsigned char *uncompressed_buf,  // >= page_sz.logical() bytes
    size_t uncompressed_buf_len)
{
    size_t psize      = page_sz.physical();   // e.g. 8 KB
    size_t logical_sz = page_sz.logical();    // e.g. 16 KB

    // We temporarily read from disk into an 8 KB temp buffer if compressed
    // or 16 KB if uncompressed.  Either way, allocate "disk_buf" of psize.
    unsigned char* disk_buf = (unsigned char*)malloc(psize);
    if (!disk_buf) {
        fprintf(stderr, "Out of memory for disk_buf\n");
        return false;
    }

    // 1) Read raw page from disk
    if (!seek_page(file_in, page_sz, page_no)) {
        free(disk_buf);
        return false;
    }
    size_t r = my_read(file_in, disk_buf, psize, MYF(0));
    if (r != psize) {
        fprintf(stderr, "Could not read physical page %u correctly.\n", page_no);
        free(disk_buf);
        return false;
    }

    memset(uncompressed_buf, 0, uncompressed_buf_len);

    // 2) If compressed, decompress
    if (page_sz.is_compressed()) {
        // Set up a temp to hold the uncompressed data
        // i.e. 16 KB
        unsigned char* temp = (unsigned char*)ut::malloc(2 * logical_sz);
        unsigned char* aligned_temp =
            (unsigned char*)ut_align(temp, logical_sz);
        memset(aligned_temp, 0, logical_sz);

        page_zip_des_t page_zip;
        page_zip_des_init(&page_zip);

        // fill page_zip struct
        page_zip.data  = disk_buf;
        // For an 8 KB compressed page, this should end up being '4'.
        page_zip.ssize = page_size_to_ssize(psize);

        // 'disk_buf' has the raw 8 KB read from file
        //uint16_t page_type = mach_read_from_2(disk_buf + FIL_PAGE_TYPE);

        // default no
        bool success = false;

        // only process index pages
        //if (page_type != FIL_PAGE_INDEX) {

        //  mach_write_to_2(disk_buf + PAGE_HEADER + PAGE_N_HEAP, 10000);
        //}

        // decompress
        success = page_zip_decompress_low(&page_zip, aligned_temp, true);
        if (!success) {
          fprintf(stderr, "Failed to decompress page %u\n", page_no);
        } else {
          memcpy(uncompressed_buf, aligned_temp, logical_sz);
        }

        // do not return false here:
        ut::free(temp);
        free(disk_buf);
        return success;
    } else {
        // Uncompressed, just copy from 'disk_buf' to 'uncompressed_buf'
        memcpy(uncompressed_buf, disk_buf, psize);
    }

    free(disk_buf);
    return true;
}

// ----------------------------------------------------------------
// The main logic that reads each page from input, decompresses if needed,
// writes out the uncompressed page to the output.
// ----------------------------------------------------------------
static bool decompress_ibd(File in_fd, File out_fd)
{
  // 1) Determine size of in_fd
  MY_STAT stat_info;
  if (my_fstat(in_fd, &stat_info) != 0) {
    fprintf(stderr, "Cannot fstat() input file.\n");
    return false;
  }
  uint64_t total_bytes = stat_info.st_size;

  // 2) Determine page size
  page_size_t pg_sz(0, 0, false);
  if (!determine_page_size(in_fd, pg_sz)) {
    fprintf(stderr, "Could not determine page size.\n");
    return false;
  }

  // 3) Calculate number of pages
  uint64_t page_physical = static_cast<uint64_t>(pg_sz.physical());
  uint64_t num_pages = total_bytes / page_physical;
  fprintf(stderr, "Found %llu pages (each %llu bytes) in input.\n",
          (unsigned long long)num_pages,
          (unsigned long long)page_physical);

  // 4) For each page, fetch + decompress, then write out
  size_t buf_size = std::max(pg_sz.physical(), pg_sz.logical());
  unsigned char* page_buf = (unsigned char*)malloc(buf_size);
  if (!page_buf) {
    fprintf(stderr, "malloc of %llu bytes for page_buf failed.\n",
            (unsigned long long)page_physical);
    return false;
  }

  for (uint64_t i = 0; i < num_pages; i++) {
     if (!fetch_page(in_fd, (page_no_t)i, pg_sz, page_buf, buf_size)) {
      fprintf(stderr, "Error reading/decompressing page %llu.\n",
              (unsigned long long)i);
      //free(page_buf);
      //return false;      
    } else {
      // Write out the (uncompressed) page
      size_t w = my_write(out_fd, (uchar*)page_buf, pg_sz.logical(), MYF(0));
      if (w != UNIV_PAGE_SIZE_ORIG) {
        fprintf(stderr, "my_write failed on page %llu.\n", (unsigned long long)i);
        free(page_buf);
        return false;
      }
    }
  }

  free(page_buf);
  return true;
}

// ----------------------------------------------------------------
// main()
// - parse minimal options
// - open input + output
// - call decompress_ibd()
// ----------------------------------------------------------------
int main(int argc, char** argv)
{
  MY_INIT(argv[0]);
  DBUG_TRACE;
  DBUG_PROCESS(argv[0]);

  // parse options
  if (handle_options(&argc, &argv, decompress_opts, decompress_get_one_option)) {
    exit(EXIT_FAILURE);
  }
  if (opt_version) {
#ifdef NDEBUG
    print_version();
#else
    print_version_debug();
#endif
    return 0;
  }
  if (opt_help) {
    usage();
    return 0;
  }
  if (argc < 2) {
    // We expect 2 positional args: in_file, out_file
    usage();
    return 1;
  }

  const char *in_file  = argv[0];
  const char *out_file = argv[1];

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
