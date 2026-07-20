# Fase 9 - operacoes de dominio

Coloque aqui exemplos que chamam operacoes remotas pelo servidor usando
`modb::app::ServerConnection::call`.

## Como rodar

```powershell
& 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe' --build --preset debug --target ring0_server_phase_09_call_operation
$env:PATH = 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\mingw\bin;' + $env:PATH
.\build\debug\ring0_server_phase_09_call_operation.exe
```

Linux:

```bash
cmake --build --preset debug --target ring0_server_phase_09_call_operation
./build/debug/ring0_server_phase_09_call_operation
```
