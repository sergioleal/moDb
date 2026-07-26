#!/usr/bin/env bash
set -euo pipefail

PROFILE="${1:-smoke}"
SEED="${2:-1}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT_DIR="${OUTPUT_DIR:-$ROOT/benchmark-results}"
if [[ -z "${BINARY:-}" ]]; then
    # Ordem deliberada: builds otimizados primeiro. Gate oficial nunca sai de
    # Debug (-O0) -- ver docs-process/PLANO_PROFILING.md §3, defeito M1.
    for candidate in "$ROOT/build/relwithdebinfo/modb_bench" "$ROOT/build/release/modb_bench" \
                     "$ROOT/build/debug/modb_bench"; do
        if [[ -x "$candidate" ]]; then
            BINARY="$candidate"
            break
        fi
    done
fi
if [[ -z "${BINARY:-}" || ! -x "$BINARY" ]]; then
    echo "modb_bench não encontrado. Compile com 'cmake --build --preset relwithdebinfo'." >&2
    exit 1
fi
if [[ "$BINARY" == *"/build/debug/"* ]]; then
    echo "aviso: usando o binário Debug (-O0) -- valida o runner, não mede desempenho." >&2
fi

mkdir -p "$OUTPUT_DIR"
"$BINARY" run --profile "$PROFILE" --seed "$SEED" --output-dir "$OUTPUT_DIR"
ls -1t "$OUTPUT_DIR"/modb-benchmark-*.jsonl | head -n 1
