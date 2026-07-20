# Fase 10 - estabilizacao e contrato publico

Mostra uma aplicacao consultando as capacidades negociadas do servidor atraves
da biblioteca `modb::app_client`.

## Como rodar

```powershell
& 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe' --build --preset debug --target ring0_server_phase_10_handshake_capabilities
$env:PATH = 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\mingw\bin;' + $env:PATH
.\build\debug\ring0_server_phase_10_handshake_capabilities.exe
```

Linux:

```bash
cmake --build --preset debug --target ring0_server_phase_10_handshake_capabilities
./build/debug/ring0_server_phase_10_handshake_capabilities
```
