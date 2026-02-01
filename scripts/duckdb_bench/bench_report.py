#!/usr/bin/env python3
import csv
import sys


def load_rows(path):
    with open(path, newline="") as fh:
        return list(csv.DictReader(fh))


def fmt(num):
    if num is None:
        return "-"
    return f"{num:.2f}"


def to_float(val):
    try:
        return float(val)
    except Exception:
        return None


def main():
    if len(sys.argv) < 2:
        print("usage: bench_report.py results.csv", file=sys.stderr)
        return 1

    rows = load_rows(sys.argv[1])
    if not rows:
        print("No results found.")
        return 0

    by_name = {row["name"]: row for row in rows if row.get("metric") == "throughput"}
    baseline = to_float(by_name.get("sql_insert", {}).get("rows_per_sec", "")) if by_name else None

    print("# DuckDB Bench Summary")
    print("")
    print("| Name | Rows | Batch | Seconds | Rows/sec | Speedup vs SQL |")
    print("| --- | --- | --- | --- | --- | --- |")
    for row in rows:
        if row.get("metric") != "throughput":
            continue
        rps = to_float(row.get("rows_per_sec", ""))
        speedup = None
        if baseline and rps:
            speedup = rps / baseline
        print(
            f"| {row.get('name')} | {row.get('rows')} | {row.get('batch')} | "
            f"{row.get('seconds')} | {row.get('rows_per_sec')} | {fmt(speedup)} |"
        )

    print("")
    print("| Query | Rows | Batch | Avg latency (ms) |")
    print("| --- | --- | --- | --- |")
    for row in rows:
        if row.get("metric") != "query":
            continue
        print(
            f"| {row.get('name')} | {row.get('rows')} | {row.get('batch')} | "
            f"{row.get('latency_ms')} |"
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
