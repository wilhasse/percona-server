// ---------------
// Declarations from parser.cc
// ---------------
#include <cstdint>
#include <cstddef>
#include "page0page.h"
#include "tables_dict.h"

void parse_records_on_page(const unsigned char* page,
                           size_t page_size,
                           uint64_t page_no);

int discover_primary_index_id(int fd);

bool is_primary_index(const unsigned char* page);

int load_ib2sdi_table_columns(const char* json_path);

int build_table_def_from_json(table_def_t* table, const char* tbl_name);

void debug_print_table_def(const table_def_t *table);

void debug_print_compact_row(const page_t* page,
                             const rec_t* rec,
                             const table_def_t* table,
                             const ulint* offsets);
