// Standard C++ includes
#include <iostream>
#include <cstdint>
#include <vector>
#include <string>
#include <cstdio>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/istreamwrapper.h>
#include <fstream>
#include <iostream>

// InnoDB includes
#include "page0page.h"  // For page structure constants
#include "rem0rec.h"    // For record handling
#include "mach0data.h"  // For mach_read_from_2
#include "ut0byte.h"    // For byte utilities
#include "page0page.h"  // FIL_PAGE_INDEX, PAGE_HEADER, etc.
#include "fil0fil.h"    // fil_page_get_type()
#include "rem0rec.h"    // btr_root_fseg_validate() might be declared
#include "mach0data.h"  // mach_read_from_8(), mach_read_from_4()
#include "fsp0fsp.h"    // FSP_SPACE_ID
#include "fsp0types.h"  // btr_root_fseg_validate() signature

/** A minimal column-definition struct */
struct MyColumnDef {
    std::string name;            // e.g., "id", "name", ...
    std::string type_utf8;       // e.g., "int", "char", "varchar"
    uint32_t    length;          // For char(N), the N, or 4 if int
};

/** We store the columns here, loaded from JSON. */
static std::vector<MyColumnDef> g_columns;

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

  // Standard Innodb Page Size
  // TODO: I couldn't use UNIV_PAGE_SIZE due to variable-length array (VLA)
  static const size_t kPageSize = 16384;

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
 * load_ib2sdi_table_columns():
 *   Parses an ib2sdi-generated JSON file (like the one you pasted),
 *   searches for the array element that has "dd_object_type" == "Table",
 *   then extracts its "columns" array from "dd_object".
 *
 * Returns 0 on success, non-0 on error.
 */
int load_ib2sdi_table_columns(const char* json_path)
{
    // 1) Open the file
    std::ifstream ifs(json_path);
    if (!ifs.is_open()) {
        std::cerr << "[Error] Could not open JSON file: " << json_path << std::endl;
        return 1;
    }

    // 2) Parse the top-level JSON
    rapidjson::IStreamWrapper isw(ifs);
    rapidjson::Document d;
    d.ParseStream(isw);
    if (d.HasParseError()) {
        std::cerr << "[Error] JSON parse error: " 
                  << rapidjson::GetParseError_En(d.GetParseError())
                  << " at offset " << d.GetErrorOffset() << std::endl;
        return 1;
    }

    if (!d.IsArray()) {
        std::cerr << "[Error] Top-level JSON is not an array.\n";
        return 1;
    }

    // 3) Find the array element whose "dd_object_type" == "Table".
    //    In your example, you had something like:
    //    [
    //       "ibd2sdi",
    //       { "type":1, "object": { "dd_object_type":"Table", ... } },
    //       { "type":2, "object": { "dd_object_type":"Tablespace", ... } }
    //    ]
    // We'll loop the array to find the "Table" entry.

    const rapidjson::Value* table_obj = nullptr;

    for (auto& elem : d.GetArray()) {
        // Each elem might be "ibd2sdi" (string) or an object with { "type":..., "object":... }
        if (elem.IsObject() && elem.HasMember("object")) {
            const rapidjson::Value& obj = elem["object"];
            if (obj.HasMember("dd_object_type") && obj["dd_object_type"].IsString()) {
                std::string ddtype = obj["dd_object_type"].GetString();
                if (ddtype == "Table") {
                    // Found the table element
                    table_obj = &obj;
                    break; 
                }
            }
        }
    }

    if (!table_obj) {
        std::cerr << "[Error] Could not find any array element with dd_object_type=='Table'.\n";
        return 1;
    }

    // 4) Inside that "object", we want "dd_object" => "columns"
    //    i.e. table_obj->HasMember("dd_object") => columns in table_obj["dd_object"]["columns"]
    if (!table_obj->HasMember("dd_object")) {
        std::cerr << "[Error] Table object is missing 'dd_object' member.\n";
        return 1;
    }
    const rapidjson::Value& dd_obj = (*table_obj)["dd_object"];

    if (!dd_obj.HasMember("columns") || !dd_obj["columns"].IsArray()) {
        std::cerr << "[Error] 'dd_object' is missing 'columns' array.\n";
        return 1;
    }

    const rapidjson::Value& columns = dd_obj["columns"];
    g_columns.clear();

    // 5) Iterate the columns array
    for (auto& c : columns.GetArray()) {
        // We expect "name", "column_type_utf8", "char_length" in each
        if (!c.HasMember("name") || !c.HasMember("column_type_utf8") || !c.HasMember("char_length")) {
            // Some columns might be hidden or missing fields
            // That's typical for DB_TRX_ID, DB_ROLL_PTR, etc.
            // We'll just skip them or give defaults.
            // For demo, skip if missing 'name' or 'column_type_utf8'
            if (!c.HasMember("name") || !c.HasMember("column_type_utf8")) {
                std::cerr << "[Warn] A column is missing 'name' or 'column_type_utf8'. Skipping.\n";
                continue;
            }
        }

        MyColumnDef def;
        def.name      = c["name"].GetString();
        def.type_utf8 = c["column_type_utf8"].GetString();

        // default length = 4 if "int"? 
        // or from "char_length"
        uint32_t length = 4; // fallback
        if (c.HasMember("char_length") && c["char_length"].IsUint()) {
            length = c["char_length"].GetUint();
        }
        def.length = length;

        // Add to global vector
        g_columns.push_back(def);

        // Optional debug
        std::cout << "[Debug] Added column: name='" << def.name
                  << "', type='" << def.type_utf8
                  << "', length=" << def.length << "\n";
    }

    ifs.close();
    return 0;
}

/**
 * parse_and_print_record():
 *   Given the page buffer + offset, read each column 
 *   from g_columns, and print their values.
 *
 *   This is very simplistic: it treats "int" as 4 bytes,
 *   "char"/"varchar" as 'length' bytes, etc.
 *   Adapt as needed for real InnoDB row formats.
 */
static void parse_and_print_record(const unsigned char* page,
                                   size_t page_size,
                                   ulint rec_offset)
{
    // For a real solution, you'd want the "rec" pointer, etc.
    // We'll do a minimal approach:
    const rec_t* rec = reinterpret_cast<const rec_t*>(page + rec_offset);

    // Example: you might check rec_get_status(rec) or rec_get_deleted_flag()

    std::cout << "  Parsing record at offset " << rec_offset << "...\n";

    // Loop over the columns from the JSON
    for (size_t i = 0; i < g_columns.size(); i++) {
        const MyColumnDef& col = g_columns[i];

        // For demonstration, we assume columns are laid out in 
        // a fixed offset: i.e. col i starts at offset (some calculation).
        // This is NOT how real InnoDB row format works! 
        // In real code, you’d build field offsets with rec_offs_* logic 
        // or the undrop approach. 
        // We'll do a naive offset = sum of previous columns' lengths:
        static size_t cumulative_offset = 0; 
        // obviously you'd store these offsets in a global array or something.

        std::cout << "    Column '" << col.name << "' = ";

        if (col.type_utf8 == "int") {
            // read 4 bytes, maybe do sign flipping
            uint32_t val = mach_read_from_4((byte*)rec + cumulative_offset);
            val ^= 0x80000000; // if you do that in your table
            std::cout << val << "\n";
            cumulative_offset += 4;
        } else {
            // char or varchar
            std::string s((char*)rec + cumulative_offset, col.length);
            std::cout << s << "\n";
            cumulative_offset += col.length;
        }
    }

    // Possibly read hidden columns (DB_TRX_ID, DB_ROLL_PTR) if you want.
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

    // **Here** we call parse_and_print_record to interpret columns
    parse_and_print_record(page, page_size, rec_offset);
      
    if (next_off == rec_offset || next_off >= page_size) {
      // Corruption or end
      break;
    }
    rec_offset = next_off;
  }

  std::cout << "Leaf Page " << page_no
            << " had " << n_records << " user records.\n";
}
