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

#
# Check if --plugin-dir was setup for DuckDB
#
if (`SELECT CONCAT('--plugin-dir=', REPLACE(@@plugin_dir, '\\', '/')) != '$DUCKDB_PLUGIN_OPT/'`) {
  --skip DuckDB plugin requires that --plugin-dir is set to the plugin dir
}

enable_query_log;
