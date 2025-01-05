#ifndef MYSQL_CRC32C_H
#define MYSQL_CRC32C_H

#include <stddef.h>   // for size_t
#include <stdint.h>   // for uint32_t, uint64_t

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initializes the internal lookup tables used by mysql_crc32c(...).
 * Call this exactly once before your first checksum call.
 * Safe to call multiple times (it just re-initializes).
 */
void mysql_crc32c_init(void);

/**
 * Pointer to the CRC-32C function (software version).
 * After you call mysql_crc32c_init(), this function pointer is set
 * to the internal software-based function that MySQL uses.
 *
 * Usage:
 *   uint32_t crc = mysql_crc32c(buffer, length);
 */
extern uint32_t (*mysql_crc32c)(const unsigned char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* MYSQL_CRC32C_H */
