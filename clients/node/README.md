# Cliente Node do Ring0

Cliente de procs em TypeScript, só com a biblioteca padrão do Node, para
servidores gerados por `modb_add_server`, atrás de um `modb-proxy` ou direto.
Implementa o protocolo de [`docs/PROTOCOLO_CLIENTES.md`](../../docs/PROTOCOLO_CLIENTES.md):

- **`Value` v1 completo** (`src/value.ts`): aninhamento até 64, contagens
  conferidas e bytes sobrando recusados. Use `Id` para mandar um id de objeto.
  Inteiros e ids voltam como `number` quando cabem, senão como `bigint`.
- **Conexão** (`src/connection.ts`): `Hello`/`HelloOk` com minor 3, `Authenticate`
  com token, e várias chamadas a caminho numa conexão, casadas pelo `call_id`.
  Do minor 3 (ADR-029): `actingAs` (em nome de quem), `idempotencyKey` e o
  `detail` do erro.
- **Pool** (`src/pool.ts`), no modelo do gateway da `biblioteca`:
  - cada chamada vai para a conexão menos ocupada;
  - uma conexão perto do idle timeout é descartada antes do uso;
  - uma escrita só reaproveita uma conexão usada no último segundo;
  - uma leitura que cai numa conexão reaproveitada é repetida uma vez numa nova;
    uma escrita só é repetida se levar `idempotencyKey`.
- **Erros** (`src/errors.ts`): `ModbError` (com `code`) para erro da proc ou do
  proxy, `ModbConnectionError` para conexão, e `ErrorCode` com os números
  estáveis do §3 do protocolo.

Fora do escopo: o anel de memória compartilhada, que precisaria de um addon
nativo para `mmap`.

Origem: escrito para o backend do registry (`agentikalreg`,
`packages/modb-client`) e trazido para cá como cliente de referência
(`docs-process/PLANO_PROPOSTAS_REGISTRY.md`, R6).

## Requisitos

Node ≥ 22.18, que executa os arquivos `.ts` direto, sem compilar. O código usa
só sintaxe que o Node sabe apagar: nada de `enum`, `namespace` nem propriedades
no construtor.

## Uso

```ts
import { Id, ModbError, Pool } from "modb-client";

const pool = new Pool({ host: "127.0.0.1", port: 7474, token: process.env.RING0_TOKEN, size: 8 });

const { id } = (await pool.call("notas.criar", { texto: "primeira" })) as { id: number };
const nota = await pool.call("notas.ler", { id: new Id(id) }, { read: true });

try {
  await pool.call("notas.criar", { texto: "primeira" });
} catch (e) {
  if (e instanceof ModbError && e.code === 70) {
    // conflict: mostrar ao usuário (HTTP 409)
  }
}
pool.close();
```

Marque as procs de leitura com `{ read: true }`: só elas são repetidas depois de
uma queda de conexão.

### Minor 3: em nome de quem, chave de idempotência, detail

```ts
import { randomUUID } from "node:crypto";

// O gateway (token com a role `delegate`) fala em nome do usuário logado.
const user = { actingAs: `user:${session.userId}`, actingAttributes: { email: session.email } };

// Uma escrita com chave pode ser repetida pelo pool depois de uma queda: o
// servidor devolve o resultado já confirmado em vez de executar de novo.
await pool.call("agents.create", input, { ...user, idempotencyKey: randomUUID() });

try {
  await pool.call("agents.create", input, user);
} catch (e) {
  if (e instanceof ModbError && e.code === 70 && (e.detail as { reason?: string })?.reason === "handle_taken") {
    // 409, marcando o campo (e.detail.field)
  }
}
```

Na proc, o usuário é `c.caller().subject()`, e o `detail` vem de
`c.fail(conflict("..."), Value::object({{"reason", ...}, {"field", ...}}))`.
Contra um servidor de minor ≤ 2, `actingAs` e `idempotencyKey` são recusados
antes de sair do cliente, com `ModbError` 1.

Para usar de outro projeto sem publicar no npm, aponte a dependência para esta
pasta, fixada numa tag do repositório (por exemplo, a `v0.2.0`).

## Testes

```bash
node --test src/value.test.ts
MODB_NOTAS_SERVER=<notas-server> MODB_PROXY=<modb-proxy> node --test test/integration.test.ts
```

No CTest, são o `modb.node_value` e o `modb.node_client`. O teste de integração
sobe o `notas-server` em `--local` atrás de um `modb-proxy` com token e confere:

- a recusa sem token e com token errado (73);
- o `detail` de um erro, a delegação (só com a role `delegate`) e a chave de
  idempotência (a repetição não cria outra nota);
- o principal, uma escrita e uma leitura;
- os erros das procs (1, 30, 48, 70);
- o pool com chamadas em paralelo;
- a queda do proxy: a escrita falha e não é repetida, e a leitura volta quando
  o proxy volta.

O `modb.error_codes` confere o `ErrorCode` deste cliente contra o
`include/modb/error.hpp`. `npm run typecheck` roda o `tsc`, depois de um
`npm install` para buscar o `typescript` e o `@types/node`.
