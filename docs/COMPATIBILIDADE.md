# Compatibilidade — Fase 10E

Tag: `0.0.10e` · ADR: [ADR-015](decisions/ADR-015-compatibilidade.md)

## Matriz

| Camada | Major diferente | Minor artefato ≤ leitor | Minor artefato > leitor |
|---|---|---|---|
| Superbloco MODB | `incompatible_format_version` | abre | recusa |
| Protocolo Hello | `incompatible_protocol_version` | negoceia `min` | peer antigo ignora / leitor atual recusa se selecionado acima |

Helpers: `include/modb/compatibility.hpp` (`from_wire_u16`, `ensure_readable`,
`negotiate_protocol_version`).

## Códigos de erro (`ErrorCode`)

O número de cada `ErrorCode` viaja no `OpResult` e é lido por clientes em
outras linguagens, que não recompilam junto com o servidor. Por isso:

- cada código tem um valor explícito em `include/modb/error.hpp`;
- um código novo entra **no fim**, com o próximo número livre;
- nenhum número é renumerado nem reusado, nem quando um código deixa de ser
  usado (ele fica no enum, com o comentário dizendo que está obsoleto);
- a tabela do [PROTOCOLO_CLIENTES.md](PROTOCOLO_CLIENTES.md) (Apêndice A) e as
  constantes do cliente Python acompanham o header.

`modb.error_code_values` fixa os valores no C++; `modb.error_codes` confere o
header contra a documentação e o cliente Python.

## Testes

```powershell
ctest --preset debug -R "modb.compatibility|modb.consumer" --output-on-failure
```

- `modb.compatibility` — matriz unitária + fixtures de superbloco + Hello legado.
- `modb.consumer` — `cmake --install` em prefixo temporário e build do consumidor.

## API pública

Ver [API_PUBLICA.md](API_PUBLICA.md).
