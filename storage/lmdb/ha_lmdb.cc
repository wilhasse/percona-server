/* 
  LMDB Interface for MySQL

  Using lmdb master last commit Aug 10, 2024
*/

#include "storage/lmdb/ha_lmdb.h"
#include "my_dbug.h"
#include "mysql/plugin.h"
#include "sql/sql_class.h"
#include "sql/field.h"
#include "sql/sql_plugin.h"
#include "typelib.h"
#include "sql/table.h"
#include "my_base.h"

#include <lmdb.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <unistd.h> 

static handler *lmdb_create_handler(handlerton *hton, TABLE_SHARE *table,
                                       bool partitioned, MEM_ROOT *mem_root);

handlerton *lmdb_hton;

/* Interface to mysqld, to check system tables supported by SE */
static bool lmdb_is_supported_system_table(const char *db,
                                           const char *table_name,
                                           bool is_sql_layer_system_table);

Lmdb_share::Lmdb_share() { thr_lock_init(&lock); }

/**
  Lmdb init function
*/
static int lmdb_init_func(void *p) {
  DBUG_TRACE;

  lmdb_hton = (handlerton *)p;
  lmdb_hton->state = SHOW_OPTION_YES;
  lmdb_hton->create = lmdb_create_handler;
  lmdb_hton->flags = HTON_CAN_RECREATE;
  lmdb_hton->is_supported_system_table = lmdb_is_supported_system_table;

  return 0;
}

/**
  Lmdb initialize database
*/
int ha_lmdb::init_lmdb() {

  // Already initialized
  if (env != nullptr) return 0;  

  int rc;
  const char *db_path = "./lmdb_data";
  fprintf(stderr, "LMDB Version: %s\n", MDB_VERSION_STRING);

  // Check if the directory exists
  if (access(db_path, F_OK) == -1) {
    if (mkdir(db_path, 0700) == -1) {
      fprintf(stderr, "Error creating directory %s: %s\n", db_path, strerror(errno));
      return -1;
    }
  }

  rc = mdb_env_create(&env);
  if (rc != 0) {
    fprintf(stderr, "mdb_env_create failed, error %d %s\n", rc, mdb_strerror(rc));
    return rc;
  }

  rc = mdb_env_set_mapsize(env, 53687091200); // 10MiB
  if (rc != 0) {
    fprintf(stderr, "mdb_env_set_mapsize failed, error %d %s\n", rc, mdb_strerror(rc));
    mdb_env_close(env);
    env = nullptr;
    return rc;
  }

  rc = mdb_env_open(env, db_path, 0, 0664);
  if (rc != 0) {
    fprintf(stderr, "mdb_env_open failed, error %d %s\n", rc, mdb_strerror(rc));
    mdb_env_close(env);
    env = nullptr;
    return rc;
  }

  MDB_txn *txn;
  rc = mdb_txn_begin(env, NULL, 0, &txn);
  if (rc != 0) {
    fprintf(stderr, "mdb_txn_begin failed, error %d %s\n", rc, mdb_strerror(rc));
    mdb_env_close(env);
    env = nullptr;
    return rc;
  }

  rc = mdb_dbi_open(txn, NULL, 0, &dbi);
  if (rc != 0) {
    fprintf(stderr, "mdb_dbi_open failed, error %d %s\n", rc, mdb_strerror(rc));
    mdb_txn_abort(txn);
    mdb_env_close(env);
    env = nullptr;
    return rc;
  }

  rc = mdb_txn_commit(txn);
  if (rc != 0) {
    fprintf(stderr, "mdb_txn_commit failed, error %d %s\n", rc, mdb_strerror(rc));
    mdb_env_close(env);
    env = nullptr;
    return rc;
  }

  fprintf(stderr, "LMDB initialized successfully\n");
  return 0;
}

/**
  Lmdb close database
*/
int ha_lmdb::close_lmdb() {
  mdb_dbi_close(env, dbi);
  mdb_env_close(env);
  return 0;
}

/**
  Lmdb deinit
*/
static int lmdb_deinit_func(void *p [[maybe_unused]]) {

  DBUG_TRACE;

  assert(p);
  return 0;
}

/**
  Lmdb of simple lock controls.
*/
Lmdb_share *ha_lmdb::get_share() {
  Lmdb_share *tmp_share;

  DBUG_TRACE;

  lock_shared_ha_data();
  if (!(tmp_share = static_cast<Lmdb_share *>(get_ha_share_ptr()))) {
    tmp_share = new Lmdb_share;
    if (!tmp_share) goto err;

    set_ha_share_ptr(static_cast<Handler_share *>(tmp_share));
  }
err:
  unlock_shared_ha_data();
  return tmp_share;
}

static handler *lmdb_create_handler(handlerton *hton, TABLE_SHARE *table,
                                       bool, MEM_ROOT *mem_root) {
  return new (mem_root) ha_lmdb(hton, table);
}

ha_lmdb::ha_lmdb(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg), batch_count(0), batch_txn(nullptr) {}

/*
  List of all system tables specific to the SE.
*/
static st_handler_tablename ha_lmdb_system_tables[] = {
    {(const char *)nullptr, (const char *)nullptr}};

/**
  Check if the given db.tablename is a system table for this SE.
*/
static bool lmdb_is_supported_system_table(const char *db,
                                              const char *table_name,
                                              bool is_sql_layer_system_table) {
  st_handler_tablename *systab;

  // Does this SE support "ALL" SQL layer system tables ?
  if (is_sql_layer_system_table) return false;

  // Check if this is SE layer system tables
  systab = ha_lmdb_system_tables;
  while (systab && systab->db) {
    if (systab->db == db && strcmp(systab->tablename, table_name) == 0)
      return true;
    systab++;
  }

  return false;
}

/**
  Open a table
*/
int ha_lmdb::open(const char *name, int mode, uint test_if_locked, const dd::Table *tab_def) {
  DBUG_TRACE;
  
  if (!(share = get_share())) return 1;
  thr_lock_data_init(&share->lock, &lock, NULL);

  int rc = init_lmdb();
  if (rc != 0) {
    // Handle error
    fprintf(stderr, "Failed to initialize LMDB, error code: %d\n", rc);
    return HA_ERR_INTERNAL_ERROR;
  }   
  
  return 0;
}

/**
  Closes a table.
*/
// Make sure to commit any pending transactions when closing the table
int ha_lmdb::close() {
  if (batch_txn != nullptr) {
    int rc = mdb_txn_commit(batch_txn);
    if (rc != 0) {
      fprintf(stderr, "mdb_txn_commit failed in close, error %d %s\n", rc, mdb_strerror(rc));
      return HA_ERR_INTERNAL_ERROR;
    }
    batch_txn = nullptr;
    batch_count = 0;
  }
  return 0; // Replace handler::close() with 0 to indicate success
}

/**
 Inserts a row.
*/
int ha_lmdb::write_row(uchar *buf) {
  DBUG_TRACE;
  
  if (env == nullptr) {
    fprintf(stderr, "LMDB environment not initialized\n");
    return HA_ERR_INTERNAL_ERROR;
  }

  int rc;

  if (batch_txn == nullptr) {
    rc = mdb_txn_begin(env, NULL, 0, &batch_txn);
    if (rc != 0) {
      fprintf(stderr, "mdb_txn_begin failed in write_row, error %d %s\n", rc, mdb_strerror(rc));
      return HA_ERR_INTERNAL_ERROR;
    }
  }

  // Use the first field (assumed to be the primary key) as the LMDB key
  Field *key_field = table->field[0];
  
  MDB_val key;
  key.mv_data = buf + key_field->offset(table->record[0]);
  key.mv_size = key_field->pack_length();

  // Use the entire row buffer as the LMDB value
  MDB_val value;
  value.mv_data = buf;
  value.mv_size = table->s->reclength;

  rc = mdb_put(batch_txn, dbi, &key, &value, 0);
  if (rc != 0) {
    fprintf(stderr, "mdb_put failed in write_row, error %d %s\n", rc, mdb_strerror(rc));
    mdb_txn_abort(batch_txn);
    batch_txn = nullptr;
    batch_count = 0;
    return HA_ERR_INTERNAL_ERROR;
  }
  
  batch_count++;

  // Commit the transaction if we've reached the batch size
  if (batch_count >= BATCH_SIZE) {
    rc = mdb_txn_commit(batch_txn);
    if (rc != 0) {
      fprintf(stderr, "mdb_txn_commit failed in write_row, error %d %s\n", rc, mdb_strerror(rc));
      return HA_ERR_INTERNAL_ERROR;
    }
    batch_txn = nullptr;
    batch_count = 0;
  }

  return 0;
}

/**
  Updates a row.
  TODO
*/
int ha_lmdb::update_row(const uchar *, uchar *) {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

/**
  Deletes a row.
  TODO
*/
int ha_lmdb::delete_row(const uchar *) {
  DBUG_TRACE;
  DBUG_PRINT("info", ("Writing row to ha_lmdb table"));

  return 0;
}

/**
  Read map
*/
int ha_lmdb::index_read_map(uchar *, const uchar *, key_part_map,
                               enum ha_rkey_function) {
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
  Next Index
*/
int ha_lmdb::index_next(uchar *) {
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
  Previous Index
*/
int ha_lmdb::index_prev(uchar *) {
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
  First Key Index
*/
int ha_lmdb::index_first(uchar *) {
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
 Last Key Index
*/
int ha_lmdb::index_last(uchar *) {
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
  Table Scan
*/
int ha_lmdb::rnd_init(bool) {
  DBUG_TRACE;
  return 0;
}

int ha_lmdb::rnd_end() {
  DBUG_TRACE;
  return 0;
}

/**
  Each row in a table scan
*/
int ha_lmdb::rnd_next(uchar *) {
  //int rc;
  DBUG_TRACE;
  //rc = HA_ERR_END_OF_FILE;
  return 0;
}

/**
  Position in rnd_next
*/
void ha_lmdb::position(const uchar *) { DBUG_TRACE; }

/**
  Position
*/
int ha_lmdb::rnd_pos(uchar *, uchar *) {
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
  Info for the optimizer
*/
int ha_lmdb::info(uint) {
  DBUG_TRACE;
  return 0;
}

/**
  Extra
*/
int ha_lmdb::extra(enum ha_extra_function) {
  DBUG_TRACE;
  return 0;
}

/**
  Delete all rows
*/
int ha_lmdb::delete_all_rows() {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

/**
  Lock on the table
*/
int ha_lmdb::external_lock(THD *, int) {
  DBUG_TRACE;
  return 0;
}

/**
  Store Lock
*/
THR_LOCK_DATA **ha_lmdb::store_lock(THD *, THR_LOCK_DATA **to,
                                       enum thr_lock_type lock_type) {
  if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK) lock.type = lock_type;
  *to++ = &lock;
  return to;
}

/**
  Delete table
*/
int ha_lmdb::delete_table(const char *, const dd::Table *) {
  DBUG_TRACE;
  /* This is not implemented but we want someone to be able that it works. */
  return 0;
}

/**
  Rename table
*/
int ha_lmdb::rename_table(const char *, const char *, const dd::Table *,
                             dd::Table *) {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

/**
  Recors in range
*/
ha_rows ha_lmdb::records_in_range(uint, key_range *, key_range *) {
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

int ha_lmdb::create(const char *name, TABLE *, HA_CREATE_INFO *,
                       dd::Table *) {
  DBUG_TRACE;
  /*
    This is not implemented but we want someone to be able to see that it
    works.
  */

  /*
    It's just an lmdb of THDVAR_SET() usage below.
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

struct st_mysql_storage_engine lmdb_storage_engine = {
    MYSQL_HANDLERTON_INTERFACE_VERSION};

static ulong srv_enum_var = 0;
static ulong srv_ulong_var = 0;
static double srv_double_var = 0;
static int srv_signed_int_var = 0;
static long srv_signed_long_var = 0;
static longlong srv_signed_longlong_var = 0;

const char *lmdb_enum_var_names[] = {"e1", "e2", NullS};

TYPELIB lmdb_enum_var_typelib = {array_elements(lmdb_enum_var_names) - 1,
                            "enum_var_typelib", lmdb_enum_var_names, nullptr};

static MYSQL_SYSVAR_ENUM(enum_var,                        // name
                         srv_enum_var,                    // varname
                         PLUGIN_VAR_RQCMDARG,             // opt
                         "Sample ENUM system variable.",  // comment
                         nullptr,                         // check
                         nullptr,                         // update
                         0,                               // def
                         &lmdb_enum_var_typelib);         // typelib

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

static SYS_VAR *lmdb_system_variables[] = {
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

// this is an lmdb of SHOW_FUNC
static int show_func_lmdb(MYSQL_THD, SHOW_VAR *var, char *buf) {
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

struct lmdb_vars_t {
  ulong var1;
  double var2;
  char var3[64];
  bool var4;
  bool var5;
  ulong var6;
};

lmdb_vars_t lmdb_vars = {100, 20.01, "three hundred", true, false, 8250};

static SHOW_VAR show_status_lmdb[] = {
    {"var1", (char *)&lmdb_vars.var1, SHOW_LONG, SHOW_SCOPE_GLOBAL},
    {"var2", (char *)&lmdb_vars.var2, SHOW_DOUBLE, SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_UNDEF,
     SHOW_SCOPE_UNDEF}  // null terminator required
};

static SHOW_VAR show_array_lmdb[] = {
    {"array", (char *)show_status_lmdb, SHOW_ARRAY, SHOW_SCOPE_GLOBAL},
    {"var3", (char *)&lmdb_vars.var3, SHOW_CHAR, SHOW_SCOPE_GLOBAL},
    {"var4", (char *)&lmdb_vars.var4, SHOW_BOOL, SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_UNDEF, SHOW_SCOPE_UNDEF}};

static SHOW_VAR func_status[] = {
    {"lmdb_func_lmdb", (char *)show_func_lmdb, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"lmdb_status_var5", (char *)&lmdb_vars.var5, SHOW_BOOL,
     SHOW_SCOPE_GLOBAL},
    {"lmdb_status_var6", (char *)&lmdb_vars.var6, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {"lmdb_status", (char *)show_array_lmdb, SHOW_ARRAY,
     SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_UNDEF, SHOW_SCOPE_UNDEF}};

mysql_declare_plugin(lmdb){
    MYSQL_STORAGE_ENGINE_PLUGIN,
    &lmdb_storage_engine,
    "LMDB",
    PLUGIN_AUTHOR_ORACLE,
    "LMDB storage engine",
    PLUGIN_LICENSE_GPL,
    lmdb_init_func,   /* Plugin Init */
    nullptr,             /* Plugin check uninstall */
    lmdb_deinit_func, /* Plugin Deinit */
    0x0001 /* 0.1 */,
    func_status,              /* status variables */
    lmdb_system_variables, /* system variables */
    nullptr,                  /* config options */
    0,                        /* flags */
} mysql_declare_plugin_end;
