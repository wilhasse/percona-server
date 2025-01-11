// Standard C++ includes
#include <iostream>
#include <cstdint>

// InnoDB includes
#include "page0page.h"  // For page structure constants
#include "rem0rec.h"    // For record handling
#include "mach0data.h"  // For mach_read_from_2
#include "ut0byte.h"    // For byte utilities

#include <iostream>
#include <cstdint>

// InnoDB includes
#include "page0page.h"         // FIL_PAGE_INDEX, PAGE_HEADER, etc.
#include "fil0fil.h"           // fil_page_get_type()
#include "rem0rec.h"           // btr_root_fseg_validate() might be declared
#include "mach0data.h"         // mach_read_from_8(), mach_read_from_4()
#include "fsp0fsp.h"           // FSP_SPACE_ID
#include "fsp0types.h"         // btr_root_fseg_validate() signature

// Adapt from your code:
static const size_t kPageSize = 16384;

// We'll store the discovered ID in this struct:
struct Dulint {
  uint32_t high;
  uint32_t low;
};

// Global or static variable to store the discovered PRIMARY index ID.
static Dulint g_primary_index_id = {0, 0}; 
static bool   g_found_primary    = false;

/**
 * btr_root_fseg_validate():
 *   find the first valid root page
 */
static bool btr_root_fseg_validate(const unsigned char* page, uint32_t space_id) {
    // Simplified check - in real InnoDB this does more validation
    // For now just check if the space_id matches and basic offset is valid
    uint32_t page_space_id = mach_read_from_4(page);
    return (page_space_id == space_id) && (mach_read_from_4(page + 4) != 0);
}

/**
 * read_uint64_from_page():
 *   Utility to read a 64-bit big-endian integer from 'ptr'.
 */
static inline uint64_t read_uint64_from_page(const unsigned char* ptr) {
  return mach_read_from_8(ptr);
}

/**
 * discover_primary_index_id():
 *   Scans all pages to find the *first* root page
 *   (FIL_PAGE_INDEX + btr_root_fseg_validate() checks).
 *   Once found, read PAGE_INDEX_ID from that page
 *   and store in g_primary_index_id. 
 *
 *   Returns 0 if success, non-0 if error.
 */
int discover_primary_index_id(int fd)
{
  // 1) get file size
  struct stat stat_buf;
  if (fstat(fd, &stat_buf) == -1) {
    perror("fstat");
    return 1;
  }

  // 2) compute number of pages
  int block_num = stat_buf.st_size / kPageSize;
  if (block_num == 0) {
    fprintf(stderr, "Empty file?\n");
    return 1;
  }

  // 3) read the "space id" from page 0 
  //    (this is used by btr_root_fseg_validate).
  unsigned char page_buf[kPageSize];
  if (pread(fd, page_buf, kPageSize, 0) != (ssize_t)kPageSize) {
    perror("pread page0");
    return 1;
  }
  uint32_t space_id = mach_read_from_4(page_buf + FSP_HEADER_OFFSET + FSP_SPACE_ID);

  // 4) loop over each page
  for (int i = 0; i < block_num; i++) {
    off_t offset = (off_t) i * kPageSize;
    if (pread(fd, page_buf, kPageSize, offset) != (ssize_t)kPageSize) {
      // partial read => break or return error
      break;
    }

    // check if FIL_PAGE_INDEX
    if (fil_page_get_type(page_buf) == FIL_PAGE_INDEX) {
      // Check if this is a *root* page (like ShowIndexSummary does)
      // by verifying the fseg headers for leaf and top
      bool is_root = btr_root_fseg_validate(page_buf + FIL_PAGE_DATA + PAGE_BTR_SEG_LEAF, space_id)
                  && btr_root_fseg_validate(page_buf + FIL_PAGE_DATA + PAGE_BTR_SEG_TOP, space_id);

      if (is_root) {
        // We consider the *first* root we find as the "Primary index"
        uint64_t idx_id_64 = read_uint64_from_page(page_buf + PAGE_HEADER + PAGE_INDEX_ID);
        g_primary_index_id.high = static_cast<uint32_t>(idx_id_64 >> 32);
        g_primary_index_id.low  = static_cast<uint32_t>(idx_id_64 & 0xffffffff);

        g_found_primary = true;
        fprintf(stderr, "discover_primary_index_id: Found primary root at page=%d  index_id=%u:%u\n",
                i, g_primary_index_id.high, g_primary_index_id.low);

        // done
        return 0;
      }
    }
  }

  // if we got here, we never found a root => maybe the table is empty or corrupted
  fprintf(stderr, "discover_primary_index_id: No root page found => can't identify primary.\n");
  return 1;
}

bool is_primary_index(const unsigned char* page)
{
  if (!g_found_primary) {
    // We never discovered the primary => default to false
    return false;
  }

  uint64_t page_index_id = read_uint64_from_page(page + PAGE_HEADER + PAGE_INDEX_ID);
  uint32_t high = static_cast<uint32_t>(page_index_id >> 32);
  uint32_t low  = static_cast<uint32_t>(page_index_id & 0xffffffff);

  return (high == g_primary_index_id.high && low == g_primary_index_id.low);
}

/**
 * Example: parse and print records from a leaf page.
 * Very minimal, ignoring many corner cases.
 *
 * If you want the "table-based" field parsing from undrop-for-innodb,
 * you’d bring in structures like table_def_t, rec_offs_* helpers, etc.
 */
void parse_records_on_page(const unsigned char* page,
                           size_t page_size,
                           uint64_t page_no)
{
  // 1) Check if this page belongs to the primary index
  if (!is_primary_index(page)) {
    // Not primary => skip
    return;
  }

  // 2) Check page level
  ulint level = mach_read_from_2(page + PAGE_HEADER + PAGE_LEVEL);
  if (level != 0) {
    // Not a leaf => skip
    return;
  }

  // From here on, we know it’s a leaf of the primary index
  std::cout << "Page " << page_no << " is primary index leaf. Parsing records.\n";

  // 3) Check if it's compact or redundant
  bool is_compact = page_is_comp(page);

  // 4) We find the "infimum" / "supremum" offsets
  ulint inf_offset = (is_compact) ? PAGE_NEW_INFIMUM : PAGE_OLD_INFIMUM;
  ulint sup_offset = (is_compact) ? PAGE_NEW_SUPREMUM : PAGE_OLD_SUPREMUM;

  // 5) Basic loop from infimum -> supremum
  ulint rec_offset = inf_offset;
  ulint n_records  = 0;

  while (rec_offset != sup_offset) {
    // Safety checks
    if (rec_offset < 2 || rec_offset + 2 > page_size) {
      // Corrupted pointer
      break;
    }

    // Next record pointer is stored differently in compact vs. redundant
    ulint next_off;
    if (is_compact) {
      // The last 2 bytes before the record store the "next" offset
      next_off = mach_read_from_2(page + rec_offset - 2);
      next_off = rec_offset + next_off;
    } else {
      // Redundant
      next_off = mach_read_from_2(page + rec_offset - 2);
    }

    // Skip the infimum record itself
    if (rec_offset != inf_offset) {
      // Attempt to parse or print this record
      n_records++;
      std::cout << "  - Found record at offset " << rec_offset
                << " (page " << page_no << ")\n";
    }

    if (next_off == rec_offset || next_off >= page_size) {
      // Corruption or end
      break;
    }
    rec_offset = next_off;
  }

  std::cout << "Leaf Page " << page_no
            << " had " << n_records << " user records.\n";
}
