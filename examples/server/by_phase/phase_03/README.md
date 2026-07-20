# Fase 3 - Handle e ProjectionPlan

Mostra o uso de `Handle<T>` para alterar um objeto tipado dentro de uma
transacao.

## Como rodar

```powershell
& 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe' --build --preset debug --target ring0_server_phase_03_handle_update
$env:PATH = 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\mingw\bin;' + $env:PATH
.\build\debug\ring0_server_phase_03_handle_update.exe
```

Linux:

```bash
cmake --build --preset debug --target ring0_server_phase_03_handle_update
./build/debug/ring0_server_phase_03_handle_update
```
