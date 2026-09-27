#!/bin/bash
# Build + ctest do preset tsan no WSL. Uso: run_tsan.sh [regex de testes]
export PATH="$HOME/.venvs/modb-tools/bin:$PATH"
export TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1"
cd /mnt/c/tmp/apps/cpp/moDb2 || exit 1
CXX=g++-14 cmake --preset tsan -B "$HOME/modb-build/tsan" > "$HOME/modb-build-tsan-config.log" 2>&1 || { echo "config falhou"; tail -20 "$HOME/modb-build-tsan-config.log"; exit 1; }
cmake --build "$HOME/modb-build/tsan" -j 14 > "$HOME/modb-build-tsan.log" 2>&1 || { echo "build falhou"; grep -E "error" "$HOME/modb-build-tsan.log" | head -20; exit 1; }
if [ -n "$1" ]; then
  setarch "$(uname -m)" -R ctest --test-dir "$HOME/modb-build/tsan" -j 8 -R "$1" --output-on-failure > "$HOME/modb-ctest-tsan.log" 2>&1
else
  setarch "$(uname -m)" -R ctest --test-dir "$HOME/modb-build/tsan" -j 8 > "$HOME/modb-ctest-tsan.log" 2>&1
fi
echo "ctest=$?"
grep -E "tests passed|\(Failed\)|Timeout|\*\*\*" "$HOME/modb-ctest-tsan.log" | head -60
