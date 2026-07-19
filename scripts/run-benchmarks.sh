#!/usr/bin/env bash
set -euo pipefail

PROFILE="${1:-smoke}"
SEED="${2:-1}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT_DIR="${OUTPUT_DIR:-$ROOT/benchmark-results}"
BINARY="${BINARY:-$ROOT/build/debug/modb_bench}"

mkdir -p "$OUTPUT_DIR"
"$BINARY" run --profile "$PROFILE" --seed "$SEED" --output-dir "$OUTPUT_DIR"
ls -1t "$OUTPUT_DIR"/modb-benchmark-*.jsonl | head -n 1
