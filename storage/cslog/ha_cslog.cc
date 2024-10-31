// ha_cslog.cc
#include "ha_cslog.h"
#include "my_dbug.h"
#include "mysql/plugin.h"
#include "sql/sql_class.h"
#include "sql/field.h"
#include "sql/sql_plugin.h"
#include "typelib.h"
#include "sql/table.h"
#include "my_base.h"
#include <dlfcn.h>
#include "ha_heap.h"
#include <mysql/components/services/log_builtins.h>

#define CSLOG_MAX_KEY 64  // Maximum number of keys allowed
#define CSLOG_MAX_KEY_LENGTH 1024  // Maximum key length
#define CSLOG_MAX_KEY_PARTS 16     // Maximum parts in a composite key

static handlerton *cslog_hton = nullptr;

static handler* cslog_create_handler(handlerton *hton, TABLE_SHARE *table, 
                                   bool partitioned, MEM_ROOT *mem_root) {
    return new (mem_root) ha_cslog(hton, table);
}

// And update table_flags() to match RocksDB's capabilities:
ulonglong ha_cslog::table_flags() const {
    DBUG_ENTER_FUNC();

    ulonglong flags = (HA_BINLOG_ROW_CAPABLE | 
                      HA_BINLOG_STMT_CAPABLE |
                      HA_CAN_INDEX_BLOBS |
                      HA_PRIMARY_KEY_REQUIRED_FOR_POSITION | 
                      HA_NULL_IN_KEY |
                      HA_PARTIAL_COLUMN_READ);

    // Add HA_PRIMARY_KEY_IN_READ_INDEX if RocksDB can decode the key
    if (rocksdb_handler && m_pk_can_be_decoded) {
        flags |= HA_PRIMARY_KEY_IN_READ_INDEX;
    }

    // Get additional flags from RocksDB if available
    if (rocksdb_handler) {
        flags |= rocksdb_handler->table_flags();
    }

    DBUG_RETURN(flags);
}

ha_cslog::ha_cslog(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg),
      share(nullptr),
      m_pk_can_be_decoded(true) {
    try {
        // rocksdb
        rocksdb_handler = new myrocks::ha_rocksdb(hton, table_arg);

        // heap
        memory_handler = new (std::nothrow) ha_heap(hton, table_share);

    } catch (const std::exception& e) {
        // Handle exception, possibly by setting rocksdb_handler to nullptr
        rocksdb_handler = nullptr;
    }
}

uint ha_cslog::max_supported_keys() const {
    return CSLOG_MAX_KEY;
}

uint ha_cslog::max_supported_key_length() const {
    return CSLOG_MAX_KEY_LENGTH;
}

uint ha_cslog::max_supported_key_parts() const {
    return CSLOG_MAX_KEY_PARTS;
}

ulong ha_cslog::index_flags(uint idx, uint part, bool all_parts) const {

  if (!table_share) {
    return 0;
  }

  // Basic flags all indexes support
  ulong flags = HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER | HA_READ_RANGE;

  // For primary key
  if (idx == table_share->primary_key) {
    if (m_pk_can_be_decoded) {
      flags |= HA_KEYREAD_ONLY;
    }
    return flags;
  }

  // For other indexes, delegate to RocksDB if available
  if (rocksdb_handler) {
    return rocksdb_handler->index_flags(idx, part, all_parts);
  }

  return flags;  
}

// Update open method to properly handle table opening
int ha_cslog::open(const char *name, int mode, uint test_if_locked, const dd::Table *tab_def) {
    DBUG_ENTER("ha_cslog::open");
    DBUG_PRINT("info", ("Opening table %s", name));
    
    if (!(share = get_share())) {
        DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }
    
    // Set the table pointer in RocksDB handler
    rocksdb_handler->change_table_ptr(table, table_share);
    memory_handler->change_table_ptr(table, table_share);
                
    thr_lock_data_init(&share->lock, &lock, NULL);

    int rc = rocksdb_handler->open(name, mode, test_if_locked, tab_def);
    if (rc) {
        DBUG_RETURN(rc);
    }
   
    rc = memory_handler->open(name, mode, test_if_locked, tab_def);
    if (rc) {
        DBUG_RETURN(rc);
    }

    DBUG_RETURN(0);
}

static int cslog_init_func(void *p) {
    DBUG_TRACE;
    
    // Initialize our handlerton first
    cslog_hton = (handlerton *)p;
    if (!cslog_hton) {
        return 1;
    }
    
    // Check if RocksDB is available
    LEX_CSTRING rocks_name;
    rocks_name.str = "rocksdb";
    rocks_name.length = 7;  // strlen("rocksdb")
    
    plugin_ref plugin = ha_resolve_by_name(nullptr, &rocks_name, false);
    if (!plugin) {
        fprintf(stderr, "CSLOG: RocksDB storage engine must be installed first\n");
        return 1;
    }
    
    cslog_hton->state = SHOW_OPTION_YES;
    cslog_hton->create = cslog_create_handler;
    cslog_hton->flags = HTON_CAN_RECREATE;
    
    return 0;
}

ha_cslog::~ha_cslog() {
    delete rocksdb_handler;
    delete memory_handler;
}

int ha_cslog::init_cslog() {
    fprintf(stderr, "cslog initialized successfully\n");
    return 0;
}

int ha_cslog::close_cslog() {
    return 0;
}

static int cslog_deinit_func(void *p) {
    return 0;
}

int ha_cslog::close() {
    int rc = 0;
    if (rocksdb_handler) {
        rc = rocksdb_handler->close();
    }
    return rc;  
}

int ha_cslog::write_row(uchar *buf) {
    if (!rocksdb_handler) {
        return HA_ERR_INITIALIZATION;
    }
    memory_handler->write_row(buf);
    return rocksdb_handler->write_row(buf);
}

int ha_cslog::rnd_init(bool scan) {
    if (!rocksdb_handler) {
        return HA_ERR_INITIALIZATION;
    }
    return rocksdb_handler->rnd_init(scan);
}

int ha_cslog::rnd_next(uchar *buf) {
    if (!rocksdb_handler) {
        return HA_ERR_INITIALIZATION;
    }
    return rocksdb_handler->rnd_next(buf);
}

int ha_cslog::rnd_end() {
    if (!rocksdb_handler) {
        return HA_ERR_INITIALIZATION;
    }
    return rocksdb_handler->rnd_end();
}

// Add these new implementations to your ha_cslog.cc file:

int ha_cslog::rnd_pos(uchar *buf, uchar *pos) {
    if (!rocksdb_handler) {
        return HA_ERR_INITIALIZATION;
    }
    return rocksdb_handler->rnd_pos(buf, pos);
}

void ha_cslog::position(const uchar *record) {
    if (rocksdb_handler) {
        rocksdb_handler->position(record);
    }
}

// Also modify the get_share() function to use proper casting:
cslog_share* ha_cslog::get_share() {
    cslog_share *tmp_share = nullptr;
    
    DBUG_TRACE;
    
    lock_shared_ha_data();
    tmp_share = static_cast<cslog_share*>(get_ha_share_ptr());
    if (!tmp_share) {
        tmp_share = new cslog_share;
        if (!tmp_share) {
            unlock_shared_ha_data();
            return nullptr;
        }
        set_ha_share_ptr(static_cast<Handler_share*>(tmp_share));
    }
    unlock_shared_ha_data();
    return tmp_share;
}

int ha_cslog::index_init(uint idx, bool sorted) {
    if (!rocksdb_handler) {
        return HA_ERR_INITIALIZATION;
    }
    return rocksdb_handler->index_init(idx, sorted);
}

int ha_cslog::index_end() {
    if (!rocksdb_handler) {
        return HA_ERR_INITIALIZATION;
    }
    return rocksdb_handler->index_end();
}

int ha_cslog::index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map,
                           enum ha_rkey_function find_flag) {
    if (!rocksdb_handler) {
        return HA_ERR_INITIALIZATION;
    }
    return rocksdb_handler->index_read_map(buf, key, keypart_map, find_flag);
}

int ha_cslog::index_next(uchar *buf) {
    if (!rocksdb_handler) {
        return HA_ERR_INITIALIZATION;
    }
    return rocksdb_handler->index_next(buf);
}

int ha_cslog::index_prev(uchar *buf) {
    if (!rocksdb_handler) {
        return HA_ERR_INITIALIZATION;
    }
    return rocksdb_handler->index_prev(buf);
}

int ha_cslog::index_first(uchar *buf) {
    if (!rocksdb_handler) {
        return HA_ERR_INITIALIZATION;
    }
    return rocksdb_handler->index_first(buf);
}

int ha_cslog::index_last(uchar *buf) {
    if (!rocksdb_handler) {
        return HA_ERR_INITIALIZATION;
    }
    return rocksdb_handler->index_last(buf);
}

// Update key statistics
int ha_cslog::info(uint flag) {
    if (!rocksdb_handler) {
        return HA_ERR_INITIALIZATION;
    }
    
    int rc = rocksdb_handler->info(flag);
    if (rc) return rc;

    // If statistics are requested, fill them in
    if (flag & HA_STATUS_TIME) {
        stats.update_time = 0;
    }
    if (flag & HA_STATUS_CONST) {
        stats.max_data_file_length = rocksdb_handler->stats.max_data_file_length;
        stats.create_time = 0;
        stats.block_size = rocksdb_handler->stats.block_size;
    }
    if (flag & HA_STATUS_VARIABLE) {
        stats.data_file_length = rocksdb_handler->stats.data_file_length;
        stats.records = rocksdb_handler->stats.records;
        stats.deleted = rocksdb_handler->stats.deleted;
        stats.mean_rec_length = rocksdb_handler->stats.mean_rec_length;
    }

    return 0;
}

THR_LOCK_DATA **ha_cslog::store_lock(THD *thd, THR_LOCK_DATA **to,
                                    enum thr_lock_type lock_type) {
    if (rocksdb_handler) {
        return rocksdb_handler->store_lock(thd, to, lock_type);
    }
    return to;
}

int ha_cslog::external_lock(THD *thd, int lock_type) {
    if (!rocksdb_handler) {
        return HA_ERR_INITIALIZATION;
    }
    return rocksdb_handler->external_lock(thd, lock_type);
}

ha_rows ha_cslog::records_in_range(uint inx, key_range *min_key, key_range *max_key) {
    if (!rocksdb_handler) {
        return HA_ERR_INITIALIZATION;
    }
    return rocksdb_handler->records_in_range(inx, min_key, max_key);
}

int ha_cslog::delete_table(const char *name, const dd::Table *table_def) {
    if (!rocksdb_handler) {
        return HA_ERR_INITIALIZATION;
    }
    return rocksdb_handler->delete_table(name, table_def);
}

int ha_cslog::create(const char *name, TABLE *table_arg, 
                    HA_CREATE_INFO *create_info, dd::Table *table_def) {
    DBUG_ENTER("ha_cslog::create");
    DBUG_PRINT("info", ("Creating table %s", name));

    if (!rocksdb_handler) {
        LEX_CSTRING rocks_name;
        rocks_name.str = "rocksdb";
        rocks_name.length = 7;
        
        plugin_ref plugin = ha_resolve_by_name(nullptr, &rocks_name, false);
        if (!plugin) {
            DBUG_RETURN(HA_ERR_INITIALIZATION);
        }
        
        handlerton *rocks_hton = plugin_data<handlerton*>(plugin);
        if (!rocks_hton) {
            DBUG_RETURN(HA_ERR_INITIALIZATION);
        }

        try {
            // Create RocksDB handler with the same table share
            rocksdb_handler = new myrocks::ha_rocksdb(rocks_hton, table_arg->s);
            if (!rocksdb_handler) {
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            }
        } catch (const std::exception& e) {
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
    }

    int rc = rocksdb_handler->create(name, table_arg, create_info, table_def);
    DBUG_RETURN(rc);
}

int ha_cslog::delete_row(const uchar *buf) {
    if (!rocksdb_handler) {
        return HA_ERR_INITIALIZATION;
    }
    return rocksdb_handler->delete_row(buf);
}

int ha_cslog::update_row(const uchar *old_data, uchar *new_data) {
    if (!rocksdb_handler) {
        return HA_ERR_INITIALIZATION;
    }
    return rocksdb_handler->update_row(old_data, new_data);
}

// Add this structure before mysql_declare_plugin
struct st_mysql_storage_engine cslog_storage_engine = {
    MYSQL_HANDLERTON_INTERFACE_VERSION
};

mysql_declare_plugin(cslog) {
    MYSQL_STORAGE_ENGINE_PLUGIN,
    &cslog_storage_engine,
    "CSLOG",
    PLUGIN_AUTHOR_ORACLE,
    "CSLOG storage engine",
    PLUGIN_LICENSE_GPL,
    cslog_init_func,
    nullptr,
    cslog_deinit_func,
    0x0001,
    nullptr,
    nullptr,
    nullptr,
    0
} mysql_declare_plugin_end;