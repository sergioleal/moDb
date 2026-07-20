# Fase 4 - relacionamentos

Mostra `Ref<T>` e `OwnedRef<T>` em objetos de dominio antes desses dados serem
consultados pelo servidor.

## Como rodar

```powershell
& 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe' --build --preset debug --target ring0_server_phase_04_relationships
$env:PATH = 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\mingw\bin;' + $env:PATH
.\build\debug\ring0_server_phase_04_relationships.exe
```

Linux:

```bash
cmake --build --preset debug --target ring0_server_phase_04_relationships
./build/debug/ring0_server_phase_04_relationships
```
