#include "ha_cslog.h"
#include "my_dbug.h"
#include "mysql/plugin.h"
#include "sql/sql_class.h"
#include "sql/field.h"
#include "sql/sql_plugin.h"
#include "typelib.h"
#include "sql/table.h"
#include "my_base.h"
#include "sql/handler.h"
#include <dlfcn.h>

 // For logging services
#include <mysql/components/services/log_builtins.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#define DBUG_ON 1

static handlerton *cslog_hton = nullptr;

static handler* cslog_create_handler(handlerton *hton, TABLE_SHARE *table, 
                                   bool partitioned, MEM_ROOT *mem_root) {
    return new (mem_root) ha_cslog(hton, table);
}

ha_cslog::ha_cslog(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg), 
      share(nullptr),
      rocksdb_handler(nullptr) {
}

int ha_cslog::open(const char *name, int mode, uint test_if_locked, const dd::Table *tab_def) {
    DBUG_ENTER("ha_cslog::open");
    DBUG_PRINT("info", ("Opening table %s", name));
    
    if (!(share = get_share())) {
        LogErr(ERROR_LEVEL, ER_UNKNOWN_ERROR, "Failed to get share");
        DBUG_RETURN(1);
    }
    
    thr_lock_data_init(&share->lock, &lock, NULL);

    if (!rocksdb_handler) {
        DBUG_PRINT("info", ("Creating RocksDB handler"));
        
        LEX_CSTRING rocks_name;
        rocks_name.str = "rocksdb";
        rocks_name.length = 7;
        
        plugin_ref plugin = ha_resolve_by_name(nullptr, &rocks_name, false);
        if (!plugin) {
            LogErr(ERROR_LEVEL, ER_UNKNOWN_ERROR, "Failed to resolve RocksDB plugin");
            DBUG_RETURN(HA_ERR_INITIALIZATION);
        }
        
        handlerton *rocks_hton = plugin_data<handlerton*>(plugin);
        if (!rocks_hton) {
            LogErr(ERROR_LEVEL, ER_UNKNOWN_ERROR, "Failed to get RocksDB handlerton");
            DBUG_RETURN(HA_ERR_INITIALIZATION);
        }

        try {
            rocksdb_handler = new myrocks::ha_rocksdb(rocks_hton, table_share);
            if (!rocksdb_handler) {
                LogErr(ERROR_LEVEL, ER_UNKNOWN_ERROR, "Failed to allocate RocksDB handler");
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            }
        } catch (const std::exception& e) {
            LogErr(ERROR_LEVEL, ER_UNKNOWN_ERROR, 
                   "Exception while creating RocksDB handler: %s", e.what());
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
    }

    int rc = rocksdb_handler->open(name, mode, test_if_locked, tab_def);
    if (rc) {
        LogErr(ERROR_LEVEL, ER_UNKNOWN_ERROR, 
               "Failed to open RocksDB handler, error code: %d", rc);
        DBUG_RETURN(rc);
    }

    rc = init_cslog();
    DBUG_PRINT("info", ("init_cslog returned: %d", rc));
    DBUG_RETURN(rc);
}

// ... (keep other methods the same)

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

ulong ha_cslog::index_flags(uint idx, uint part, bool all_parts) const {
    if (!rocksdb_handler) {
        return 0;
    }
    return rocksdb_handler->index_flags(idx, part, all_parts);
}

// Also modify the get_share() function to use proper casting:
cslog_share* ha_cslog::get_share() {
    cslog_share *tmp_share;
    
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

int ha_cslog::info(uint flag) {
    if (!rocksdb_handler) {
        return HA_ERR_INITIALIZATION;
    }
    return rocksdb_handler->info(flag);
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
        // Create a temporary RocksDB handler for table creation
        LEX_CSTRING rocks_name;
        rocks_name.str = "rocksdb";
        rocks_name.length = 7;
        
        plugin_ref plugin = ha_resolve_by_name(nullptr, &rocks_name, false);
        if (!plugin) {
            LogErr(ERROR_LEVEL, ER_UNKNOWN_ERROR, 
                   "Failed to resolve RocksDB plugin during table creation");
            DBUG_RETURN(HA_ERR_INITIALIZATION);
        }
        
        handlerton *rocks_hton = plugin_data<handlerton*>(plugin);
        if (!rocks_hton) {
            LogErr(ERROR_LEVEL, ER_UNKNOWN_ERROR, 
                   "Failed to get RocksDB handlerton during table creation");
            DBUG_RETURN(HA_ERR_INITIALIZATION);
        }

        try {
            rocksdb_handler = new myrocks::ha_rocksdb(rocks_hton, table_share);
        } catch (const std::exception& e) {
            LogErr(ERROR_LEVEL, ER_UNKNOWN_ERROR, 
                   "Exception while creating RocksDB handler for table creation: %s", e.what());
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
    }

    int rc = rocksdb_handler->create(name, table_arg, create_info, table_def);
    DBUG_PRINT("info", ("RocksDB create returned: %d", rc));
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