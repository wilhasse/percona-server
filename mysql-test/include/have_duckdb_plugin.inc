#
# Check if server has support for loading plugins
#
if (`SELECT @@have_dynamic_loading != 'YES'`) {
  --skip DuckDB plugin requires dynamic loading
}

#
# Check if the variable DUCKDB_PLUGIN is set
#
if (!$DUCKDB_PLUGIN) {
  --skip Could not find library for DuckDB plugin, see the file plugin.defs
}

enable_query_log;
