#!/bin/bash
# Resume os avisos do ThreadSanitizer de alguns testes (build do run_tsan.sh).
# Uso: tsan_summary.sh [executável de teste ...]  (padrão: os do servidor de rede)
export TSAN_OPTIONS="halt_on_error=0 report_signal_unsafe=0"
B="$HOME/modb-build/tsan"
OUT="$HOME/tsan-reports"
mkdir -p "$OUT"
rm -f "$OUT"/*.log
cd "$B" || exit 1
tests=("$@")
if [ ${#tests[@]} -eq 0 ]; then
  tests=(modb_server_host_tests modb_server_streaming_tests modb_operation_server_tests modb_facade_server_tests modb_app_server_connection_tests)
fi
for t in "${tests[@]}"; do
  [ -x "./$t" ] && timeout 180 setarch "$(uname -m)" -R "./$t" > "$OUT/$t.log" 2>&1
done
echo "== avisos por teste"
for f in "$OUT"/*.log; do echo "$(basename "$f" .log): $(grep -c 'WARNING: ThreadSanitizer' "$f")"; done
echo "== locais (SUMMARY), mais frequentes"
cat "$OUT"/*.log | grep "SUMMARY: ThreadSanitizer" | sed 's#/mnt/c/tmp/apps/cpp/moDb2/##; s#/usr/include/c++/14/##' | sort | uniq -c | sort -rn | head -25
