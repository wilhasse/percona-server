#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:-build}"
output_csv="${2:-${build_dir}/duckdb_bench/results.csv}"

rows="${DUCKDB_BENCH_ROWS:-100000}"
batch="${DUCKDB_BENCH_BATCH:-1000}"
query_iters="${DUCKDB_BENCH_QUERY_ITERS:-5}"

if [[ ! -d "${build_dir}" ]]; then
  echo "Build directory not found: ${build_dir}" >&2
  exit 1
fi

cmake --build "${build_dir}" --target duckdb_bench

bin_path="${build_dir}/bin/duckdb_bench"
if [[ ! -x "${bin_path}" ]]; then
  bin_path="${build_dir}/runtime_output_directory/duckdb_bench"
fi

if [[ ! -x "${bin_path}" ]]; then
  echo "duckdb_bench binary not found in ${build_dir}/bin" >&2
  exit 1
fi

mkdir -p "$(dirname "${output_csv}")"

"${bin_path}" \
  --rows "${rows}" \
  --batch "${batch}" \
  --query-iters "${query_iters}" \
  --out "${output_csv}"

python3 scripts/duckdb_bench/bench_report.py "${output_csv}" \
  > "${output_csv%.csv}.md"

echo "Results written to ${output_csv}"
echo "Markdown summary: ${output_csv%.csv}.md"
