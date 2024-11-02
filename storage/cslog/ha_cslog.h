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
#include "mysql/components/services/log_builtins.h"

// Define logging levels
#define CSLOG_LEVEL_ERROR 3
#define CSLOG_LEVEL_WARNING 2
#define CSLOG_LEVEL_INFO 1
#define CSLOG_LEVEL_DEBUG 0

// Enable debug logging by default
#ifndef CSLOG_LOG_LEVEL
#define CSLOG_LOG_LEVEL CSLOG_LEVEL_DEBUG
#endif

// Logging macros
#define CSLOG_DEBUG(format, ...) \
    if (CSLOG_LOG_LEVEL <= CSLOG_LEVEL_DEBUG) { \
        LogEvent().type(LOG_TYPE_ERROR) \
                  .prio(WARNING_LEVEL) \
                  .errcode(ER_PARSER_TRACE) \
                  .subsys("Storage/CSLOG") \
                  .verbatim(true) \
                  .msg("CSLOG-DEBUG: " format, ##__VA_ARGS__); \
    }

#define CSLOG_INFO(format, ...) \
    if (CSLOG_LOG_LEVEL <= CSLOG_LEVEL_INFO) { \
        LogEvent().type(LOG_TYPE_ERROR) \
                  .prio(WARNING_LEVEL) \
                  .errcode(ER_PARSER_TRACE) \
                  .subsys("Storage/CSLOG") \
                  .verbatim(true) \
                  .msg("CSLOG-INFO: " format, ##__VA_ARGS__); \
    }

#define CSLOG_WARNING(format, ...) \
    if (CSLOG_LOG_LEVEL <= CSLOG_LEVEL_WARNING) { \
        LogEvent().type(LOG_TYPE_ERROR) \
                  .prio(WARNING_LEVEL) \
                  .errcode(ER_PARSER_TRACE) \
                  .subsys("Storage/CSLOG") \
                  .verbatim(true) \
                  .msg("CSLOG-WARNING: " format, ##__VA_ARGS__); \
    }

#define CSLOG_ERROR(format, ...) \
    if (CSLOG_LOG_LEVEL <= CSLOG_LEVEL_ERROR) { \
        LogEvent().type(LOG_TYPE_ERROR) \
                  .prio(ERROR_LEVEL) \
                  .errcode(ER_PARSER_TRACE) \
                  .subsys("Storage/CSLOG") \
                  .verbatim(true) \
                  .msg("CSLOG-ERROR: " format, ##__VA_ARGS__); \
    }

class ha_heap;

class cslog_share : public Handler_share {
public:
    THR_LOCK lock;
    cslog_share() { thr_lock_init(&lock); }
    ~cslog_share() { thr_lock_delete(&lock); }
};

class ha_cslog: public handler {
private:
    THR_LOCK_DATA lock;      
    cslog_share *share = nullptr;
    ha_heap* memory_handler;    
    myrocks::ha_rocksdb* rocksdb_handler;
    bool m_pk_can_be_decoded;

    cslog_share* get_share();
    int init_cslog();
    int close_cslog();

public:
    ha_cslog(handlerton *hton, TABLE_SHARE *table_arg);
    ~ha_cslog();

    // Required abstract method implementations
    ulonglong table_flags() const override;
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