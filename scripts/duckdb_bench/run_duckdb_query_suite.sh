#!/usr/bin/env bash
set -euo pipefail

query_file="${1:-scripts/duckdb_bench/query_suite.sql}"
out_dir="${2:-build/duckdb_bench/query_suite}"

mysql_cli="${MYSQL:-mysql}"
db_name="${DUCKDB_BENCH_DB:-duckdb_bench}"
iters="${DUCKDB_QUERY_ITERS:-3}"
setup="${DUCKDB_QUERY_SETUP:-0}"
customers="${DUCKDB_QUERY_CUSTOMERS:-1000}"
orders="${DUCKDB_QUERY_ORDERS:-5000}"
lineitems="${DUCKDB_QUERY_LINEITEMS:-20000}"

baseline_mode="${DUCKDB_QUERY_BASELINE_MODE:-OFF}"
duckdb_mode="${DUCKDB_QUERY_DUCKDB_MODE:-FORCED}"

mysql_args=()
if [[ -n "${MYSQL_HOST:-}" ]]; then mysql_args+=("-h" "${MYSQL_HOST}"); fi
if [[ -n "${MYSQL_PORT:-}" ]]; then mysql_args+=("-P" "${MYSQL_PORT}"); fi
if [[ -n "${MYSQL_SOCKET:-}" ]]; then mysql_args+=("-S" "${MYSQL_SOCKET}"); fi
if [[ -n "${MYSQL_USER:-}" ]]; then mysql_args+=("-u" "${MYSQL_USER}"); fi
if [[ -n "${MYSQL_PASSWORD:-}" ]]; then mysql_args+=("-p${MYSQL_PASSWORD}"); fi

hash_cmd="sha256sum"
if ! command -v "${hash_cmd}" >/dev/null 2>&1; then
  hash_cmd="shasum -a 256"
fi

mkdir -p "${out_dir}"

if [[ "${setup}" == "1" ]]; then
  "${mysql_cli}" "${mysql_args[@]}" -e "CREATE DATABASE IF NOT EXISTS ${db_name};"
  setup_sql="${out_dir}/setup.sql"
  {
    echo "USE ${db_name};"
    echo "SET @customers = ${customers};"
    echo "SET @orders = ${orders};"
    echo "SET @lineitems = ${lineitems};"
    cat scripts/duckdb_bench/query_suite_setup.sql
  } > "${setup_sql}"
  "${mysql_cli}" "${mysql_args[@]}" < "${setup_sql}"
fi

mysql_args+=("--batch" "--raw" "--skip-column-names" "--database" "${db_name}")

read_queries() {
  local file="$1"
  local name=""
  local sql=""
  local idx=0

  while IFS= read -r line || [[ -n "$line" ]]; do
    if [[ "$line" =~ ^[[:space:]]*--[[:space:]]*name:[[:space:]]*(.*)$ ]]; then
      name="${BASH_REMATCH[1]}"
      continue
    fi
    if [[ "$line" =~ ^[[:space:]]*-- ]]; then
      continue
    fi
    if [[ -z "${line//[[:space:]]/}" ]]; then
      continue
    fi

    sql+="${line} "
    if [[ "$line" == *";"* ]]; then
      queries[idx]="${sql%;*}"
      names[idx]="${name:-query_${idx}}"
      sql=""
      name=""
      idx=$((idx + 1))
    fi
  done < "$file"

  if [[ -n "${sql//[[:space:]]/}" ]]; then
    echo "Unterminated query in ${file}" >&2
    exit 1
  fi
}

run_query() {
  local mode="$1"
  local name="$2"
  local sql="$3"
  local total_ms=0
  local hash=""
  local status=0
  local errfile="${out_dir}/${name}.${mode}.err"
  local outfile="${out_dir}/${name}.${mode}.out"
  local output=""

  : > "${errfile}"

  for ((i=1; i<=iters; i++)); do
    local start_ns
    local end_ns
    start_ns=$(date +%s%N)
    output=$("${mysql_cli}" "${mysql_args[@]}" -e "SET SESSION use_secondary_engine=${mode}; ${sql}" 2>>"${errfile}") || status=$?
    end_ns=$(date +%s%N)
    local elapsed_ms=$(((end_ns - start_ns) / 1000000))

    if [[ ${status} -ne 0 ]]; then
      break
    fi

    total_ms=$((total_ms + elapsed_ms))
    if [[ $i -eq 1 ]]; then
      printf "%s\n" "${output}" > "${outfile}"
      hash=$(printf "%s" "${output}" | ${hash_cmd} | awk '{print $1}')
    fi
  done

  if [[ ${status} -ne 0 ]]; then
    echo "ERROR"
    return 0
  fi

  local avg_ms=$((total_ms / iters))
  echo "${avg_ms},${hash}"
}

queries=()
names=()
read_queries "${query_file}"

results_csv="${out_dir}/results.csv"
results_md="${out_dir}/results.md"

{
  echo "query,baseline_ms,duckdb_ms,baseline_hash,duckdb_hash,match"
  for i in "${!queries[@]}"; do
    name="${names[$i]}"
    sql="${queries[$i]}"

    baseline=$(run_query "${baseline_mode}" "${name}" "${sql}")
    duckdb=$(run_query "${duckdb_mode}" "${name}" "${sql}")

    if [[ "${baseline}" == "ERROR" || "${duckdb}" == "ERROR" ]]; then
      echo "${name},ERROR,ERROR,,,false"
      continue
    fi

    baseline_ms=${baseline%%,*}
    baseline_hash=${baseline#*,}
    duckdb_ms=${duckdb%%,*}
    duckdb_hash=${duckdb#*,}

    match="false"
    if [[ "${baseline_hash}" == "${duckdb_hash}" ]]; then
      match="true"
    fi

    echo "${name},${baseline_ms},${duckdb_ms},${baseline_hash},${duckdb_hash},${match}"
  done
} > "${results_csv}"

{
  echo "# DuckDB Analytical Query Suite"
  echo
  echo "Database: ${db_name}"
  echo "Queries: ${#queries[@]}"
  echo "Baseline mode: ${baseline_mode}"
  echo "DuckDB mode: ${duckdb_mode}"
  echo
  echo "| Query | Baseline ms | DuckDB ms | Match |"
  echo "| --- | ---: | ---: | :---: |"
  tail -n +2 "${results_csv}" | while IFS=',' read -r q bms dms _ _ match; do
    echo "| ${q} | ${bms} | ${dms} | ${match} |"
  done
} > "${results_md}"

echo "Results written to ${results_csv}"
echo "Markdown summary: ${results_md}"
