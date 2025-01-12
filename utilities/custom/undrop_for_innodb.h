#ifndef UNDROP_FOR_INNODB_H
#define UNDROP_FOR_INNODB_H

#include "page0page.h"
#include "tables_dict.h"

bool check_for_a_record(page_t *page, rec_t *rec, table_def_t *table, ulint *offsets);
ulint process_ibrec(page_t *page, rec_t *rec, table_def_t *table, ulint *offsets, bool hex);

#endif // UNDROP_FOR_INNODB_H
