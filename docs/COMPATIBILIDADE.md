# Compatibilidade — Fase 10E

Tag: `0.0.10e` · ADR: [ADR-015](decisions/ADR-015-compatibilidade.md)

## Matriz

| Camada | Major diferente | Minor artefato ≤ leitor | Minor artefato > leitor |
|---|---|---|---|
| Superbloco MODB | `incompatible_format_version` | abre | recusa |
| Protocolo Hello | `incompatible_protocol_version` | negoceia `min` | peer antigo ignora / leitor atual recusa se selecionado acima |

Helpers: `include/modb/compatibility.hpp` (`from_wire_u16`, `ensure_readable`,
`negotiate_protocol_version`).

## Testes

```powershell
ctest --preset debug -R "modb.compatibility|modb.consumer" --output-on-failure
```

- `modb.compatibility` — matriz unitária + fixtures de superbloco + Hello legado.
- `modb.consumer` — `cmake --install` em prefixo temporário e build do consumidor.

## API pública

Ver [API_PUBLICA.md](API_PUBLICA.md).
