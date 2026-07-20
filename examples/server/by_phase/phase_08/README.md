# Fase 8 - servidor, protocolo e streaming

Exemplos desta fase devem cobrir:

- conectar e negociar `Hello`/`HelloOk`;
- executar `QueryDescription`;
- consumir `ObjectStream`;
- aplicar `Cancel` e observar backpressure quando o exemplo precisar disso.

## Como rodar

```powershell
& 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe' --build --preset debug --target ring0_server_phase_08_connect_query
$env:PATH = 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\mingw\bin;' + $env:PATH
.\build\debug\ring0_server_phase_08_connect_query.exe
```

Linux:

```bash
cmake --build --preset debug --target ring0_server_phase_08_connect_query
./build/debug/ring0_server_phase_08_connect_query
```
