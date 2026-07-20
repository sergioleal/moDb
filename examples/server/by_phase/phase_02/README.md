# Fase 2 - codec e ObjectStore persistente

Cria um objeto, fecha o banco e reabre o arquivo para materializar o mesmo
objeto pelo `ObjectId`.

## Como rodar

```powershell
& 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe' --build --preset debug --target ring0_server_phase_02_persist_reopen
$env:PATH = 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\mingw\bin;' + $env:PATH
.\build\debug\ring0_server_phase_02_persist_reopen.exe
```

Linux:

```bash
cmake --build --preset debug --target ring0_server_phase_02_persist_reopen
./build/debug/ring0_server_phase_02_persist_reopen
```
