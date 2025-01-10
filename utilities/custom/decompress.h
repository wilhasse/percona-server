// ---------------
// Declarations from decompress.cc
// ---------------
#include <cstddef>

/**
 * decompress_page_inplace():
 *   Decompress (or copy) one page in memory from src_buf -> out_buf.
 *
 *   is_compressed = true if the page is physically smaller than 16K,
 *   or if your logic says "this is a compressed page".
 *   In your code, you often rely on page_sz.is_compressed().
 */
extern bool decompress_page_inplace(
    const unsigned char* src_buf,
    size_t               physical_size,
    bool                 is_compressed,
    unsigned char*       out_buf,
    size_t               out_buf_len,
    size_t               logical_size
);

/**
 * decompress_ibd():
 *   The function that reads each page from `in_fd`, checks if compressed,
 *   and writes uncompressed pages to `out_fd`.
 */
bool decompress_ibd(File in_fd, File out_fd);

bool is_page_compressed(const unsigned char* page_data,
                               size_t physical_size,
                               size_t logical_size);

