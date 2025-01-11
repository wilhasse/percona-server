// ---------------
// Declarations from parser.cc
// ---------------
#include <cstdint>
#include <cstddef>

void parse_records_on_page(const unsigned char* page,
                           size_t page_size,
                           uint64_t page_no);