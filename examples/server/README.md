# Exemplos de uso do servidor

Esta pasta guarda exemplos de aplicacoes que conectam ao servidor Ring0 usando a
biblioteca publica `modb::app_client`.

Organizacao:

- `by_phase/phase_00`: contrato, versao e compatibilidade.
- `by_phase/phase_01`: binding e registro de tipo.
- `by_phase/phase_02`: persistencia e reabertura.
- `by_phase/phase_03`: handles tipados.
- `by_phase/phase_04`: relacionamentos.
- `by_phase/phase_05`: transacoes, WAL e recovery.
- `by_phase/phase_06`: snapshots e MVCC.
- `by_phase/phase_07`: consultas em streaming.
- `by_phase/phase_08`: conexao, handshake, query e streaming remoto.
- `by_phase/phase_09`: operacoes de dominio remotas.
- `by_phase/phase_10`: capacidades publicas negociadas.
- `by_phase/phase_11`: facades e handles remotos tipados.
- `by_phase/phase_12`: travessias de grafo.
- `by_phase/phase_13`: I/O assincrono posicional.

Cada fase contem um exemplo pequeno e independente, registrado como alvo CMake
`ring0_server_phase_XX_*` quando `MODB_BUILD_EXAMPLES=ON`.

## Como rodar

Configure uma vez:

```powershell
& 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe' --preset debug
```

Compile o alvo da fase desejada:

```powershell
& 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\cmake\win\x64\bin\cmake.exe' --build --preset debug --target ring0_server_phase_08_connect_query
```

No Windows com a toolchain MinGW do CLion, rode com o runtime no `PATH`:

```powershell
$env:PATH = 'C:\Program Files\JetBrains\CLion 2026.1.4\bin\mingw\bin;' + $env:PATH
.\build\debug\ring0_server_phase_08_connect_query.exe
```

No Linux, o equivalente esperado e:

```bash
cmake --preset debug
cmake --build --preset debug --target ring0_server_phase_08_connect_query
./build/debug/ring0_server_phase_08_connect_query
```
