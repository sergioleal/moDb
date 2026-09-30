# Protocolo para clientes em outras linguagens

Como chamar as stored procedures de um servidor moDb (`modb_add_server`,
ADR-025) de qualquer linguagem: o protocolo nativo por TCP e, na mesma máquina,
o anel de memória compartilhada (ADR-026). Implementação de referência:
[`clients/python/modb_client.py`](https://github.com/sergioleal/moDb/blob/v0.1.1/clients/python/modb_client.py) (só
biblioteca padrão, ~350 linhas), testada contra o `notas-server` em
`modb.python_client`.

Tudo é **little-endian**. `string` = `u32 comprimento | bytes UTF-8`.

## 1. Frame

Toda mensagem, nos dois sentidos, é um frame:

```text
| length u32 | type u8 | payload |      length = 1 + tamanho do payload
```

`length` 0 ou maior que o `max_frame_bytes` do `HelloOk` (16 MiB) é erro de
protocolo. Tipos usados por um cliente de procs:

| type | mensagem | sentido |
|---|---|---|
| 1 | `Hello` | cliente → servidor (primeira mensagem) |
| 2 | `HelloOk` | servidor → cliente |
| 9 | `OpCall` | cliente → servidor |
| 10 | `OpResult` | servidor → cliente |
| 15 | `ShmAttach` | cliente → servidor (minor ≥ 1) |
| 16 | `ShmAttachOk` | servidor → cliente |
| 17 | `Authenticate` | cliente → proxy (minor ≥ 2) |
| 18 | `AuthenticateOk` | proxy → cliente |

Os demais (consultas em *stream*, facades) estão na ADR-010.

## 2. Abertura

```text
Hello    = version u16 (=1) | database string | codecs u8 n, u8[n] (use 1, [0]) | minor u16 (=2)
HelloOk  = version u16 | baseline u64 | codec u8 | max_frame_bytes u32 |
           max_streams u16 | max_expansion u16 | idle_timeout_ms u32 | minor u16 |
           [minor ≥ 2 e o proxy exige autenticação: mechanisms u8 n, string[n]]
```

`database` pode ser vazio (o servidor tem um banco só). O `minor` do `HelloOk` é
o negociado: `≥ 1` significa que o servidor aceita `ShmAttach`. Um `HelloOk` sem
o `minor` no fim é de um servidor antigo (minor 0).

### 2.1 Autenticação (minor ≥ 2, atrás de um `modb-proxy`)

Um proxy com autenticação (ADR-028) anuncia os mecanismos no fim do `HelloOk`.
Até o cliente se autenticar, todo pedido volta recusado com `unauthenticated`
(73); três credenciais recusadas fecham a conexão.

```text
Authenticate   = request_id u32 | mechanism string | payload_len u32 | payload
AuthenticateOk = request_id u32 | ok u8 | code u16 | message string | principal string
```

Mecanismo `token`: o `payload` é o token em UTF-8. O proxy guarda só o SHA-256
dos tokens (`modb-proxy hash-token`). O token viaja em claro: fora de uma rede
confiável, use TLS entre cliente e proxy. Um servidor sem proxy responde
`AuthenticateOk{ok = 0, code = 1}`.

O servidor fecha conexões ociosas depois de `idle_timeout_ms`: um cliente que
fica parado reconecta, e só deve repetir sozinho chamadas de leitura (uma
escrita pode ter sido confirmada antes da queda).

## 3. Chamada de proc

```text
OpCall   = call_id u32 | proc string | args_len u32 | args (Value, §4)
OpResult = call_id u32 | ok u8 | se ok=1: payload_len u32 | payload (Value)
                                 se ok=0: code u16 | message string
```

`call_id` é do cliente (qualquer valor; a resposta repete). As respostas saem na
ordem dos pedidos de uma conexão. `code` é o `modb::ErrorCode`
([`include/modb/error.hpp`](https://github.com/sergioleal/moDb/blob/v0.1.1/include/modb/error.hpp)); os de regra das procs:

| code | nome | significado |
|---|---|---|
| 1 | `invalid_argument` | dados inválidos (argumento ausente ou de tipo errado, regra violada) |
| 30 | `record_not_found` | não existe |
| 48 | `operation_not_found` | proc desconhecida |
| 70 | `conflict` | conflito com o estado atual |
| 71 | `internal_error` | a proc falhou (exceção); a transação foi desfeita |
| 72 | `operation_timeout` | passou do tempo limite do servidor; desfeita |
| 73 | `unauthenticated` | o proxy exige autenticação, ou a credencial foi recusada |
| 74 | `permission_denied` | a política do proxy não permite o pedido |

Os valores numéricos seguem a ordem do enum; confira no `error.hpp` da versão do
servidor (`sys.procs` e `modb procs` listam as procs e os argumentos esperados).

## 4. Value (versão 1)

Argumentos e resultados. Os argumentos são sempre um mapa.

```text
Value  = versão u8 (=1) | valor
valor  = tag u8 | conteúdo
  0 null
  1 bool      u8 (0/1)
  2 integer   i64
  3 real      f64 (IEEE 754)
  4 text      u32 n | n bytes UTF-8
  5 id        u64 (id de objeto; em JSON vira número)
  6 list      u32 n | n valores
  7 map       u32 n | n × (u32 k | k bytes UTF-8 da chave | valor)
```

Limites que o servidor impõe: aninhamento até 64, contagens não maiores que os
bytes restantes, chaves de mapa sem repetição (em qualquer ordem), nada depois
do valor. Onde a proc espera um id, um `integer ≥ 1` também serve.
Args vazios (0 bytes) = mapa vazio.

Exemplo: `{"id": id 5}` =
`01 07 01000000 02000000 6964 05 0500000000000000`.

## 5. Anel de memória compartilhada (mesma máquina)

Para chamadas sem syscall nem troca de thread. O TCP continua aberto (é a linha
de vida) e continua servindo para o resto; só `OpCall`/`OpResult` passam pelo
anel.

```text
ShmAttach   = request_id u32 | ring_bytes u32 (0 = padrão, 1 MiB; 4 KiB..64 MiB)
ShmAttachOk = request_id u32 | ok u8 |
              se ok=1: kind u8 | name string | ring_bytes u32
              se ok=0: code u16 | message string
```

`kind` 1 = mapeamento nomeado do Windows (`OpenFileMapping(name)`; Python:
`mmap.mmap(-1, tamanho, tagname=name)`); 2 = arquivo (`/dev/shm/...` no Linux):
`open` + `mmap`. Tamanho da região = `512 + 2 × ring_bytes`.

Layout da região:

| offset | campo | quem escreve |
|---|---|---|
| 0 | `"MDBSHM01"` (8 bytes) | servidor |
| 8 | `ring_bytes` u32 | servidor |
| 64 | `req_tail` u64 | cliente |
| 128 | `req_head` u64 | servidor |
| 192 | `resp_tail` u64 | servidor |
| 256 | `resp_head` u64 | cliente |
| 320 | `client_state` u32: 1 anexado, 2 saindo | cliente |
| 384 | `server_state` u32: 1 servindo, 2 encerrado | servidor |
| 512 | anel de pedidos (`ring_bytes`) | cliente |
| 512 + ring_bytes | anel de respostas (`ring_bytes`) | servidor |

Depois de mapear, grave `client_state = 1` (o servidor então tira o nome do
sistema de arquivos). Ao sair, `client_state = 2`.

**Escrever** (produtor, com `tail` seu e `head` do outro):

1. `need = align8(tamanho do frame)`; se `need > ring_bytes`, o frame nunca cabe (use o TCP).
2. `off = tail % ring_bytes`, `to_end = ring_bytes − off`; se `need > to_end`,
   o frame vai para o início: `total = to_end + need`, senão `total = need`.
3. Se `tail + total − head > ring_bytes`, o anel está cheio: espere.
4. Se foi para o início, grave `u32 0xFFFFFFFF` (marcador) em `off` e some `to_end` a `tail`.
5. Copie o frame (exatamente o frame do TCP, §1) e **depois** publique
   `tail + need` (store com release).

**Ler** (consumidor): leia `tail` (load com acquire); se `head == tail`, vazio.
Em `head % ring_bytes`: `0xFFFFFFFF` = pule até o fim do anel; senão é um frame
de `4 + length` bytes, contíguo — decodifique no lugar e, ao terminar, publique
`head + align8(4 + length)`.

As posições só crescem (não voltam a 0). Em x86-64 um load/store de 8 bytes
alinhado já tem a ordem necessária; em ARM, use as atômicas da linguagem
(`AtomicU64`, `VarHandle`, `sync/atomic`, `Volatile`).

**Esperar:** gire ~100 µs, ceda a CPU até ~5 ms, depois durma em passos
crescentes (50 µs → 1 ms). Enquanto espera a resposta, confira de vez em quando
`server_state == 2` e a linha de vida TCP (EOF = o servidor morreu): sem isso,
um cliente espera para sempre por um servidor morto.

Uma resposta maior que o anel volta como `OpResult` de erro `value_too_large`;
peça um anel maior no `ShmAttach` ou faça essa chamada pelo TCP.

## 6. Medir

`modb_rpc_bench` mede a mesma proc pelos dois transportes (CSV com ops/s e
p50/p99/p99,9). Números só valem de máquina dedicada.
