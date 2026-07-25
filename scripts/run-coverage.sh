#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/build/coverage"
OUTPUT_DIR="${OUTPUT_DIR:-$ROOT/coverage-results}"
SKIP_BUILD="${SKIP_BUILD:-0}"
SKIP_TESTS="${SKIP_TESTS:-0}"

if [[ "$SKIP_BUILD" != "1" ]]; then
    cmake --preset coverage
    cmake --build --preset coverage
fi

if [[ "$SKIP_TESTS" != "1" ]]; then
    ctest --preset coverage || echo "ctest reportou falhas - o relatorio ainda sera gerado com os dados coletados." >&2
fi

mkdir -p "$OUTPUT_DIR"

if command -v gcovr >/dev/null 2>&1; then
    gcovr --root "$ROOT" --filter 'include/modb/.*' --filter 'src/.*' \
        --object-directory "$BUILD_DIR" \
        --print-summary --html-details "$OUTPUT_DIR/coverage.html" -o "$OUTPUT_DIR/coverage.txt"
    echo "Relatorio: $OUTPUT_DIR/coverage.txt / $OUTPUT_DIR/coverage.html"
else
    echo "gcovr nao encontrado no PATH (instale com 'pip install gcovr' para um relatorio agregado em HTML). Gerando saida bruta do gcov ..." >&2
    mapfile -t gcda_files < <(find "$BUILD_DIR" -name '*.gcda')
    if [[ ${#gcda_files[@]} -eq 0 ]]; then
        echo "Nenhum arquivo .gcda encontrado em $BUILD_DIR - confirme que MODB_ENABLE_COVERAGE=ON e que os testes rodaram." >&2
        exit 1
    fi
    (cd "$OUTPUT_DIR" && for gcda in "${gcda_files[@]}"; do
        gcov --object-directory "$(dirname "$gcda")" "$gcda" >/dev/null
    done)
    echo "Arquivos .gcov brutos gerados em $OUTPUT_DIR (um por unidade de traducao)."
fi
