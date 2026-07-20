# Fase 1 - modelo de objetos e catalogo

Mostra o primeiro passo de uma aplicacao: declarar um tipo C++ e registra-lo no
catalogo do banco antes de qualquer servidor atender consultas.

## Como rodar

```powershell
& 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe' --build --preset debug --target ring0_server_phase_01_bind_type
$env:PATH = 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\mingw\bin;' + $env:PATH
.\build\debug\ring0_server_phase_01_bind_type.exe
```

Linux:

```bash
cmake --build --preset debug --target ring0_server_phase_01_bind_type
./build/debug/ring0_server_phase_01_bind_type
```
