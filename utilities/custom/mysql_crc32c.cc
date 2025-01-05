/*
  Derived from MySQL 8.0's ut/crc32.cc
  This code is GPLv2 per original MySQL licensing.
  (See the large header comment from your snippet if you need the full text.)
*/

#include "mysql_crc32c.h"
#include <string.h>   // for memcpy, etc.

// ---------------------------------------------------------
// The polynomial 0x1EDC6F41 (Castagnoli), in its bit-reflected form 0x82F63B78.
// ---------------------------------------------------------
static const uint32_t CRC32C_POLY = 0x82F63B78U;

/*
  We'll build an 8x256 lookup table for "slice-by-8" CRC32C in software.
*/
static uint32_t crc32_slice8_table[8][256];

/*
  The actual function pointer we export.
  After initialization, mysql_crc32c points to mysql_crc32c_software().
*/
uint32_t (*mysql_crc32c)(const unsigned char *buf, size_t len) = NULL;

/* Forward declaration of our internal software-based routine. */
static uint32_t mysql_crc32c_software(const unsigned char *buf, size_t len);

// ---------------------------------------------------------
// 1) Build the slice-by-8 table
// ---------------------------------------------------------
static void build_crc32c_slice8_table(void)
{
  // bit-reversed polynomial = 0x82f63b78
  const uint32_t poly = CRC32C_POLY;
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) {
      if (c & 1) {
        c = (c >> 1) ^ poly;
      } else {
        c >>= 1;
      }
    }
    crc32_slice8_table[0][n] = c;
  }

  // For slice [1..7], build from table[0]
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = crc32_slice8_table[0][n];
    for (int k = 1; k < 8; k++) {
      c = crc32_slice8_table[0][c & 0xFF] ^ (c >> 8);
      crc32_slice8_table[k][n] = c;
    }
  }
}

// ---------------------------------------------------------
// 2) The software-based CRC-32C routine (matches MySQL 8.0 logic)
// ---------------------------------------------------------
static uint32_t mysql_crc32c_software(const unsigned char *buf, size_t len)
{
  // MySQL starts with 0xFFFFFFFF, and finalizes by inverting again.
  uint32_t crc = 0xFFFFFFFFU;

  // We'll process misaligned bytes one at a time:
  while (len > 0 && ((uintptr_t)buf & 7) != 0) {
    // Same as "crc32_8()" in MySQL code
    unsigned char index = (unsigned char)((crc ^ *buf) & 0xFF);
    crc = (crc >> 8) ^ crc32_slice8_table[0][index];
    buf++;
    len--;
  }

  // Process 8 bytes at a time
  while (len >= 8) {
    // read a 64-bit chunk
    uint64_t data;
    memcpy(&data, buf, sizeof(data));

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    // If big-endian, we must swap the bytes
    uint64_t swapped =  (uint64_t)( \
      ((data & 0xFF00000000000000ULL) >> 56) | \
      ((data & 0x00FF000000000000ULL) >> 40) | \
      ((data & 0x0000FF0000000000ULL) >> 24) | \
      ((data & 0x000000FF00000000ULL) >>  8) | \
      ((data & 0x00000000FF000000ULL) <<  8) | \
      ((data & 0x0000000000FF0000ULL) << 24) | \
      ((data & 0x000000000000FF00ULL) << 40) | \
      ((data & 0x00000000000000FFULL) << 56));
    data = swapped;
#endif

    // "crc32_64_low" inline from MySQL
    uint64_t x = (uint64_t)crc ^ data;

    crc = crc32_slice8_table[7][(x      ) & 0xFF] ^
          crc32_slice8_table[6][(x >>  8) & 0xFF] ^
          crc32_slice8_table[5][(x >> 16) & 0xFF] ^
          crc32_slice8_table[4][(x >> 24) & 0xFF] ^
          crc32_slice8_table[3][(x >> 32) & 0xFF] ^
          crc32_slice8_table[2][(x >> 40) & 0xFF] ^
          crc32_slice8_table[1][(x >> 48) & 0xFF] ^
          crc32_slice8_table[0][(x >> 56) & 0xFF];

    buf += 8;
    len -= 8;
  }

  // Process leftover bytes
  while (len > 0) {
    unsigned char index = (unsigned char)((crc ^ *buf) & 0xFF);
    crc = (crc >> 8) ^ crc32_slice8_table[0][index];
    buf++;
    len--;
  }

  return (crc ^ 0xFFFFFFFFU);
}

// ---------------------------------------------------------
// 3) Our public initialization function
// ---------------------------------------------------------
void mysql_crc32c_init(void)
{
  build_crc32c_slice8_table();
  // We skip hardware detection and always use the software version:
  mysql_crc32c = mysql_crc32c_software;
}
