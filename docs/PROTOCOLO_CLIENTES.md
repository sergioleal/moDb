# Protocolo para clientes em outras linguagens

Como chamar as stored procedures de um servidor moDb (`modb_add_server`,
ADR-025) de qualquer linguagem: o protocolo nativo por TCP e, na mesma máquina,
o anel de memória compartilhada (ADR-026). Implementação de referência:
[`clients/python/modb_client.py`](../clients/python/modb_client.py) (só
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
(`include/modb/error.hpp`). Os que um cliente de procs encontra:

| code | nome | significado | o que o cliente faz |
|---|---|---|---|
| 1 | `invalid_argument` | dados inválidos: argumento ausente ou de tipo errado, regra violada | mostrar ao usuário; não repetir igual |
| 21 | `value_too_large` | pedido maior que o frame, ou resposta maior que o anel (§5) | defeito do cliente; no anel, refazer a chamada pelo TCP ou pedir um anel maior |
| 30 | `record_not_found` | não existe | mostrar ao usuário |
| 44 | `snapshot_conflict` | escrita concorrente num objeto disputado (ADR-027); a transação foi desfeita | o servidor já repete escritas sozinho; se chegar ao cliente, repetir a chamada |
| 45 | `protocol_error` | frame inválido | defeito do cliente; a conexão fecha: reconectar |
| 46 | `frame_too_large` | frame acima do `max_frame_bytes` | defeito do cliente; a conexão fecha: reconectar |
| 47 | `connection_closed` | a conexão caiu (erro local da biblioteca cliente) | reconectar; repetir só leituras |
| 48 | `operation_not_found` | proc desconhecida | defeito: nome errado ou servidor de outra versão |
| 50 | `incompatible_protocol_version` | major do `Hello` incompatível | atualizar o cliente ou o servidor |
| 70 | `conflict` | conflito com o estado atual | mostrar ao usuário |
| 71 | `internal_error` | a proc falhou (exceção); a transação foi desfeita | defeito do servidor: registrar e mostrar erro genérico |
| 72 | `operation_timeout` | passou do tempo limite do servidor; a transação foi desfeita | pode repetir (nada ficou gravado), mas o custo tende a se repetir |
| 73 | `unauthenticated` | o proxy exige autenticação, ou a credencial foi recusada | autenticar; com credencial recusada, não insistir (três recusas fecham) |
| 74 | `permission_denied` | a política do proxy não permite o pedido | mostrar ao usuário; não repetir |

Os números são estáveis: cada código tem um valor explícito no `error.hpp`, e um
código novo entra no fim, sem renumerar nem reusar os outros
([COMPATIBILIDADE.md](COMPATIBILIDADE.md)). Qualquer outro código que chegue a
um cliente de procs é erro interno do servidor: registre e mostre um erro
genérico. A lista completa está no [Apêndice A](#apêndice-a--todos-os-códigos).
`sys.procs` e `modb procs` listam as procs e os argumentos esperados.

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

## Apêndice A — todos os códigos

Gerado do `include/modb/error.hpp`; o teste `modb.error_codes` confere que esta
tabela e o header não divergem.

| code | nome | significado |
|---|---|---|
| 0 | `invalid_identifier` | O nome de uma tabela ou coluna é inválido. |
| 1 | `invalid_argument` | Um argumento fornecido para uma operação é inválido. |
| 2 | `empty_schema` | Foi criado um schema sem colunas. |
| 3 | `duplicate_column` | Duas colunas possuem o mesmo nome. |
| 4 | `column_not_found` | A coluna procurada não existe. |
| 5 | `value_count_mismatch` | A quantidade de valores não corresponde à quantidade de colunas. |
| 6 | `type_mismatch` | O tipo de um valor não corresponde ao tipo da coluna. |
| 7 | `null_constraint_violation` | Uma coluna NOT NULL recebeu NULL. |
| 8 | `duplicate_table` | Já existe uma tabela com o mesmo nome. |
| 9 | `table_not_found` | A tabela procurada não existe. |
| 10 | `file_already_exists` | A criação não pode sobrescrever um arquivo existente. |
| 11 | `file_not_found` | O arquivo solicitado não foi encontrado. |
| 12 | `io_error` | O sistema operacional informou uma falha de entrada ou saída. |
| 13 | `invalid_file_format` | O arquivo não possui a assinatura do moDb. |
| 14 | `incompatible_format_version` | A versão do arquivo não é suportada. |
| 15 | `corrupt_file` | O arquivo está truncado ou possui metadados inconsistentes. |
| 16 | `page_not_found` | O identificador aponta para uma página que não existe. |
| 17 | `reserved_page` | A operação tentou alterar diretamente uma página reservada. |
| 18 | `unexpected_end_of_input` | Os bytes terminaram antes que o valor estivesse completo. |
| 19 | `invalid_encoding` | Os bytes não representam uma codificação reconhecida. |
| 20 | `trailing_data` | Restaram bytes depois que o objeto completo foi decodificado. |
| 21 | `value_too_large` | O valor não cabe nos campos de tamanho do formato. |
| 22 | `too_many_columns` | O schema ultrapassa o limite de colunas do produto. |
| 23 | `invalid_page_format` | A página não contém a assinatura esperada para seu tipo. |
| 24 | `incompatible_page_version` | A versão da estrutura interna da página não é suportada. |
| 25 | `corrupt_page` | Os offsets ou tamanhos internos da página são inconsistentes. |
| 26 | `page_full` | A página não possui espaço livre suficiente para o registro. |
| 27 | `slot_not_found` | O identificador aponta para um slot que não existe. |
| 28 | `record_too_large` | O registro é maior que a capacidade de uma página vazia. |
| 29 | `page_chain_cycle` | Uma cadeia de páginas aponta novamente para uma página já visitada. |
| 30 | `record_not_found` | O RecordId não pertence ao heap consultado. |
| 31 | `duplicate_field` | Duas colunas/atributos de um mesmo tipo usam o mesmo FieldId. |
| 32 | `field_not_found` | O FieldId consultado não existe no tipo. |
| 33 | `duplicate_type` | Já existe um tipo registrado com o mesmo nome. |
| 34 | `type_not_found` | O tipo consultado não existe no registro. |
| 35 | `invalid_object_id` | Um ObjectId/TypeDefinitionId/BaselineId igual a zero foi usado onde um identificador válido (não nulo) era exigido. |
| 36 | `binding_mismatch` | Um tipo C++ já possui outro binding ativo na instância. |
| 37 | `incompatible_projection` | Uma projeção não pôde reconciliar o tipo persistido com o binding atual (conversão de tipo não permitida sem migração registrada). |
| 38 | `transaction_required` | Uma escrita foi tentada sem uma transação ativa (Fase 5). |
| 39 | `transaction_active` | Uma segunda transação foi iniciada com uma já em andamento (single-writer). |
| 40 | `transaction_committed` | A transação já alcançou o ponto de commit durável e não pode mais reverter. |
| 41 | `commit_recovery_required` | O commit está durável no WAL, mas a aplicação local falhou; reabra o banco para a recuperação refazer as páginas pendentes. |
| 42 | `database_recovery_required` | Esta instância observou uma falha depois de um commit durável e só pode voltar a ser usada após reabrir o banco. |
| 43 | `wal_corrupt` | O WAL presente não pode ser interpretado com segurança; ele é preservado para diagnóstico e a abertura do banco é interrompida. |
| 44 | `snapshot_conflict` | Uma segunda alteração do mesmo objeto foi tentada enquanto a versão anterior ainda é visível a um snapshot aberto (Fase 6B: só há uma posição `previous` por objeto — limitação documentada no ADR-009). |
| 45 | `protocol_error` | Frame de protocolo inválido, inconsistente ou hostil (Fase 8). |
| 46 | `frame_too_large` | length do frame excede o máximo negociado / 16 MiB (Fase 8). |
| 47 | `connection_closed` | A conexão de rede foi fechada pelo peer ou pelo transporte (Fase 8). |
| 48 | `operation_not_found` | Operação de domínio não registrada (Fase 9). |
| 49 | `incompatible_module` | Manifesto/módulo incompatível com o runtime ou a allowlist (Fase 9). |
| 50 | `incompatible_protocol_version` | Major de protocolo incompatível na negociação Hello (Fase 10E). |
| 51 | `facade_not_found` | Facade ausente no catálogo (Fase 11). |
| 52 | `facade_method_not_found` | Método invocado não pertence à facade do handle (Fase 11). |
| 53 | `incompatible_facade_version` | Versão de facade incompatível na negociação/lookup (Fase 11). |
| 54 | `invalid_edge` | Campo/membro não forma aresta tipada válida (Fase 12). |
| 55 | `edge_target_not_found` | Alvo da aresta ausente sob o Snapshot (ref órfã; Fase 12). |
| 56 | `graph_limit_exceeded` | Travessia excedeu profundidade/máximo de vértices (Fase 12). |
| 57 | `graph_cycle` | Ciclo detectado onde a topologia não permite (Fase 12). |
| 58 | `replica_read_only` | Escrita/begin/GC em follower read-only (Fase 14). |
| 59 | `replication_gap` | Pedido de LSN abaixo da retenção / gap no stream (Fase 14). |
| 60 | `timeline_mismatch` | timeline_id diverge entre primary e follower (Fase 14). |
| 61 | `database_uuid_mismatch` | DatabaseUuid diverge entre primary e follower (Fase 14). |
| 62 | `bootstrap_required` | Follower precisa de novo bootstrap (Fase 14). |
| 63 | `invalid_instance_config` | Combinação inválida de papel/parâmetro de instância (Fase 15). |
| 64 | `data_files_disabled` | Operação exige arquivo de dados; primary está em wal_only (Fase 15). |
| 65 | `no_data_replica` | Commit wal_only exige réplica de dados e nenhuma está disponível (Fase 15). |
| 66 | `commit_await_replica_timeout` | Timeout aguardando ACK de réplica de dados (Fase 15). |
| 67 | `invalid_replica_state` | Transição ou estado de catch-up inválido para a réplica (Fase 16). |
| 68 | `replica_download_failed` | Falha ao baixar/spoolar segmentos WAL para catch-up (Fase 16). |
| 69 | `manifest_hash_mismatch` | Manifesto ou segmento WAL não bate com o hash/tamanho declarado (Fase 16). |
| 70 | `conflict` | A operação é válida, mas conflita com o estado atual (regra de negócio de uma stored procedure: ex. exemplar já emprestado). Servidor de aplicação, S3. |
| 71 | `internal_error` | Falha interna de uma stored procedure (exceção): a transação foi desfeita. |
| 72 | `operation_timeout` | A stored procedure passou do tempo máximo configurado: a transação foi desfeita. |
| 73 | `unauthenticated` | O cliente não se autenticou, ou a credencial foi recusada (proxy, ADR-028). |
| 74 | `permission_denied` | O cliente autenticado não pode fazer o que pediu (política do proxy, ADR-028). |
