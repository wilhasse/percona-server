// ha_cslog.h
#ifndef HA_CSLOG_INCLUDED
#define HA_CSLOG_INCLUDED

/* Standard C++ includes */
#include <string>

/* MySQL includes */
#include "my_base.h"          /* ha_rows */
#include "handler.h"          /* handler */
#include "thr_lock.h"        /* THR_LOCK, THR_LOCK_DATA */
#include "ha_rocksdb.h"

class cslog_share : public Handler_share {
public:
    THR_LOCK lock;
    cslog_share() { thr_lock_init(&lock); }
    ~cslog_share() { thr_lock_delete(&lock); }
};

class ha_cslog: public handler {
private:
    THR_LOCK_DATA lock;      
    cslog_share *share;    
    myrocks::ha_rocksdb* rocksdb_handler;

    cslog_share* get_share();
    int init_cslog();
    int close_cslog();

public:
    ha_cslog(handlerton *hton, TABLE_SHARE *table_arg);
    ~ha_cslog();

    // Required abstract method implementations
    Table_flags table_flags() const override;
    const char *table_type() const override { return "CSLOG"; }
    
    // Key related methods
    uint max_supported_keys() const override;
    uint max_supported_key_length() const override;
    uint max_supported_key_parts() const override;
    ulong index_flags(uint idx, uint part, bool all_parts) const override;

    // Table scanning methods
    int rnd_pos(uchar *buf, uchar *pos) override;
    void position(const uchar *record) override;
    int open(const char *name, int mode, uint test_if_locked, const dd::Table *tab_def) override;
    int close() override;
    int rnd_init(bool scan) override;
    int rnd_next(uchar *buf) override;
    int rnd_end() override;

    // Index methods
    int index_init(uint idx, bool sorted) override;
    int index_end() override;
    int index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map,
                      enum ha_rkey_function find_flag) override;
    int index_next(uchar *buf) override;
    int index_prev(uchar *buf) override;
    int index_first(uchar *buf) override;
    int index_last(uchar *buf) override;

    // DML operations
    int write_row(uchar *buf) override;
    int delete_row(const uchar *buf) override;
    int update_row(const uchar *old_data, uchar *new_data) override;

    // Table operations
    int create(const char *name, TABLE *table_arg, HA_CREATE_INFO *create_info,
              dd::Table *table_def) override;
    int delete_table(const char *name, const dd::Table *table_def) override;

    // Transaction/locking methods
    THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                              enum thr_lock_type lock_type) override;
    int external_lock(THD *thd, int lock_type) override;

    // Information methods
    int info(uint flag) override;
    ha_rows records_in_range(uint inx, key_range *min_key, key_range *max_key) override;
};

#endif