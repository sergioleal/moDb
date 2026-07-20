# Fase 13 - I/O assincrono no servidor

Coloque aqui exemplos focados em comportamento operacional do servidor quando o
storage usa a camada `AsyncFile`.

## Como rodar

```powershell
& 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe' --build --preset debug --target ring0_server_phase_13_async_io
$env:PATH = 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\mingw\bin;' + $env:PATH
.\build\debug\ring0_server_phase_13_async_io.exe
```

Linux:

```bash
cmake --build --preset debug --target ring0_server_phase_13_async_io
./build/debug/ring0_server_phase_13_async_io
```
