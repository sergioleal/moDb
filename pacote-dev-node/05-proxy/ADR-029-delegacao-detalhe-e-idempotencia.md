# ADR-029 — Protocolo minor 3: delegação, detalhe nos erros e idempotência

- Estado: aceito
- Data: 2026-09-30
- Plano: [PLANO_PROPOSTAS_REGISTRY.md](https://github.com/sergioleal/moDb/blob/v0.2.0/docs-process/PLANO_PROPOSTAS_REGISTRY.md), P2 (R7–R12)
- Relacionados: [ADR-015](https://github.com/sergioleal/moDb/blob/v0.2.0/docs/decisions/ADR-015-compatibilidade.md) (compatibilidade),
  [ADR-025](https://github.com/sergioleal/moDb/blob/v0.2.0/docs/decisions/ADR-025-servidor-de-aplicacao-com-procedures-compiladas.md) (procs),
  [ADR-027](https://github.com/sergioleal/moDb/blob/v0.2.0/docs/decisions/ADR-027-leitores-concorrentes-escritor-exclusivo.md) (um escritor),
  [ADR-028](https://github.com/sergioleal/moDb/blob/v0.2.0/docs/decisions/ADR-028-proxy-de-acesso-remoto.md) (proxy)

## Contexto

O backend do registry mostrou três faltas no protocolo de procs (propostas 1, 2
e 9):

1. **Quem é o usuário.** Num gateway web com pool, cada conexão carrega um
   principal só: o do gateway. As procs recebiam o usuário num argumento, e a
   confiança nele era uma convenção de cada proc; auditoria e limites do proxy
   não viam o usuário.
2. **Erros sem estrutura.** `OpResult` de erro leva só `code` e `message`. Três
   casos diferentes caíam no mesmo `conflict`, e o campo inválido ia escrito
   dentro da mensagem.
3. **Escrita que não pode ser repetida.** Se a conexão cai depois do commit e
   antes da resposta, o cliente não sabe se a escrita aconteceu. Por isso os
   clientes nunca repetem escritas.

## Decisão

Um minor novo do protocolo, **3**, com três extensões. Nada muda para quem
negociou minor ≤ 2.

### Regra de compatibilidade

Os campos novos vão **no fim** das mensagens, e **só quando têm valor**. Quem
manda decide pelo minor negociado:

- cliente: só usa os campos novos se o `HelloOk` disse minor ≥ 3;
- engine e proxy: só mandam `detail` para uma sessão de minor ≥ 3;
- proxy: negocia com o cliente `min(cliente, proxy, engine)`.

Um peer de minor ≤ 2 recusa bytes sobrando (`trailing_data`): nunca os recebe.
O decoder de minor 3 aceita a mensagem com ou sem a extensão.

### 1. Delegação: em nome de quem

`OpCall`, `Query`, `FacadeList` e `FacadeOpen` ganham uma extensão opcional:

```text
extensão = flags u8 |
           [flags & 1: acting_as string | n u8 | n × (chave string, valor string)] |
           [flags & 2, só OpCall: idempotency_key string]
```

Bits desconhecidos em `flags` → `protocol_error`. Atributos: até 32; chave de
idempotência: 1 a 128 bytes.

- **Só o proxy autoriza.** Um principal pode delegar se tiver a role
  `delegate` (no arquivo de tokens). Sem ela, o proxy recusa o pedido com
  `permission_denied` antes do engine. A regra é do proxy, fora da cadeia de
  políticas: nenhuma política configurada a desliga.
- **O engine confia no link**, como já confia no principal (ADR-028). No modo
  TCP direto não há quem autorize: um `OpCall` com `acting_as` volta com
  `permission_denied`.
- **A proc vê os dois.** `ops::Caller` ganha `acting_as` e `acting_attributes`;
  `subject()` devolve `acting_as` ou, sem delegação, o principal.
- **Política e auditoria veem o par.** O `authorize` recebe o chamador efetivo
  (principal + delegado). O limite de chamadas por segundo passa a valer por
  delegado quando há delegação, e `max_delegated_calls_per_second` limita a
  soma de um principal sobre todos os seus delegados. Assim, um gateway
  comprometido não contorna o limite inventando usuários. A allowlist continua
  decidindo pelo principal e pelas roles: o delegado não tem roles no proxy.
- **Log e auditoria:** `by <principal> as <delegado>`.
- **Facades:** os métodos de uma facade são `OpCall`, então herdam a
  delegação; `FacadeList`/`FacadeOpen` a levam para a política e a auditoria.

### 2. `detail` nos erros

```text
OpResult (ok = 0) = call_id u32 | ok u8 | code u16 | message string |
                    [minor ≥ 3 e há detalhe: detail_len u32 | detail (Value)]
```

- Na proc: `return c.fail(conflict("..."), Value::object({{"reason", "handle_taken"}, {"field", "handle"}}));`.
- O detalhe fica na camada das procs, e não no `modb::Error`: o `Error` é o tipo
  base do motor, e um `ops::Value` nele faria a camada mais baixa depender de
  `ops`. O `ExecutionContext` guarda o detalhe da chamada, e o
  `OperationRegistry` o devolve junto com o erro.
- Convenção documentada (não obrigatória): `reason` (`[a-z_]+`, estável) e
  `field` (caminho com [`.`](https://github.com/sergioleal/moDb/tree/v0.2.0/.)). O log de chamadas grava o `reason`.
- Só em erro: num sucesso, o detalhe é descartado.

### 3. Chave de idempotência

`OpCall` com `flags & 2` leva uma chave escolhida pelo cliente (um UUID por
escrita lógica).

- Numa proc de escrita, o engine procura a chave **na mesma transação** da
  chamada. Se já existe, devolve o resultado gravado sem executar de novo. Se
  não, executa e, se der certo, grava chave e resultado antes do commit.
- Como a gravação está na transação da escrita, vale depois de uma queda do
  engine: o que foi confirmado tem a chave junto; o que não foi não tem.
- A chave vale por principal (a mesma chave de dois principais são duas).
- Erros não são gravados (a transação foi desfeita): repetir executa de novo.
- Numa proc de leitura, a chave é ignorada.
- Retenção: `idempotency_retention_s` no servidor (padrão 86400). Cada escrita
  apaga até 16 registros vencidos.
- Um resultado maior que 4 KB não cabe no registro (um objeto precisa caber
  numa página, R14). A chave é gravada sem ele, e a repetição recebe
  `conflict` com `detail.reason = "idempotent_result_too_large"`: a escrita não
  acontece duas vezes, mas o resultado não é repetido.
- Com isso, o pool pode repetir uma escrita que leva chave.

## Alternativas avaliadas

| Opção | Por que não |
|---|---|
| Uma conexão por usuário | handshake e autenticação por usuário; o Ring0 teria que emitir tokens por usuário |
| Asserção assinada (JWT) conferida pelo proxy | mais forte, mas acopla o proxy ao provedor de identidade; pode vir depois como outro mecanismo |
| `Ping`/`Pong` para validar a conexão | só estreita a janela entre o teste e a escrita; a escrita continua sem poder ser repetida |
| Chave de idempotência só em memória | uma queda do engine entre o commit e a resposta perderia a chave, e a repetição duplicaria a escrita |
| `detail` dentro do `modb::Error` | faria o tipo base do motor depender de `ops::Value` |

## Consequências

- `protocol_minor` passa a 3. Clientes e servidores de minor 2 continuam se
  entendendo com os de minor 3 (teste de compatibilidade nos dois sentidos).
- O engine ganha um tipo de sistema, `sys.Idempotency`, no arquivo do banco
  (chave, principal, resultado, instante). Um banco que nunca recebeu chave não
  tem nenhum registro dele.
- Cada escrita com chave faz uma busca no índice e grava um objeto a mais.
- Os clientes de referência (Python, Node, C++) expõem `acting_as`,
  `idempotency_key` e o `detail` do erro. O pool Node repete uma escrita só se
  ela levar chave.
