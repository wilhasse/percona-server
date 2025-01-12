/**
 * undrop_for_innodb.cc
 *
 * Code extracted from undrop_for_innodb.cc.
 *
 * A minimal "undrop" style code that:
 *   - avoids MySQL rec_get_nth_field(),
 *   - uses a local "my_rec_get_nth_field()" for retrieving column data,
 *   - prints a simple pipe-separated row of data,
 *   - prints column headers only once.
 */
#include <cstdio>
#include <cstdint>
#include <cstring>

// MySQL includes
#include "tables_dict.h"
#include "univ.i"
#include "page0page.h"
#include "rem0rec.h"

// Undefine MySQL rec_offs_* if they exist
#ifdef rec_offs_nth_size
#undef rec_offs_nth_size
#endif
#ifdef rec_offs_nth_extern
#undef rec_offs_nth_extern
#endif
#ifdef rec_offs_n_fields
#undef rec_offs_n_fields
#endif
#ifdef rec_offs_data_size
#undef rec_offs_data_size
#endif
#ifdef rec_offs_base
#undef rec_offs_base
#endif
#ifdef rec_offs_set_n_fields
#undef rec_offs_set_n_fields
#endif

#ifndef UNIV_SQL_NULL
#define UNIV_SQL_NULL 0xFFFFFFFF
#endif
#ifndef REC_OFFS_SQL_NULL
#define REC_OFFS_SQL_NULL     0x80000000UL
#endif
#ifndef REC_OFFS_EXTERNAL
#define REC_OFFS_EXTERNAL     0x40000000UL
#endif

/** Our local "undrop" offset array format:
  offsets[0] = #fields
  offsets[i+1] = offset bits for field i
*/

/** my_rec_offs_n_fields() => number of fields. */
inline ulint my_rec_offs_n_fields(const ulint* offsets) {
  return offsets[0];
}

/** my_rec_offs_set_n_fields() => sets #fields. */
inline void my_rec_offs_set_n_fields(ulint* offsets, ulint n) {
  offsets[0] = n;
}

/** my_rec_offs_nth_size() => returns length for i-th field. */
inline ulint my_rec_offs_nth_size(const ulint* offsets, ulint i) {
  ulint end   = (offsets[i+1] & ~(REC_OFFS_SQL_NULL | REC_OFFS_EXTERNAL));
  ulint start = (offsets[i]   & ~(REC_OFFS_SQL_NULL | REC_OFFS_EXTERNAL));
  if (end < start) {
    return UNIV_SQL_NULL;
  }
  return (end - start);
}

/** my_rec_offs_nth_extern() => checks EXTERNAL bit. */
inline bool my_rec_offs_nth_extern(const ulint* offsets, ulint i) {
  return (offsets[i+1] & REC_OFFS_EXTERNAL) != 0;
}

/** my_rec_offs_data_size() => basic row data size. */
inline ulint my_rec_offs_data_size(const ulint* offsets) {
  ulint n = offsets[0];
  // For example, end=offsets[n], start=offsets[1]
  ulint end   = (offsets[n] & ~(REC_OFFS_SQL_NULL | REC_OFFS_EXTERNAL));
  ulint start = (offsets[1] & ~(REC_OFFS_SQL_NULL | REC_OFFS_EXTERNAL));
  if (end < start) return 0;
  return end - start;
}

/** my_rec_get_nth_field() => returns pointer and length for i-th field. */
inline const unsigned char*
my_rec_get_nth_field(const rec_t* rec, const ulint* offsets,
                     ulint i, ulint* len)
{
  ulint end_bits   = offsets[i+1];
  ulint start_bits = offsets[i];

  bool is_null    = (end_bits   & REC_OFFS_SQL_NULL) != 0;
  bool is_extern  = (end_bits   & REC_OFFS_EXTERNAL) != 0;
  (void)is_extern; // if we don't handle external data

  ulint end   = end_bits   & ~(REC_OFFS_SQL_NULL | REC_OFFS_EXTERNAL);
  ulint start = start_bits & ~(REC_OFFS_SQL_NULL | REC_OFFS_EXTERNAL);

  if (is_null) {
    *len = UNIV_SQL_NULL;
  } else {
    *len = (end >= start) ? (end - start) : 0;
  }

  return (const unsigned char*)rec + start;
}

/** check_fields_sizes() => minimal check for each field. */
inline bool check_fields_sizes(const rec_t* rec, table_def_t* table, ulint* offsets)
{
  // remove "ulint n = offsets[0]" since we don't use it
  // remove "field_ptr" or cast to (void) if not used

  for (ulint i = 0; i < (ulint)table->fields_count; i++) {
    ulint field_len;
    (void)my_rec_get_nth_field(rec, offsets, i, &field_len); // if we don't actually need 'field_ptr'

    // Check range
    if (field_len != UNIV_SQL_NULL) {
      ulint minL = (ulint)table->fields[i].min_length;
      ulint maxL = (ulint)table->fields[i].max_length;
      if (field_len < minL || field_len > maxL) {
        printf("ERROR: field #%lu => length %lu out of [%u..%u]\n",
               (unsigned long)i, (unsigned long)field_len,
               table->fields[i].min_length, table->fields[i].max_length);
        return false;
      }
    }
  }
  return true;
}

/** ibrec_init_offsets_new() => fill offsets array for a COMPACT record. */
inline bool ibrec_init_offsets_new(const page_t* page,
                                   const rec_t* rec,
                                   table_def_t* table,
                                   ulint* offsets)
{
  ulint status = rec_get_status((rec_t*)rec);
  if (status != REC_STATUS_ORDINARY) {
    return false;
  }
  // set #fields
  my_rec_offs_set_n_fields(offsets, (ulint)table->fields_count);

  const unsigned char* nulls = (const unsigned char*)rec - (REC_N_NEW_EXTRA_BYTES + 1);
  const unsigned char* lens  = nulls - ((table->n_nullable + 7) / 8);

  ulint offs = 0;
  ulint null_mask = 1;

  for (ulint i = 0; i < (ulint)table->fields_count; i++) {
    field_def_t* fld = &table->fields[i];
    bool is_null = false;

    if (fld->can_be_null) {
      if (null_mask == 0) {
        nulls--;
        null_mask = 1;
      }
      if ((*nulls & null_mask) != 0) {
        is_null = true;
      }
      null_mask <<= 1;
    }

    ulint len_val;
    if (is_null) {
      len_val = offs | REC_OFFS_SQL_NULL;
    } else {
      if (fld->fixed_length == 0) {
        ulint lenbyte = *lens--;
        if (fld->max_length > 255
            || fld->type == FT_BLOB
            || fld->type == FT_TEXT) {
          if (lenbyte & 0x80) {
            lenbyte <<= 8;
            lenbyte |= *lens--;
            offs += (lenbyte & 0x3fff);
            if (lenbyte & 0x4000) {
              len_val = offs | REC_OFFS_EXTERNAL;
              goto store_len;
            } else {
              len_val = offs;
              goto store_len;
            }
          }
        }
        offs += lenbyte;
        len_val = offs;
      } else {
        offs += (ulint)fld->fixed_length;
        len_val = offs;
      }
    }
store_len:
    offs &= 0xffff;
    ulint diff = (ulint)((const unsigned char*)rec + offs - (const unsigned char*)page);
    if (diff > (ulint)UNIV_PAGE_SIZE) {
      printf("Invalid offset => field %lu => %lu\n",
             (unsigned long)i, (unsigned long)offs);
      return false;
    }
    offsets[i+1] = len_val;
  }
  return true;
}

/** check_for_a_record() => basic validity check. */
bool check_for_a_record(page_t *page, rec_t *rec, table_def_t *table, ulint *offsets)
{
  ulint offset_in_page = (ulint)((const unsigned char*)rec - (const unsigned char*)page);
  ulint min_hdr = (ulint)(table->min_rec_header_len + record_extra_bytes);
  if (offset_in_page < min_hdr) {
    return false;
  }

  if (!ibrec_init_offsets_new(page, rec, table, offsets)) {
    return false;
  }

  ulint data_sz = my_rec_offs_data_size(offsets);
  if (data_sz > (ulint)table->data_max_size) {
    printf("DATA_SIZE=FAIL(%lu > %ld)\n",
           (unsigned long)data_sz, (long)table->data_max_size);
    return false;
  }
  if (data_sz < (ulint)table->data_min_size) {
    printf("DATA_SIZE=FAIL(%lu < %d)\n",
           (unsigned long)data_sz, table->data_min_size);
    return false;
  }

  if (!check_fields_sizes(rec, table, offsets)) {
    return false;
  }

  return true;
}

// global so we can print header once
static bool g_printed_header = false;

/** process_ibrec() => print columns in pipe-separated format. */
ulint process_ibrec(page_t *page, rec_t *rec, table_def_t *table, ulint *offsets, bool hex)
{
  (void)page; // not used here

  // Print header once:
  if (!g_printed_header) {
    // cast to ulint to avoid sign-compare warning
    for (ulint i = 0; i < (ulint)table->fields_count; i++) {
      printf("%s", table->fields[i].name);
      if (i < (ulint)(table->fields_count - 1)) {
        printf("|");
      }
    }
    printf("\n");
    g_printed_header = true;
  }

  // Print each column
  ulint data_size = my_rec_offs_data_size(offsets);

  for (ulint i = 0; i < (ulint)table->fields_count; i++) {
    ulint field_len;
    const unsigned char* field_ptr = my_rec_get_nth_field(rec, offsets, i, &field_len);

    if (field_len == UNIV_SQL_NULL) {
      // print "NULL"
      printf("NULL");
    } else {
      // naive approach: print as text 
      // (for real usage, interpret as int/binary if needed)
      printf("%.*s", (int)field_len, (const char*)field_ptr);
    }

    if (i < (ulint)(table->fields_count - 1)) {
      printf("|");
    }
  }
  printf("\n");

  return data_size;
}
