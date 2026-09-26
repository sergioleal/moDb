#!/usr/bin/env bash
# Gera o site HTML navegável com toda a documentação Markdown do repositório
# (build/docs-site/index.html). Falha se houver link interno quebrado.
#
#   ./scripts/build-docs.sh
#   OUTPUT_DIR=/tmp/modb-docs NO_STRICT=1 ./scripts/build-docs.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT_DIR="${OUTPUT_DIR:-$ROOT/build/docs-site}"
NO_STRICT="${NO_STRICT:-0}"

PYTHON="$(command -v python3 || command -v python || true)"
if [[ -z "$PYTHON" ]]; then
    echo "Python 3 não encontrado no PATH (precisa do pacote 'markdown': pip install markdown)." >&2
    exit 1
fi

args=("$ROOT/scripts/build_docs_site.py" --out "$OUTPUT_DIR")
if [[ "$NO_STRICT" == "1" ]]; then
    args+=(--no-strict)
fi

"$PYTHON" "${args[@]}"
