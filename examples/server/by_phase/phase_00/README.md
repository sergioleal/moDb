# Fase 0 - contrato e compatibilidade

Mostra as constantes publicas de versao e a negociacao major/minor usada pelos
artefatos e pelo protocolo.

## Como rodar

```powershell
& 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe' --build --preset debug --target ring0_server_phase_00_version_compatibility
$env:PATH = 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\mingw\bin;' + $env:PATH
.\build\debug\ring0_server_phase_00_version_compatibility.exe
```

Linux:

```bash
cmake --build --preset debug --target ring0_server_phase_00_version_compatibility
./build/debug/ring0_server_phase_00_version_compatibility
```
