# ADR-015 — Política de compatibilidade (formato, protocolo e API C++)

- Estado: aceito para a Fase 10E
- Data: 2026-07-19

## Contexto

O moDb precisa evoluir o superbloco, as páginas internas e o protocolo Hello
sem forçar migração destrutiva silenciosa, e precisa oferecer uma superfície
C++ instalável para consumidores externos. Até a 10D as versões eram escalares
com igualdade exata; faltava uma matriz major/minor e a instalação CMake.

## Decisão

### Formato de arquivo

- Cada versão persistida é um par `CompatibilityVersion{major, minor}`.
- No fio/disco legado, um `u16` com valor `< 256` significa `{major=valor, minor=0}`.
  Quando `minor > 0`, o fio usa `(major << 8) | minor`.
- **Major diferente** → `incompatible_format_version` (ou o código específico do
  artefato) com mensagem clara; nunca migrar automaticamente entre majors.
- **Mesmo major, minor do artefato ≤ minor do leitor** → abre (extensões aditivas).
- **Mesmo major, minor do artefato > minor do leitor** → recusa (recurso futuro).
- Migrações aditivas documentadas (ex.: DBRT/IDMP v1→v2) permanecem explícitas
  no código do artefato; não são “minor implícito”.

### Protocolo

- `Hello.version` / `HelloOk.version` carregam o **major**.
- `Hello.minor` / `HelloOk.minor` são campos aditivos no fim do payload; peers
  antigos sem o campo são lidos como `minor=0`. Bytes após o minor conhecido
  são **ignoráveis** (extensão minor).
- Negociação: majors iguais; minor efetiva = `min(client, server)`.
- Major divergente → `incompatible_protocol_version`.

### API C++ pública

- Headers instalados sob `include/modb/` listados em
  [API_PUBLICA.md](../API_PUBLICA.md) e no `CMakeLists.txt` (`MODB_PUBLIC_HEADERS`).
- Detalhes de I/O nativo, páginas físicas e WAL não fazem parte do contrato
  estável para consumidores externos (podem existir no include tree in-tree).
- Consumidores usam `find_package(moDb CONFIG)` / alvo `modb::modb`.

## Consequências

- Fixtures e testes em `tests/compatibility_test.cpp` cobrem a matriz.
- O projeto em `tests/consumer/` valida compilação apenas contra a API instalada.
- Documentação operacional completa permanece na Fase 10F.
