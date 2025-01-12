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

#include "tables_dict.h"
#include "undrop_for_innodb.h"

/** A minimal column-definition struct */
struct MyColumnDef {
    std::string name;            // e.g., "id", "name", ...
    std::string type_utf8;       // e.g., "int", "char", "varchar"
    uint32_t    length;          // For char(N), the N, or 4 if int
    bool        is_nullable;     // Add this
    bool        is_unsigned;     // Add this too since it's used
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

// If you used "my_rec_offs_*" from the "undrop" style code:
extern ulint my_rec_offs_nth_size(const ulint* offsets, ulint i);
extern bool  my_rec_offs_nth_extern(const ulint* offsets, ulint i);
extern const unsigned char*
       my_rec_get_nth_field(const rec_t* rec, const ulint* offsets,
                            ulint i, ulint* len);

// --------------------------------------------------------------------
// 1) debug_print_table_def: print table->fields[] in a user-friendly way.
void debug_print_table_def(const table_def_t *table)
{
    if (!table) {
        printf("[debug_print_table_def] table is NULL\n");
        return;
    }

    printf("=== Table Definition for '%s' ===\n", (table->name ? table->name : "(null)"));
    printf("fields_count=%u, n_nullable=%u\n", table->fields_count, table->n_nullable);

    // Possibly also print data_min_size / data_max_size if your code uses them:
    // e.g. printf("data_min_size=%d, data_max_size=%ld\n",
    //              table->data_min_size, table->data_max_size);

    for (int i = 0; i < table->fields_count; i++) {
        const field_def_t *fld = &table->fields[i];

        // for "type" => we have an enum { FT_INT, FT_UINT, FT_CHAR, FT_TEXT, FT_DATETIME, FT_INTERNAL, etc. }
        // We'll define a helper to map enum => string:
        const char* type_str = nullptr;
        switch (fld->type) {
        case FT_INTERNAL:   type_str = "FT_INTERNAL"; break;
        case FT_INT:        type_str = "FT_INT";      break;
        case FT_UINT:       type_str = "FT_UINT";     break;
        case FT_CHAR:       type_str = "FT_CHAR";     break;
        case FT_TEXT:       type_str = "FT_TEXT";     break;
        case FT_BLOB:       type_str = "FT_BLOB";     break;
        case FT_DATETIME:   type_str = "FT_DATETIME"; break;
        case FT_FLOAT:      type_str = "FT_FLOAT";    break;
        case FT_DOUBLE:     type_str = "FT_DOUBLE";   break;
        // ... if you have more, add them
        default:            type_str = "FT_???";      break;
        }

        printf(" Field #%u:\n", i);
        printf("   name=%s\n", (fld->name ? fld->name : "(null)"));
        printf("   type=%s\n", type_str);
        printf("   can_be_null=%s\n", (fld->can_be_null ? "true" : "false"));
        printf("   fixed_length=%u\n", fld->fixed_length);
        printf("   min_length=%u, max_length=%u\n", fld->min_length, fld->max_length);
        printf("   decimal_precision=%d, decimal_digits=%d\n",
               fld->decimal_precision, fld->decimal_digits);
        printf("   time_precision=%d\n", fld->time_precision);
    }
    printf("=== End of Table Definition ===\n\n");
}

// --------------------------------------------------------------------
// 2) debug_print_compact_row: read each field from offsets[] 
//    and print in a "rough" typed format (e.g. int => 4 bytes, char => string).
//
//    This is for demonstration or debugging. 
//    If your code calls "check_for_a_record(...)" first to build offsets, 
//    you can then do:
//      debug_print_compact_row(page, rec, table, offsets);
// 
void debug_print_compact_row(const page_t* page,
                             const rec_t* rec,
                             const table_def_t* table,
                             const ulint* offsets)
{
    if (!page || !rec || !table || !offsets) {
        printf("[debug_print_compact_row] invalid pointer(s)\n");
        return;
    }

    // Print a header line or something
    printf("Row at rec=%p => columns:\n", (const void*)rec);

    // For each field
    for (ulint i = 0; i < (ulint)table->fields_count; i++) {

        // read the pointer and length
        ulint field_len;
        const unsigned char* field_ptr = my_rec_get_nth_field(rec, offsets, i, &field_len);

        // If length is UNIV_SQL_NULL => print "NULL"
        if (field_len == UNIV_SQL_NULL) {
            printf("  [%2lu] %-15s => NULL\n", i, table->fields[i].name);
            continue;
        }

        // Otherwise interpret based on "table->fields[i].type"
        switch (table->fields[i].type) {
        case FT_INT:
        case FT_UINT:
            // If it's truly a 4-byte int, let's read it:
            if (field_len == 4) {
                // typical InnoDB "int" might also do sign-flipping. 
                // For a quick debug, let's do:
                uint32_t val = 0;
                memcpy(&val, field_ptr, 4);
                // If you do sign-flipping => val ^= 0x80000000;
                printf("  [%2lu] %-15s => (INT) %u\n",
                       i, table->fields[i].name, val);
            } else {
                // length isn't 4 => just hex-dump or do naive printing
                printf("  [%2lu] %-15s => (INT?) length=%lu => ",
                       i, table->fields[i].name, (unsigned long)field_len);
                for (ulint k=0; k<field_len && k<16; k++) {
                    printf("%02X ", field_ptr[k]);
                }
                printf("\n");
            }
            break;

        case FT_CHAR:
        case FT_TEXT:
            // Treat as textual => do a naive printing (limit ~200 bytes for safety)
            {
                ulint to_print = (field_len < 200 ? field_len : 200);
                printf("  [%2lu] %-15s => (CHAR) len=%lu => \"", 
                       i, table->fields[i].name, (unsigned long)field_len);
                for (ulint k=0; k<to_print; k++) {
                    unsigned char c = field_ptr[k];
                    if (c >= 32 && c < 127) {
                        putchar((int)c);
                    } else {
                        // print as \xNN
                        printf("\\x%02X", c);
                    }
                }
                if (field_len > 200) printf("...(truncated)...");
                printf("\"\n");
            }
            break;

        case FT_DATETIME:
            // If we treat it as 5 or 8 bytes, let's do a mini decode
            if (field_len == 5) {
                // e.g. MySQL 5.6 DATETIME(0) in "COMPACT" 
                // This is advanced, but let's just hex dump for now:
                printf("  [%2lu] %-15s => (DATETIME-5) => ",
                       i, table->fields[i].name);
                for (ulint k=0; k<field_len; k++) {
                    printf("%02X ", field_ptr[k]);
                }
                printf("\n");
            } else if (field_len == 8) {
                // older DATETIME
                // same approach
                printf("  [%2lu] %-15s => (DATETIME-8) => ",
                       i, table->fields[i].name);
                for (ulint k=0; k<8; k++) {
                    printf("%02X ", field_ptr[k]);
                }
                printf("\n");
            } else {
                // fallback
                printf("  [%2lu] %-15s => (DATETIME) length=%lu => raw hex ",
                       i, table->fields[i].name, (unsigned long)field_len);
                for (ulint k=0; k<field_len && k<16; k++) {
                    printf("%02X ", field_ptr[k]);
                }
                printf("\n");
            }
            break;

        case FT_INTERNAL:
            // e.g. DB_TRX_ID(6 bytes) or DB_ROLL_PTR(7 bytes)
            printf("  [%2lu] %-15s => (INTERNAL) length=%lu => ", 
                   i, table->fields[i].name, (unsigned long)field_len);
            for (ulint k=0; k<field_len && k<16; k++) {
                printf("%02X ", field_ptr[k]);
            }
            printf("\n");
            break;

        // ... other types (FLOAT, DOUBLE, DECIMAL, BLOB, etc.)
        default:
            // fallback => hex-dump
            printf("  [%2lu] %-15s => (type=%d) length=%lu => ",
                   i, table->fields[i].name, table->fields[i].type,
                   (unsigned long)field_len);
            for (ulint k=0; k<field_len && k<16; k++) {
                printf("%02X ", field_ptr[k]);
            }
            if (field_len>16) printf("...(truncated)...");
            printf("\n");
            break;
        } // switch
    }

    printf("End of row\n\n");
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
 * build_table_def_from_json():
 *   Creates a table_def_t from the columns in g_columns.
 *   This way, you can pass table_def_t into ibrec_init_offsets_new().
 *
 *   @param[out] table    The table_def_t to fill
 *   @param[in]  tbl_name The table name (e.g. "HISTORICO")
 *   @return 0 on success
 */
int build_table_def_from_json(table_def_t* table, const char* tbl_name)
{
    // 1) Zero out "table_def_t"
    std::memset(table, 0, sizeof(table_def_t));

    // 2) Copy the table name
    table->name = strdup(tbl_name);

    // 3) Loop over columns
    unsigned colcount = 0;
    for (size_t i = 0; i < g_columns.size(); i++) {
        if (colcount >= MAX_TABLE_FIELDS) {
            fprintf(stderr, "[Error] Too many columns (>MAX_TABLE_FIELDS)\n");
            return 1;
        }

        field_def_t* fld = &table->fields[colcount];
        std::memset(fld, 0, sizeof(*fld));

        // (A) Name
        fld->name = strdup(g_columns[i].name.c_str());

        // (B) is_nullable => can_be_null
        // If the JSON had is_nullable => g_columns[i].nullable, adapt.
        // Let's assume we store is_nullable in g_columns[i].is_nullable:
        fld->can_be_null = g_columns[i].is_nullable;

        // (C) If the JSON had "is_unsigned" => store or adapt type
        // For example, if we see "int" + is_unsigned => FT_UINT
        bool is_unsigned = g_columns[i].is_unsigned; // e.g. from JSON

        // (D) "type_utf8" => decide the main field type
        if (g_columns[i].type_utf8.find("int") != std::string::npos) {
            if (is_unsigned) {
                fld->type = FT_UINT;
            } else {
                fld->type = FT_INT;
            }
            // 4 bytes typical
            fld->fixed_length = 4;
            // For check_fields_sizes => maybe min_length=4, max_length=4
            fld->min_length = 4;
            fld->max_length = 4;

        } else if (g_columns[i].type_utf8.find("char") != std::string::npos) {
            // Suppose "char(N)" => fixed_length = N
            // or "varchar(N)" => fixed_length=0, max_length=N
            // For a simplistic approach, do:
            fld->type = FT_CHAR;
            // if we see something like "char(1)", we do:
            fld->fixed_length = 0; // treat as variable
            fld->min_length = 0;
            fld->max_length = g_columns[i].length; 
            // (If your code wants a truly fixed CHAR(1), you can do that logic.)

        } else if (g_columns[i].type_utf8.find("datetime") != std::string::npos) {
            // Usually datetime is 5 or 8 bytes in "undrop" logic
            fld->type         = FT_DATETIME;
            fld->fixed_length = 5; // or 8
            // min_length=5, max_length=5 for check_fields_sizes
            fld->min_length   = 5;
            fld->max_length   = 5;

        } else {
            // fallback => treat as text
            fld->type = FT_TEXT;
            fld->fixed_length = 0; 
            fld->min_length   = 0;
            // if JSON has "char_length" => we do
            fld->max_length   = g_columns[i].length;
        }

        // (E) Possibly parse numeric precision, scale => decimal_digits, etc.
        // (F) Possibly parse "char_length" => do above or below.

        // done
        colcount++;
    }

    // 5) fields_count
    table->fields_count = colcount;

    // 6) Optionally compute table->n_nullable
    table->n_nullable = 0;
    for (unsigned i = 0; i < colcount; i++) {
        if (table->fields[i].can_be_null) {
            table->n_nullable++;
        }
    }

    // optionally set data_max_size, data_min_size
    // or do so in your calling code if you want consistent row checks.

    return 0;
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
    return; // Not primary => skip
  }

  // 2) Check if it’s a LEAF page
  ulint page_level = mach_read_from_2(page + PAGE_HEADER + PAGE_LEVEL);
  if (page_level != 0) {
    // Non-leaf => skip
    return;
  }

  std::cout << "Page " << page_no 
            << " is primary index leaf. Parsing records.\n";

  // 3) Check if COMPACT or REDUNDANT
  bool is_compact = page_is_comp(page);

  // 4) infimum / supremum offsets
  ulint inf_offset = (is_compact) ? PAGE_NEW_INFIMUM : PAGE_OLD_INFIMUM;
  ulint sup_offset = (is_compact) ? PAGE_NEW_SUPREMUM : PAGE_OLD_SUPREMUM;

  // 5) Loop from infimum -> supremum
  ulint rec_offset = inf_offset;
  ulint n_records  = 0;

  while (rec_offset != sup_offset) {
    // Safety checks
    if (rec_offset < 2 || rec_offset + 2 > page_size) {
      // Looks corrupted => break
      break;
    }

    // Next record pointer is stored differently in COMPACT vs. REDUNDANT
    ulint next_off;
    if (is_compact) {
      next_off = mach_read_from_2(page + rec_offset - 2);
      next_off = rec_offset + next_off;
    } else {
      // Redundant
      next_off = mach_read_from_2(page + rec_offset - 2);
    }

    // skip infimum
    if (rec_offset != inf_offset) {
      n_records++;
      std::cout << "  - Found record at offset " 
                << rec_offset << " (page " << page_no << ")\n";

      // (A) We'll do the undrop approach: check_for_a_record() => if valid => process_ibrec()
      rec_t* rec = (rec_t*)(page + rec_offset);
      ulint offsets[MAX_TABLE_FIELDS + 2];

      // Using the JSON-based table you previously set
      table_def_t* table = &table_definitions[0];

      // (A.1) Attempt to build offsets + check constraints
      bool valid = check_for_a_record(
          (page_t*)page, 
          rec, 
          table, 
          offsets);

      // Debug data
      debug_print_compact_row(page, rec, table, offsets);

      if (valid) {
        // (A.2) If valid => call process_ibrec() to print each column
        bool hex_output = false; // or true if you want hex strings
        process_ibrec((page_t*)page, rec, table, offsets, hex_output);
      }
    }

    if (next_off == rec_offset || next_off >= page_size) {
      // corruption or end
      break;
    }
    rec_offset = next_off;
  }

  std::cout << "Leaf Page " << page_no
            << " had " << n_records << " user records.\n";
}
