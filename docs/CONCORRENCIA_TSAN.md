# ThreadSanitizer no WSL

O critério das tarefas de concorrência (docs-process/PLANO_CONCORRENCIA.md, da
C4 em diante) é passar limpo no ThreadSanitizer. O MinGW não tem TSan; ele
roda no WSL (ou em qualquer Linux) com GCC ≥ 13 ou Clang.

## Uma vez: ferramentas

No PowerShell, um comando de cada vez (pede a senha do usuário do Ubuntu):

```powershell
wsl -d Ubuntu-24.04 -- sudo apt-get update
```

```powershell
wsl -d Ubuntu-24.04 -- sudo apt-get install -y cmake ninja-build g++-14
```

O moDb pede CMake ≥ 3.30 e o Ubuntu 24.04 traz 3.28; o CMake oficial da
Kitware (PyPI) vai num venv do usuário, sem sudo:

```powershell
wsl -d Ubuntu-24.04 -- bash -c 'python3 -m venv ~/.venvs/modb-tools && ~/.venvs/modb-tools/bin/pip install "cmake>=3.30"'
```

## Compilar e rodar

Tudo de uma vez (configura, compila e roda o `ctest`; opcional: regex de testes):

```powershell
wsl -d Ubuntu-24.04 -- bash /mnt/c/tmp/apps/cpp/moDb2/scripts/run_tsan.sh
```

Ou passo a passo:

O fonte fica em `/mnt/c/...` (o mesmo checkout do Windows); a árvore de build
fica no Linux (`~/modb-build/tsan`), que é bem mais rápido que compilar sobre
`/mnt/c`:

```powershell
wsl -d Ubuntu-24.04 -- bash -c 'export PATH="$HOME/.venvs/modb-tools/bin:$PATH"; cd /mnt/c/tmp/apps/cpp/moDb2 && CXX=g++-14 cmake --preset tsan -B ~/modb-build/tsan && cmake --build ~/modb-build/tsan -j 14'
```

```powershell
wsl -d Ubuntu-24.04 -- bash -c 'export PATH="$HOME/.venvs/modb-tools/bin:$PATH"; export TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1"; setarch "$(uname -m)" -R ctest --test-dir ~/modb-build/tsan -j 8 --output-on-failure'
```

**`setarch -R` é obrigatório:** o kernel do WSL randomiza o espaço de
endereços com mais bits do que o TSan aceita, e todo teste morre na partida com
`FATAL: ThreadSanitizer: unexpected memory mapping`. `setarch -R` desliga a
randomização para o `ctest` e os processos filhos (sem sudo; a alternativa é
`sysctl vm.mmap_rnd_bits=28`, que pede root).

`modb.concurrency_smoke` (C4.2: 8 threads só lendo) só existe neste preset até
a C6: antes dela o caminho de leitura escreve em estado compartilhado sem lock,
e o TSan acusa — é a linha de base.

O preset `sanitizers` também funciona no WSL e lá ativa ASan e UBSan de
verdade (no MinGW ele só liga as asserções da libstdc++).
