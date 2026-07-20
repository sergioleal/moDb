# Fase 12 - grafos via servidor

Coloque aqui exemplos de aplicacoes que consomem dados de grafo expostos pelo
servidor.

## Como rodar

```powershell
& 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe' --build --preset debug --target ring0_server_phase_12_graph_traversal
$env:PATH = 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\mingw\bin;' + $env:PATH
.\build\debug\ring0_server_phase_12_graph_traversal.exe
```

Linux:

```bash
cmake --build --preset debug --target ring0_server_phase_12_graph_traversal
./build/debug/ring0_server_phase_12_graph_traversal
```
