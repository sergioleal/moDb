# Fase 11 - facades e handles remotos

Coloque aqui exemplos de descoberta de facades, negociacao de versao e uso de
`open_facade<TFacade>()`.

## Como rodar

```powershell
& 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe' --build --preset debug --target ring0_server_phase_11_open_facade
$env:PATH = 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\mingw\bin;' + $env:PATH
.\build\debug\ring0_server_phase_11_open_facade.exe
```

Linux:

```bash
cmake --build --preset debug --target ring0_server_phase_11_open_facade
./build/debug/ring0_server_phase_11_open_facade
```
