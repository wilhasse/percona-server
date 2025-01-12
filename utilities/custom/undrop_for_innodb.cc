/**
 * undrop_for_innodb.cc
 *
 * Code extracted from undrop_for_innodb.cc.
 * It redefines local "rec_offs_nth_size()"" and "rec_offs_nth_extern()" 
 * so they do NOT conflict with MySQL's built-in versions. 
 * We rely on a classic "undrop" approach:
 *
 *   offsets[0] = number of fields,
 *   offsets[i+1] = offset bits, etc.
 *
 * Hence we must undef the MySQL macros and define our own.
 */
#include <cstdio>      // for printf
#include <cstdint>     // for uint64_t, etc.
#include <cstring>     // for memset, etc.

// MySQL / InnoDB includes that define "dict_index_t*" versions of rec_offs_*:
#include "tables_dict.h"   // your table_def_t, field_def_t definitions
#include "univ.i"          // UNIV_PAGE_SIZE, etc.
#include "page0page.h"     // page_is_comp, PAGE_HEADER, etc.
#include "rem0rec.h"       // rec_t, rec_get_status, etc.

// --------------------------------------------------------------------
// Undefine MySQL’s rec_offs_* so we can define our own “undrop” versions
#ifdef rec_offs_n_fields
#undef rec_offs_n_fields
#endif
#ifdef rec_offs_base
#undef rec_offs_base
#endif
#ifdef rec_offs_set_n_fields
#undef rec_offs_set_n_fields
#endif
#ifdef rec_offs_data_size
#undef rec_offs_data_size
#endif
#ifdef rec_offs_nth_size
#undef rec_offs_nth_size
#endif
#ifdef rec_offs_nth_extern
#undef rec_offs_nth_extern
#endif

// --------------------------------------------------------------------
// Provide local “undrop” macros/inline functions for “offsets” array

#ifndef UNIV_SQL_NULL
#define UNIV_SQL_NULL 0xFFFFFFFF
#endif

#ifndef REC_OFFS_SQL_NULL
#define REC_OFFS_SQL_NULL     0x80000000UL
#endif

#ifndef REC_OFFS_EXTERNAL
#define REC_OFFS_EXTERNAL     0x40000000UL
#endif

/**
 * In the older “undrop” approach, the offsets array format is:
 *
 *   offsets[0] = number of fields
 *   offsets[i+1] = offset bits for the i-th field
 *
 * This differs from MySQL 8.0's official format that stores "n_alloc"
 * at offsets[0].
 *
 * We'll define "my_rec_offs_*" so we don't conflict with MySQL code.
 */

// 1) Number of fields is stored in offsets[0]
inline ulint my_rec_offs_n_fields(const ulint* offsets) {
  return offsets[0];
}

// 2) Offsets “base” just returns the pointer
inline ulint* my_rec_offs_base(ulint* offsets) {
  return offsets;
}

// 3) Setting number of fields
inline void my_rec_offs_set_n_fields(ulint* offsets, ulint n) {
  offsets[0] = n;
}

// 4) “nth_size” logic for the “undrop” style
//    end = offsets[i+1] & ~(SQL_NULL|EXTERNAL)
//    start = offsets[i] & ~(SQL_NULL|EXTERNAL)
//    if end < start => return UNIV_SQL_NULL
//    else => end - start
inline ulint my_rec_offs_nth_size(const ulint* offsets, ulint i) {
  ulint end   = (offsets[i+1] & ~(REC_OFFS_SQL_NULL | REC_OFFS_EXTERNAL));
  ulint start = (offsets[i]   & ~(REC_OFFS_SQL_NULL | REC_OFFS_EXTERNAL));
  if (end < start) {
    return UNIV_SQL_NULL;
  }
  return (end - start);
}

// 5) “nth_extern” logic => is the EXTERNAL bit set for i-th field
inline bool my_rec_offs_nth_extern(const ulint* offsets, ulint i) {
  return (offsets[i+1] & REC_OFFS_EXTERNAL) != 0;
}

/**
 * In many “undrop” variants, data_size = offsets[n_fields] - offsets[0].
 * But do what your code expects. For example:
 *
 * data_size = 
 *   (offsets[n_fields] & ~(SQL_NULL|EXTERNAL))
 *   - (offsets[0] & ~(SQL_NULL|EXTERNAL))   // or offsets[1], depending on your usage
 */
inline ulint my_rec_offs_data_size(const ulint* offsets) {
  ulint n = offsets[0]; // #fields
  // The last offset is offsets[n], the first offset is offsets[0 or 1].
  // We'll do an example: end = offsets[n], start=offsets[1].
  ulint end   = (offsets[n] & ~(REC_OFFS_SQL_NULL | REC_OFFS_EXTERNAL));
  ulint start = (offsets[1] & ~(REC_OFFS_SQL_NULL | REC_OFFS_EXTERNAL));
  if (end < start) return 0;
  return end - start;
}

// --------------------------------------------------------------------
// Now define our usage, referencing the “my_rec_offs_*” functions:

inline bool check_fields_sizes(rec_t* rec, table_def_t* table, ulint* offsets)
{
  ulint n = my_rec_offs_n_fields(offsets);
  printf("\n[undrop] Checking field lengths for a row (%s):", table->name);
  printf("  OFFSETS: ");

  // Just debug printing
  {
    ulint prev_offset = 0;
    for (ulint idx = 0; idx < n; idx++) {
      ulint curr_offset = my_rec_offs_base(offsets)[idx];
      printf("%lu (+%lu); ",
             (unsigned long)curr_offset,
             (unsigned long)(curr_offset - prev_offset));
      prev_offset = curr_offset;
    }
  }

  // For each “real” column
  for (ulint i = 0; i < (ulint)table->fields_count; i++) {
    ulint len = my_rec_offs_nth_size(offsets, i);
    printf("\n - field %s(%lu):", table->fields[i].name, (unsigned long)len);

    // If field is null
    if (len == UNIV_SQL_NULL) {
      if (table->fields[i].can_be_null) continue;
      printf("  => can't be NULL!\n");
      return false;
    }

    // If fixed length
    if (table->fields[i].fixed_length != 0) {
      ulint fixlen = (ulint) table->fields[i].fixed_length;
      if (len == fixlen || (len == 0 && table->fields[i].can_be_null)) {
        continue;
      }
      printf("Invalid fixed length => got %lu, expected %u!\n",
             (unsigned long)len, table->fields[i].fixed_length);
      return false;
    }

    // Check if externally stored
    if (my_rec_offs_nth_extern(offsets, i)) {
      printf("\nEXTERNALLY STORED => field %lu\n", (unsigned long)i);
      if (table->fields[i].type != FT_TEXT && table->fields[i].type != FT_BLOB) {
        printf("Invalid external data!\n");
        return false;
      }
    }

    // Range checks
    ulint minL = (ulint)table->fields[i].min_length;
    ulint maxL = (ulint)table->fields[i].max_length;
    if (len < minL || len > maxL) {
      printf("Length out of range => %lu not in [%u..%u]\n",
             (unsigned long)len,
             table->fields[i].min_length,
             table->fields[i].max_length);
      return false;
    }
    printf(" OK!");
  }

  printf("\n");
  return true;
}

inline bool ibrec_init_offsets_new(page_t* page,
                                   rec_t* rec,
                                   table_def_t* table,
                                   ulint* offsets)
{
  ulint i = 0;
  ulint offs = 0;
  const byte* nulls;
  const byte* lens;
  ulint null_mask = 1;
  ulint status = rec_get_status(rec);

  if (status != REC_STATUS_ORDINARY) return false;

  // In older “undrop” => offsets[0] = #fields
  my_rec_offs_base(offsets)[0] = 0;  // 0 initially
  my_rec_offs_set_n_fields(offsets, (ulint)table->fields_count);

  // Typically for COMPACT => we read null bits from 
  // rec - (REC_N_NEW_EXTRA_BYTES + 1)
  nulls = (const byte*)rec - (REC_N_NEW_EXTRA_BYTES + 1);
  lens  = nulls - ((table->n_nullable + 7) / 8);

  for (i = 0; i < (ulint)table->fields_count; i++) {
    ulint len;
    field_def_t* fld = &table->fields[i];

    if (fld->can_be_null) {
      if (null_mask == 0) {
        nulls--;
        null_mask = 1;
      }
      if ((*nulls & null_mask) != 0) {
        null_mask <<= 1;
        len = offs | REC_OFFS_SQL_NULL;
        goto store_len;
      }
      null_mask <<= 1;
    }

    if (fld->fixed_length == 0) {
      // var-len
      len = *lens--;
      if (fld->max_length > 255
          || fld->type == FT_BLOB
          || fld->type == FT_TEXT) {
        if (len & 0x80) {
          len <<= 8;
          len |= *lens--;
          offs += (len & 0x3fff);
          if (len & 0x4000) {
            len = offs | REC_OFFS_EXTERNAL;
          } else {
            len = offs;
          }
          goto store_len;
        }
      }
      offs += len;
      len = offs;
    } else {
      // fixed
      offs += (ulint)fld->fixed_length;
      len = offs;
    }
store_len:
    offs &= 0xffff;

    ulint diff = (ulint)((const byte*)rec + offs - (const byte*)page);
    if (diff > (ulint)UNIV_PAGE_SIZE) {
      printf("Offset overflow => field %lu => %lu\n",
             (unsigned long)i, (unsigned long)offs);
      return false;
    }
    // store => offsets[i+1] = len
    my_rec_offs_base(offsets)[i+1] = len;
  }

  return true;
}

bool check_for_a_record(page_t* page,
                        rec_t* rec,
                        table_def_t* table,
                        ulint* offsets)
{
  ulint offset = (ulint)((const byte*)rec - (const byte*)page);
  ulint min_hdr = (ulint)(table->min_rec_header_len + record_extra_bytes);
  if (offset < min_hdr) {
    return false;
  }
  printf("ORIGIN=OK ");

  int flag = rec_get_deleted_flag(rec, page_is_comp(page));
  printf("DELETED=0x%X ", flag);

  if (!ibrec_init_offsets_new(page, rec, table, offsets)) {
    return false;
  }
  printf("OFFSETS=OK ");

  ulint data_size = my_rec_offs_data_size(offsets);
  if (data_size > (ulint)table->data_max_size) {
    printf("DATA_SIZE=FAIL(%lu > %ld) ",
           (unsigned long)data_size, (long)table->data_max_size);
    return false;
  }
  if (data_size < (ulint)table->data_min_size) {
    printf("DATA_SIZE=FAIL(%lu < %d) ",
           (unsigned long)data_size, table->data_min_size);
    return false;
  }
  printf("DATA_SIZE=OK ");

  if (!check_fields_sizes(rec, table, offsets)) {
    return false;
  }
  printf("FIELD_SIZES=OK ");

  return true;
}

ulint process_ibrec(page_t* page,
                    rec_t* rec,
                    table_def_t* table,
                    ulint* offsets,
                    bool hex)
{
  (void)page;  // if not used
  (void)hex;   // pass to printing if needed
  ulint data_size = my_rec_offs_data_size(offsets);
  printf("process_ibrec => row from table '%s' => data_size=%lu\n",
         table->name, (unsigned long)data_size);

  // In a real usage, you'd loop over each field, call rec_get_nth_field(...),
  // print them, etc.
  return data_size;
}