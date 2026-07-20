# Fase 6 - snapshots e MVCC

Mostra uma leitura por snapshot que permanece estavel mesmo depois de um commit
posterior alterar o objeto.

## Como rodar

```powershell
& 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe' --build --preset debug --target ring0_server_phase_06_snapshot_read
$env:PATH = 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\mingw\bin;' + $env:PATH
.\build\debug\ring0_server_phase_06_snapshot_read.exe
```

Linux:

```bash
cmake --build --preset debug --target ring0_server_phase_06_snapshot_read
./build/debug/ring0_server_phase_06_snapshot_read
```
