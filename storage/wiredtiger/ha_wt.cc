/* 
  WiredTiger Interface for MySQL
*/

#include "ha_wt.h"
#include "my_dbug.h"
#include "mysql/plugin.h"
#include "sql/sql_class.h"
#include "sql/field.h"
#include "sql/sql_plugin.h"
#include "typelib.h"
#include "sql/table.h"
#include "my_base.h"
#include "wt_man.cc"

#include <wiredtiger.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <unistd.h> 

handlerton *wt_hton = nullptr;

static handler *wt_create_handler(handlerton *hton, TABLE_SHARE *table,
                                       bool partitioned, MEM_ROOT *mem_root);

/* Interface to mysqld, to check system tables supported by SE */
static bool wt_is_supported_system_table(const char *db,
                                           const char *table_name,
                                           bool is_sql_layer_system_table);

wt_share::wt_share() { thr_lock_init(&lock); }

/**
  wt init function
*/
static int wt_init_func(void *p) {
  DBUG_TRACE;

  wt_hton = (handlerton *)p;
  wt_hton->state = SHOW_OPTION_YES;
  wt_hton->create = wt_create_handler;
  wt_hton->flags = HTON_CAN_RECREATE;
  wt_hton->is_supported_system_table = wt_is_supported_system_table;

  // Initialize the WiredTiger connection
  if (WTConnectionManager::getConnection() == nullptr) {
      return 1;  // Initialization failed
  }

  return 0;
}

/**
  wt deinit
*/
static int wt_deinit_func(void* p [[maybe_unused]]) {
    DBUG_TRACE;

    assert(p);
    
    // Close the WiredTiger connection
    WTConnectionManager::closeConnection();

    return 0;
}

/**
  wt create class
*/
ha_wt::ha_wt(handlerton *hton, TABLE_SHARE *table_arg)
  : handler(hton, table_arg), conn(nullptr), session(nullptr), cursor(nullptr) {

}

/**
  wt destructor class
*/
ha_wt::~ha_wt() {

    close();
}

int ha_wt::close() {

  if (cursor) {
      cursor->close(cursor);
      cursor = nullptr;
  }
  if (session) {
      session->close(session, NULL);
      session = nullptr;
  }
  if (conn) {
      conn->close(conn, NULL);
      conn = nullptr;
  }
  return 0;
}

/**
  Open a table
*/
int ha_wt::open(const char *name, int mode, uint test_if_locked, const dd::Table *tab_def) {
  
  DBUG_TRACE;
  
  WT_CONNECTION* conn = WTConnectionManager::getConnection();
  if (conn == nullptr) {
      return HA_ERR_INTERNAL_ERROR;
  }

  int ret = conn->open_session(conn, NULL, NULL, &session);
  if (ret != 0) {
      // Handle error
      return HA_ERR_INTERNAL_ERROR;
  }
        
  uri = "table:" + std::string(name);

  // Create the table if it doesn't exist
  ret = session->create(session, uri.c_str(), "key_format=i,value_format=u");
  if (ret != 0 && ret != EEXIST) {
      my_printf_error(ER_UNKNOWN_ERROR, "WiredTiger table creation failed: %s", MYF(0), wiredtiger_strerror(ret));
      return HA_ERR_INTERNAL_ERROR;
  }

  // Open the cursor
  ret = session->open_cursor(session, uri.c_str(), NULL, NULL, &cursor);
  if (ret != 0) {
      my_printf_error(ER_UNKNOWN_ERROR, "WiredTiger cursor open failed: %s", MYF(0), wiredtiger_strerror(ret));
      return HA_ERR_INTERNAL_ERROR;
  }
    
  if (!(share = get_share())) return 1;
  thr_lock_data_init(&share->lock, &lock, NULL);

  return 0;
}

/**
  Insert a Row
*/
int ha_wt::write_row(uchar *buf) {
    DBUG_TRACE;
    DBUG_ENTER("ha_wt::write_row");

    int ret;

    // Set the record buffer to point to the row data
    table->record[0] = buf;

    // Mark all columns as needed for reading
    bitmap_set_all(table->read_set);

    // Get the primary key field (assuming it's an integer)
    Field *key_field = table->field[0];
    int32_t key_value = (int32_t) key_field->val_int();

    // Serialize the rest of the fields into a value buffer
    std::vector<char> value_buffer;
    for (uint i = 1; i < table->s->fields; i++) {
        Field *field = table->field[i];
        String field_value;
        field->val_str(&field_value);

        // Append field length and data to value_buffer
        uint32_t len = field_value.length();
        value_buffer.insert(value_buffer.end(), (char*)&len, (char*)&len + sizeof(len));
        value_buffer.insert(value_buffer.end(), field_value.ptr(), field_value.ptr() + len);
    }

    // Set the key and value for the WiredTiger cursor
    cursor->set_key(cursor, key_value);

    WT_ITEM value_item;
    value_item.data = value_buffer.data();
    value_item.size = value_buffer.size();
    cursor->set_value(cursor, &value_item);

    // Insert the record
    ret = cursor->insert(cursor);
    if (ret != 0) {
        my_printf_error(ER_UNKNOWN_ERROR, "WiredTiger insert failed: %s", MYF(0),
                        wiredtiger_strerror(ret));
        DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }

    DBUG_RETURN(0);
}

/**
  wt of simple lock controls.
*/
wt_share *ha_wt::get_share() {
  wt_share *tmp_share = nullptr;

  DBUG_TRACE;

  lock_shared_ha_data();
  if (!(tmp_share = static_cast<wt_share *>(get_ha_share_ptr()))) {
    tmp_share = new wt_share;
    if (!tmp_share) goto err;

    set_ha_share_ptr(static_cast<Handler_share *>(tmp_share));
  }
err:
  unlock_shared_ha_data();
  return tmp_share;
}

/**
  Updates a row.
  TODO
*/
int ha_wt::update_row(const uchar *, uchar *) {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

/**
  Deletes a row.
  TODO
*/
int ha_wt::delete_row(const uchar *) {
  DBUG_TRACE;
  DBUG_PRINT("info", ("Writing row to ha_wt table"));

  return 0;
}

/**
  Read map
*/
int ha_wt::index_read_map(uchar *, const uchar *, key_part_map,
                               enum ha_rkey_function) {
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
  Next Index
*/
int ha_wt::index_next(uchar *) {
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
  Previous Index
*/
int ha_wt::index_prev(uchar *) {
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
  First Key Index
*/
int ha_wt::index_first(uchar *) {
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
 Last Key Index
*/
int ha_wt::index_last(uchar *) {
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
  Table Scan
*/
int ha_wt::rnd_init(bool) {
  DBUG_TRACE;
  return 0;
}

int ha_wt::rnd_end() {
  DBUG_TRACE;
  return 0;
}

/**
  Each row in a table scan
*/
int ha_wt::rnd_next(uchar *) {
  //int rc;
  DBUG_TRACE;
  //rc = HA_ERR_END_OF_FILE;
  return 0;
}

/**
  Position in rnd_next
*/
void ha_wt::position(const uchar *) { DBUG_TRACE; }

/**
  Position
*/
int ha_wt::rnd_pos(uchar *, uchar *) {
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
  Info for the optimizer
*/
int ha_wt::info(uint) {
  DBUG_TRACE;
  return 0;
}

/**
  Extra
*/
int ha_wt::extra(enum ha_extra_function) {
  DBUG_TRACE;
  return 0;
}

/**
  Delete all rows
*/
int ha_wt::delete_all_rows() {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

/**
  Lock on the table
*/
int ha_wt::external_lock(THD *, int) {
  DBUG_TRACE;
  return 0;
}

static handler *wt_create_handler(handlerton *hton, TABLE_SHARE *table,
                                       bool, MEM_ROOT *mem_root) {
  return new (mem_root) ha_wt(hton, table);
}

/**
  Store Lock
*/
THR_LOCK_DATA **ha_wt::store_lock(THD *, THR_LOCK_DATA **to,
                                       enum thr_lock_type lock_type) {
  if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK) lock.type = lock_type;
  *to++ = &lock;
  return to;
}

/**
  Delete table
*/
int ha_wt::delete_table(const char *, const dd::Table *) {
  DBUG_TRACE;
  /* This is not implemented but we want someone to be able that it works. */
  return 0;
}

/**
  Rename table
*/
int ha_wt::rename_table(const char *, const char *, const dd::Table *,
                             dd::Table *) {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

/**
  Recors in range
*/
ha_rows ha_wt::records_in_range(uint, key_range *, key_range *) {
  DBUG_TRACE;
  return 10;  // low number to force index usage
}

static MYSQL_THDVAR_STR(last_create_thdvar, PLUGIN_VAR_MEMALLOC, nullptr,
                        nullptr, nullptr, nullptr);

static MYSQL_THDVAR_UINT(create_count_thdvar, 0, nullptr, nullptr, nullptr, 0,
                         0, 1000, 0);

/**
  Create database
*/

int ha_wt::create(const char *name, TABLE *, HA_CREATE_INFO *,
                       dd::Table *) {
  DBUG_TRACE;
  /*
    This is not implemented but we want someone to be able to see that it
    works.
  */

  /*
    It's just an wt of THDVAR_SET() usage below.
  */
  THD *thd = ha_thd();
  char *buf = (char *)my_malloc(PSI_NOT_INSTRUMENTED, SHOW_VAR_FUNC_BUFF_SIZE,
                                MYF(MY_FAE));
  snprintf(buf, SHOW_VAR_FUNC_BUFF_SIZE, "Last creation '%s'", name);
  THDVAR_SET(thd, last_create_thdvar, buf);
  my_free(buf);

  uint count = THDVAR(thd, create_count_thdvar) + 1;
  THDVAR_SET(thd, create_count_thdvar, &count);

  return 0;
}

/*
  List of all system tables specific to the SE.
*/
static st_handler_tablename ha_wt_system_tables[] = {
    {(const char *)nullptr, (const char *)nullptr}};

/**
  Check if the given db.tablename is a system table for this SE.
*/
static bool wt_is_supported_system_table(const char *db,
                                              const char *table_name,
                                              bool is_sql_layer_system_table) {
  st_handler_tablename *systab = nullptr;

  // Does this SE support "ALL" SQL layer system tables ?
  if (is_sql_layer_system_table) return false;

  // Check if this is SE layer system tables
  systab = ha_wt_system_tables;
  while (systab && systab->db) {
    if (systab->db == db && strcmp(systab->tablename, table_name) == 0)
      return true;
    systab++;
  }

  return false;
}

struct st_mysql_storage_engine wt_storage_engine = {
    MYSQL_HANDLERTON_INTERFACE_VERSION};

static ulong srv_enum_var = 0;
static ulong srv_ulong_var = 0;
static double srv_double_var = 0;
static int srv_signed_int_var = 0;
static long srv_signed_long_var = 0;
static longlong srv_signed_longlong_var = 0;

const char *wt_enum_var_names[] = {"e1", "e2", NullS};

TYPELIB wt_enum_var_typelib = {array_elements(wt_enum_var_names) - 1,
                            "enum_var_typelib", wt_enum_var_names, nullptr};

static MYSQL_SYSVAR_ENUM(enum_var,                        // name
                         srv_enum_var,                    // varname
                         PLUGIN_VAR_RQCMDARG,             // opt
                         "Sample ENUM system variable.",  // comment
                         nullptr,                         // check
                         nullptr,                         // update
                         0,                               // def
                         &wt_enum_var_typelib);         // typelib

static MYSQL_SYSVAR_ULONG(ulong_var, srv_ulong_var, PLUGIN_VAR_RQCMDARG,
                          "0..1000", nullptr, nullptr, 8, 0, 1000, 0);

static MYSQL_SYSVAR_DOUBLE(double_var, srv_double_var, PLUGIN_VAR_RQCMDARG,
                           "0.500000..1000.500000", nullptr, nullptr, 8.5, 0.5,
                           1000.5,
                           0);  // reserved always 0

static MYSQL_THDVAR_DOUBLE(double_thdvar, PLUGIN_VAR_RQCMDARG,
                           "0.500000..1000.500000", nullptr, nullptr, 8.5, 0.5,
                           1000.5, 0);

static MYSQL_SYSVAR_INT(signed_int_var, srv_signed_int_var, PLUGIN_VAR_RQCMDARG,
                        "INT_MIN..INT_MAX", nullptr, nullptr, -10, INT_MIN,
                        INT_MAX, 0);

static MYSQL_THDVAR_INT(signed_int_thdvar, PLUGIN_VAR_RQCMDARG,
                        "INT_MIN..INT_MAX", nullptr, nullptr, -10, INT_MIN,
                        INT_MAX, 0);

static MYSQL_SYSVAR_LONG(signed_long_var, srv_signed_long_var,
                         PLUGIN_VAR_RQCMDARG, "LONG_MIN..LONG_MAX", nullptr,
                         nullptr, -10, LONG_MIN, LONG_MAX, 0);

static MYSQL_THDVAR_LONG(signed_long_thdvar, PLUGIN_VAR_RQCMDARG,
                         "LONG_MIN..LONG_MAX", nullptr, nullptr, -10, LONG_MIN,
                         LONG_MAX, 0);

static MYSQL_SYSVAR_LONGLONG(signed_longlong_var, srv_signed_longlong_var,
                             PLUGIN_VAR_RQCMDARG, "LLONG_MIN..LLONG_MAX",
                             nullptr, nullptr, -10, LLONG_MIN, LLONG_MAX, 0);

static MYSQL_THDVAR_LONGLONG(signed_longlong_thdvar, PLUGIN_VAR_RQCMDARG,
                             "LLONG_MIN..LLONG_MAX", nullptr, nullptr, -10,
                             LLONG_MIN, LLONG_MAX, 0);

static SYS_VAR *wt_system_variables[] = {
    MYSQL_SYSVAR(enum_var),
    MYSQL_SYSVAR(ulong_var),
    MYSQL_SYSVAR(double_var),
    MYSQL_SYSVAR(double_thdvar),
    MYSQL_SYSVAR(last_create_thdvar),
    MYSQL_SYSVAR(create_count_thdvar),
    MYSQL_SYSVAR(signed_int_var),
    MYSQL_SYSVAR(signed_int_thdvar),
    MYSQL_SYSVAR(signed_long_var),
    MYSQL_SYSVAR(signed_long_thdvar),
    MYSQL_SYSVAR(signed_longlong_var),
    MYSQL_SYSVAR(signed_longlong_thdvar),
    nullptr};

// this is an wt of SHOW_FUNC
static int show_func_wt(MYSQL_THD, SHOW_VAR *var, char *buf) {
  var->type = SHOW_CHAR;
  var->value = buf;  // it's of SHOW_VAR_FUNC_BUFF_SIZE bytes
  snprintf(buf, SHOW_VAR_FUNC_BUFF_SIZE,
           "enum_var is %lu, ulong_var is %lu, "
           "double_var is %f, signed_int_var is %d, "
           "signed_long_var is %ld, signed_longlong_var is %lld",
           srv_enum_var, srv_ulong_var, srv_double_var, srv_signed_int_var,
           srv_signed_long_var, srv_signed_longlong_var);
  return 0;
}

struct wt_vars_t {
  ulong var1;
  double var2;
  char var3[64];
  bool var4;
  bool var5;
  ulong var6;
};

wt_vars_t wt_vars = {100, 20.01, "three hundred", true, false, 8250};

static SHOW_VAR show_status_wt[] = {
    {"var1", (char *)&wt_vars.var1, SHOW_LONG, SHOW_SCOPE_GLOBAL},
    {"var2", (char *)&wt_vars.var2, SHOW_DOUBLE, SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_UNDEF,
     SHOW_SCOPE_UNDEF}  // null terminator required
};

static SHOW_VAR show_array_wt[] = {
    {"array", (char *)show_status_wt, SHOW_ARRAY, SHOW_SCOPE_GLOBAL},
    {"var3", (char *)&wt_vars.var3, SHOW_CHAR, SHOW_SCOPE_GLOBAL},
    {"var4", (char *)&wt_vars.var4, SHOW_BOOL, SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_UNDEF, SHOW_SCOPE_UNDEF}};

static SHOW_VAR func_status[] = {
    {"wt_func_wt", (char *)show_func_wt, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"wt_status_var5", (char *)&wt_vars.var5, SHOW_BOOL,
     SHOW_SCOPE_GLOBAL},
    {"wt_status_var6", (char *)&wt_vars.var6, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {"wt_status", (char *)show_array_wt, SHOW_ARRAY,
     SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_UNDEF, SHOW_SCOPE_UNDEF}};

mysql_declare_plugin(wt){
    MYSQL_STORAGE_ENGINE_PLUGIN,
    &wt_storage_engine,
    "WIREDTIGER",
    PLUGIN_AUTHOR_ORACLE,
    "WiredTiger storage engine",
    PLUGIN_LICENSE_GPL,
    wt_init_func,   /* Plugin Init */
    nullptr,             /* Plugin check uninstall */
    wt_deinit_func, /* Plugin Deinit */
    0x0001 /* 0.1 */,
    func_status,              /* status variables */
    wt_system_variables, /* system variables */
    nullptr,                  /* config options */
    0,                        /* flags */
} mysql_declare_plugin_end;
