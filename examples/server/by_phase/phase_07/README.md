# Fase 7 - consultas em streaming

Mostra uma query tipada com filtro e limite antes da mesma descricao ser
transportada pelo protocolo do servidor na Fase 8.

## Como rodar

```powershell
& 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe' --build --preset debug --target ring0_server_phase_07_streaming_query
$env:PATH = 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\mingw\bin;' + $env:PATH
.\build\debug\ring0_server_phase_07_streaming_query.exe
```

Linux:

```bash
cmake --build --preset debug --target ring0_server_phase_07_streaming_query
./build/debug/ring0_server_phase_07_streaming_query
```
