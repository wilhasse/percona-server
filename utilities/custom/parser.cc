// Standard C++ includes
#include <iostream>
#include <cstdint>

// InnoDB includes
#include "page0page.h"  // For page structure constants
#include "rem0rec.h"    // For record handling
#include "mach0data.h"  // For mach_read_from_2
#include "ut0byte.h"    // For byte utilities

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
  // 1) Check if it's compact or redundant
  bool is_compact = page_is_comp(page);

  // 2) We find the "infimum" / "supremum" offsets
  //    In reality, you'd do more robust scanning.
  ulint inf_offset = (is_compact) ? PAGE_NEW_INFIMUM : PAGE_OLD_INFIMUM;
  ulint sup_offset = (is_compact) ? PAGE_NEW_SUPREMUM : PAGE_OLD_SUPREMUM;

  // 3) Basic loop from infimum -> supremum
  //    We'll do a naive forward-lists approach
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
      // The last 2 bytes *before* the record store the "next" offset
      next_off = mach_read_from_2(page + rec_offset - 2);
      next_off = rec_offset + next_off;
    } else {
      // Redundant
      next_off = mach_read_from_2(page + rec_offset - 2);
    }

    // Skip the infimum record itself
    if (rec_offset != inf_offset) {
      // Attempt to parse or print this record
      // In a real system, you'd do: rec_t* rec = (rec_t*)(page + rec_offset);
      // Then parse the fields, etc.
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
