// keyring_stubs.cc
//
// Purpose: Provide minimal/do-nothing definitions for MySQL symbols
// that Percona keyring plugin code references, so we can build
// a standalone "decrypt" tool without linking the full server.
//
// NOTE: No "extern \"C\"" for push_warning(...) etc., because
// file_io.cc calls them as normal C++ functions.

#include <cstddef>  // for size_t
#include <cstdint>
#include <cstring>
#include "m_ctype.h"

// ---------- 1) Minimal THD, Security_context, and thread-local current_thd ----------

class THD {};               // empty stand-in for MySQL's THD
class Security_context {};  // empty stand-in

// MySQL often declares "thread_local THD *current_thd;"
__thread THD *current_thd = nullptr;

// ---------- 2) push_warning, thd_get_security_context, security_context_get_option ----------

// MySQL uses an enum in a "Sql_condition" namespace for severity levels:
namespace Sql_condition {
  enum enum_severity_level {
    SL_NOTE=0, SL_WARNING=1, SL_ERROR=2
  };
}

// The plugin code calls push_warning(THD*, Sql_condition::enum_severity_level, unsigned int, const char*)
// We'll define a no-op function in the global namespace (C++).
void push_warning(
    THD*,
    Sql_condition::enum_severity_level,
    unsigned int,
    const char*)
{
  // do nothing
}

// The plugin calls these to check if the current user is super user:
int thd_get_security_context(THD*, Security_context**)
{
  return 1; // do nothing
}

int security_context_get_option(Security_context*, const char*, void*)
{
  return 1; // do nothing
}

// ---------- 3) MySQL RW-lock stubs for LOCK_keyring ----------

struct mysql_rwlock_t_fake {};  // minimal placeholder
using mysql_rwlock_t = mysql_rwlock_t_fake;

// The plugin references this global lock:
mysql_rwlock_t LOCK_keyring;

using PSI_rwlock_key = void*; // dummy pointer

int mysql_rwlock_init(PSI_rwlock_key, mysql_rwlock_t*) { return 0; }
int mysql_rwlock_destroy(mysql_rwlock_t*) { return 0; }
int mysql_rwlock_rdlock(mysql_rwlock_t*) { return 0; }
int mysql_rwlock_wrlock(mysql_rwlock_t*) { return 0; }
int mysql_rwlock_unlock(mysql_rwlock_t*) { return 0; }

// ---------- 4) system_charset_info stub ----------

MYSQL_PLUGIN_IMPORT CHARSET_INFO* system_charset_info = &my_charset_latin1;