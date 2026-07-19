# Fixtures de compatibilidade (Fase 10E).

Os cenários de versões anteriores/futuras do superbloco são gerados em
`tests/compatibility_test.cpp` (criar arquivo na versão corrente e patchar o
`u16` de versão). Isso evita binários opacos no repositório e cobre:

- major futuro (wire = 2) → `incompatible_format_version`
- minor futuro empacotado (`1.1` → `0x0102` wait 0x0101) → recusa no leitor 1.0
- Hello legado sem campo minor → `minor = 0`
