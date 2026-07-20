# Fase 5 - transacoes, WAL e recovery

Cria um commit duravel, reabre o banco e confirma que a alteracao sobreviveu ao
ciclo de fechamento/reabertura.

## Como rodar

```powershell
& 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe' --build --preset debug --target ring0_server_phase_05_transaction_recovery
$env:PATH = 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\mingw\bin;' + $env:PATH
.\build\debug\ring0_server_phase_05_transaction_recovery.exe
```

Linux:

```bash
cmake --build --preset debug --target ring0_server_phase_05_transaction_recovery
./build/debug/ring0_server_phase_05_transaction_recovery
```
