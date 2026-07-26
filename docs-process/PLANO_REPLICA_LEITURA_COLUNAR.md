# Plano de replica de leitura com armazenamento colunar selecionavel

- Data: 2026-07-25
- Branch de trabalho: `codex/read-replica-columnar-plan`
- Objetivo: permitir que uma replica de leitura seja criada em modo row-store
  tradicional ou em modo colunar, escolhendo o layout no follower sem alterar o
  formato do primary nem o contrato de commit do Ring0.
- Decisao principal apos diagnostico: com o WAL atual, que e redo-only por
  `page_image`, o MVP honesto de replica colunar e **row follower sombra +
  projecao colunar read-only**. Uma replica colunar pura depende de WAL logico
  ou de um spike que prove extracao barata de objetos alterados a partir das
  page images.

## 1. Diagnostico atual

O codigo de replicacao existente e fisico:

- `WalRecordType` contem `begin`, `page_image`, `commit` e `checkpoint`;
- `apply_wal_records` aplica apenas registros `page_image` de transacoes
  commitadas em `storage::PageFile`;
- `seed_replica_from_wal` cria um arquivo `.modb` vazio e aplica o WAL desde o
  LSN pedido, materializando paginas no follower.

Consequencia: uma `page_image` nao informa diretamente quais objetos mudaram,
quais campos foram alterados, nem quais tombstones logicos precisam ser
refletidos no layout colunar. Para aplicar incrementalmente uma replica
colunar, o follower precisa de uma destas fontes:

1. um row store sombra, atualizado pelo applier fisico atual, do qual a
   projecao colunar deriva mudancas;
2. um mecanismo comprovado para diferenciar page images e extrair objetos
   alterados com custo aceitavel;
3. WAL logico novo, com registros de `object_put`, `object_delete` e
   `schema_baseline`.

O plano adota a opcao 1 para MVP, mas coloca uma Fase A0 curta antes da ADR
para medir se a opcao 2 e viavel. Se o spike falhar, a ADR deve assumir
explicitamente o custo de disco e apply do row store sombra.

## 2. Decisao arquitetural

A opcao de armazenamento pertence ao follower, nao ao primary:

```text
primary full ou wal_only
  |
  | WAL fisico / bootstrap / catch-up
  v
replica read-only
  |
  +-- RowReplicaStorage
  |     arquivo .modb normal
  |     aplica page images como hoje
  |
  +-- ColumnarReplicaStorage MVP
        row follower sombra (.modb interno)
        +
        projecao colunar derivada
        +
        backend colunar para QueryDescription read-only
```

O modo `row` preserva o comportamento atual. O modo `columnar` cria uma replica
read-only otimizada para relatorios, mas nao aceita writes, nao vira primary por
failover e tem custo esperado maior de armazenamento: row store sombra +
segmentos colunares + indices auxiliares.

### 2.1 CLI

Os comandos existentes recebem `--storage row|columnar`:

```text
modb replicate bootstrap primary.modb follower.modb --storage row
modb replicate bootstrap primary.modb follower.colmodb --storage columnar
modb replicate seed-wal follower.colmodb primary.modb.wal primary.modb --storage columnar
modb replicate catch-up follower.colmodb primary.modb.wal primary.modb --storage columnar
modb replicate status follower.colmodb
```

Defaults:

- `--storage row`, para compatibilidade;
- `status` detecta o modo pelo controle do follower;
- erro claro se um comando row tentar abrir follower colunar ou vice-versa.

`seed-wal --storage columnar` evita transferir snapshot de paginas, mas nao
evita materializar paginas: com WAL fisico, o follower ainda precisa reconstruir
o row store sombra antes de projetar colunas.

## 3. O que muda e o que nao muda

Nao muda:

- primary continua single-writer;
- WAL fisico continua sendo a fonte de catch-up do MVP;
- identidade do banco (`uuid`, `timeline`) continua obrigatoria;
- LSN aplicado continua monotonico e idempotente;
- replicas continuam read-only para clientes;
- bootstrap/catch-up continuam detectando gap e exigindo novo bootstrap quando
  o WAL retido nao cobre a lacuna;
- o protocolo publico nao passa a expor paginas ou WAL fora do canal de
  replicacao privilegiado.

Muda:

- o follower passa a ter `ReplicaStorageKind::{row,columnar}`;
- `replicate status` mostra o storage kind e, no modo colunar, tambem o estado
  da projecao;
- bootstrap row copia arquivo de dados;
- bootstrap colunar captura um corte consistente e constroi segmentos por
  varredura logica;
- catch-up colunar aplica WAL fisico no row sombra e depois atualiza a projecao;
- query no follower colunar usa backend alternativo para o mesmo
  `net::QueryDescription`/planner, nao uma API de consulta nova.

Fora do MVP:

- replica colunar parcial por `include_types`/`include_fields`;
- failover/promocao de replica colunar;
- WAL logico;
- snapshots historicos longos no layout colunar;
- facades read-write.

## 4. Modelo de armazenamento colunar

### 4.1 Diretorio do follower

Proposta inicial:

```text
follower.colmodb/
  control.mctl              identidade, timeline, applied_lsn, storage kind
  row-shadow.modb           follower row-store interno aplicado por page image
  row-shadow.modb.wal       WAL local se necessario para recovery/check
  columnar/
    manifest.json           versao selada, applied_lsn, schemas, segmentos
    catalog.modbc           tipos, baselines, FieldId -> column id
    types/
      <type_id>/
        objects.oid         posicao -> ObjectId
        oid_index.*         ObjectId -> posicao atual
        live.bitmap         visibilidade corrente selada
        fields/
          <field_id>.col    valores colunares
          <field_id>.nulls  bitmap de null/default
        segments/
          <segment_id>.meta min/max/null_count/row_count/checksum
```

O diretorio pode ser empacotado depois. Comecar como diretorio reduz risco de
formato, facilita check/reparo e permite rebuild da projecao a partir do row
sombra quando necessario.

### 4.2 Indice `ObjectId -> posicao`

O indice auxiliar nao e detalhe menor: ele esta no caminho quente de apply,
precisa sobreviver a crash e precisa ser idempotente por LSN. O plano deve
trata-lo como subsistema.

Opcoes:

- reusar `index::BTree` se o contrato atender `ObjectId -> RowPosition`;
- criar um indice colunar proprio, com manifest e replay;
- reconstruir o indice no open a partir de `objects.oid` para MVP pequeno,
  aceitando custo de abertura.

Recomendacao: comecar com reconstrução no open apenas se os datasets de teste
forem pequenos; para producao, entregar B-tree ou indice persistente nomeado.

### 4.3 Tipos e encoding

Colunas usam o mesmo contrato logico do object model:

- `FieldId` e `TypeDefinitionId` sao a identidade persistente;
- schema evolution cria nova baseline;
- campos ausentes em objetos antigos sao lidos via default/nullability;
- blobs e colecoes podem ficar como `BlobId`/bytes inicialmente, sem decompor.

O formato colunar inicial deve suportar:

- boolean;
- int64;
- float64;
- string com dicionario simples;
- bytes/blob como referencia ou payload separado;
- refs/owned refs como `ObjectId`.

## 5. Aplicacao do WAL

### 5.1 MVP: row sombra + projecao

Fluxo incremental:

```text
WAL fisico
  -> apply_wal_records(row-shadow.modb)
  -> identificar objetos alterados por spike/diff/indice auxiliar
  -> append/tombstone na projecao colunar
  -> fsync segmentos
  -> selar manifest
  -> avancar control.mctl.applied_lsn
```

O row sombra permite compatibilidade com o WAL atual e fornece fonte logica para
rebuild/check. O custo precisa aparecer nas metricas: bytes do row sombra,
bytes colunares, tempo de apply fisico e tempo de projecao.

### 5.2 Spike: extracao de mudancas de page image

Antes da ADR, medir se e viavel extrair objetos alterados sem row sombra
persistente:

- comparar page image anterior/posterior;
- inspecionar slotted page para registros afetados;
- resolver `ObjectId`/tipo por `IdentityMap` ou estrutura equivalente;
- medir custo por commit, por pagina alterada e por objeto alterado;
- validar schema/tombstone/update/delete.

Se o spike for caro, incompleto ou fragil, ele nao entra no MVP.

### 5.3 Evolucao: WAL logico opcional

WAL logico fica como evolucao com ADR propria:

```text
object_put { lsn, type_id, object_id, epoch, field_values }
object_delete { lsn, type_id, object_id, epoch }
schema_baseline { lsn, baseline_id, type_definitions }
```

Ele reduz custo de apply colunar e melhora `wal_only`, mas altera formato do
WAL e precisa coexistir com recovery row-store.

## 6. Query na replica colunar

A replica colunar deve executar o mesmo contrato remoto declarativo do produto:
`net::QueryDescription` (`type`, `limit`, `equals`, `project`) e o mesmo
conceito de `QueryPlan`/`ProjectionPlan`, com backend alternativo.

Primeiro corte:

- scan de tipo;
- projecao de campos;
- filtro por igualdade compatível com `EqualityFilter`;
- limit;
- snapshot corrente da replica;
- resultados byte/logicamente equivalentes ao follower row.

Depois:

- filtros de range, quando entrarem no contrato de query;
- count/sum/min/max por extensao explicita do contrato ou caminho interno de
  relatorio;
- order by, top-k e distinct;
- predicate pushdown por segmento;
- materializacao parcial para facades read-only.

Pergunta de produto a fechar na ADR: `modb serve follower.colmodb` deve abrir o
servidor normal com backend colunar read-only ou deve haver comando/flag
explicito? A recomendacao e aceitar `serve` detectando o storage kind, mantendo
erros de dominio para operacoes nao suportadas.

## 7. Atomicidade, visibilidade e crash safety

Leitores da replica colunar nunca devem enxergar apply parcial.

Contrato:

- leitores abrem a ultima versao selada no `manifest.json`;
- append de segmentos, `live.bitmap` e tombstones acontecem em area temporaria;
- segmentos e bitmaps sao fsyncados antes do manifest;
- `manifest.json` e escrito em arquivo temporario, fsyncado e renomeado
  atomicamente;
- `control.mctl.applied_lsn` so avanca depois do manifest selado e fsyncado;
- ao abrir apos crash, o follower reusa o ultimo manifest selado e reexecuta
  apply a partir do `applied_lsn` persistido;
- se a projecao ficar suspeita, pode ser descartada e reconstruida do row sombra.

Isso torna `applied_lsn` fronteira de visibilidade, nao apenas progresso de
download.

## 8. Bootstrap colunar

O bootstrap colunar nao deve segurar a barreira do writer durante uma varredura
logica completa.

Fluxo recomendado:

1. abrir barreira curta no primary;
2. capturar `cut_lsn`, epoch, baseline, uuid e timeline;
3. soltar a barreira;
4. varrer com `scan_at` ou `scan_stream(epoch, type)` sob snapshot;
5. fazer uma unica passada com fan-out por tipo/campo quando possivel;
6. escrever segmentos colunares temporarios;
7. selar manifest e persistir `applied_lsn = cut_lsn`.

Evitar varrer `N tipos x heap` no MVP. O filtro por tipo hoje e filtro sobre
varredura de heap; para muitos tipos, uma passada unica com fan-out e mais
honesta para custo e menos agressiva ao primary.

## 9. Replica parcial

`include_types` e `include_fields` ficam fora do MVP. Eles quebram ou complicam:

- checksum logico row vs columnar;
- `replicate check`;
- semantica de query quando um campo/tipo ausente e solicitado;
- rebuild de projecao;
- status operacional.

Quando entrarem, devem virar fase propria com:

- manifest de escopo;
- `status` exibindo tipos/campos incluidos;
- erros de dominio para query fora do escopo;
- checksum parcial;
- regra de alteracao de escopo: rebuild obrigatorio ou backfill controlado.

## 10. Fases de implementacao

### Fase A0 -- Spike de derivacao a partir de page images

Objetivo: decidir se o MVP pode evitar row sombra persistente.

Tarefas:

- [ ] Medir diff de page images em commits com create/update/delete.
- [ ] Validar se slotted pages permitem recuperar registros alterados de forma
      robusta.
- [ ] Medir custo de resolver `ObjectId -> tipo -> campos` por pagina alterada.
- [ ] Documentar quando a extracao falha ou exige estado row-like.
- [ ] Decidir entre row sombra obrigatorio ou WAL logico como pre-requisito.

Criterio de pronto:

- A ADR nao fecha contrato colunar sem numero de custo para a maior incognita.

### Fase A -- Especificacao e ADR-021

Objetivo: fechar contrato antes de formato.

Tarefas:

- [ ] Criar ADR-021: replica colunar derivada, selecionavel por follower.
- [ ] Declarar que a ADR estende ADR-016, ADR-017 e ADR-020.
- [ ] Registrar a escolha do MVP: row sombra + projecao, ou WAL logico se A0
      provar que row sombra e inaceitavel.
- [ ] Definir `ReplicaStorageKind`.
- [ ] Definir extensao de `replicate status`.
- [ ] Definir o que e MVP e o que e erro explicito.
- [ ] Documentar que replica colunar nao e failover target.
- [ ] Posicionar a fase no `docs-process/RASTREADOR.md`.
- [ ] Documentar caminho de saida: rebootstrap row para abandonar replica
      colunar.

Criterio de pronto:

- Um leitor consegue dizer quando escolher `row`, quando escolher `columnar` e
  qual custo extra o MVP colunar assume.

### Fase B -- Controle, modo e durabilidade minima

Objetivo: criar follower colunar vazio, detectavel e crash-safe no controle.

Tarefas:

- [ ] Criar `ColumnarReplicaControl` com uuid, timeline, applied_lsn,
      storage_kind, format version e projection state.
- [ ] Implementar create/open/status.
- [ ] Adicionar `--storage row|columnar` na CLI, default row.
- [ ] Definir ordem de durabilidade: segmentos fsync -> manifest selado ->
      `control.mctl.applied_lsn`.
- [ ] Testar deteccao de modo e erro de comando incompativel.
- [ ] Testar crash antes/depois de avancar `applied_lsn`.

Criterio de pronto:

- `modb replicate status follower.colmodb` mostra `storage=columnar`, LSNs e
  estado da projecao sem expor progresso rasgado.

### Fase C -- Catalogo colunar e indice de identidade

Objetivo: persistir tipos/baselines e localizar linhas por `ObjectId`.

Tarefas:

- [ ] Persistir TypeDefinition ativa por baseline.
- [ ] Mapear `TypeDefinitionId + FieldId` para arquivo de coluna.
- [ ] Definir e implementar `ObjectId -> posicao` como indice nomeado.
- [ ] Suportar schema evolution additive com default/null.
- [ ] Testar leitura de objetos v1 por schema v2.
- [ ] Testar rebuild do indice ou recovery do indice apos crash.

Criterio de pronto:

- A replica colunar sabe quais colunas existem, como ler campos antigos e como
  aplicar update/delete idempotente por `ObjectId`.

### Fase D -- Bootstrap colunar por snapshot logico

Objetivo: construir uma replica colunar a partir de snapshot consistente sem
travar o primary durante toda a varredura.

Tarefas:

- [ ] Capturar `cut_lsn`/epoch/baseline sob barreira curta.
- [ ] Soltar barreira antes da varredura.
- [ ] Varredura com `scan_at` ou `scan_stream(epoch, type)`.
- [ ] Preferir uma passada com fan-out por tipo/campo.
- [ ] Escrever segmentos colunares temporarios.
- [ ] Selar manifest e persistir `applied_lsn = cut_lsn`.
- [ ] Testar contagem e checksum logico vs. primary.

Criterio de pronto:

- `bootstrap --storage columnar` cria follower com mesma contagem logica do
  primary para o escopo MVP sem manter o writer bloqueado pela varredura.

### Fase E -- Query colunar minima pelo contrato existente

Objetivo: provar equivalencia funcional antes de otimizar apply.

Tarefas:

- [ ] Implementar backend colunar para `QueryDescription`.
- [ ] Implementar scan de tipo.
- [ ] Implementar projecao de campos.
- [ ] Implementar filtro simples de igualdade.
- [ ] Implementar limit.
- [ ] Integrar ao `serve`/cliente read-only sem criar API nova.
- [ ] Criar testes comparando resultados row vs columnar.

Criterio de pronto:

- A mesma query remota retorna o mesmo resultado na replica row e na colunar.

### Fase F -- Apply incremental idempotente

Objetivo: manter a replica colunar atualizada por WAL/catch-up.

Tarefas:

- [ ] Aplicar WAL fisico no row sombra.
- [ ] Definir `ColumnarApplier`.
- [ ] Traduzir create/update/delete de objeto para append/tombstone colunar.
- [ ] Atualizar `live.bitmap` e manifest apenas em fronteira de commit.
- [ ] Garantir idempotencia por `applied_lsn`.
- [ ] Detectar gap como `replication_gap`.
- [ ] Testar replay repetido, replay parcial, retomada e crash em cada etapa de
      durabilidade.

Criterio de pronto:

- `catch-up --storage columnar` aplica WAL incremental, pode ser reexecutado sem
  duplicar linhas vivas e nunca expoe estado parcial a leitores.

### Fase G -- Seed de `wal_only`

Objetivo: permitir replica colunar a partir de primary sem transferir snapshot.

Tarefas:

- [ ] Reusar `seed-wal` com `--storage columnar`.
- [ ] Validar uuid/timeline do primary control.
- [ ] Aplicar WAL desde LSN 1 no row sombra.
- [ ] Construir ou atualizar projecao colunar a partir do row sombra aplicado.
- [ ] Testar primary `wal_only` com follower colunar.

Criterio de pronto:

- Um primary `wal_only` consegue criar follower colunar sem transferencia de
  snapshot, mas o plano e os diagnosticos deixam claro que o follower ainda
  materializa estado row sombra com WAL fisico.

### Fase H -- Segmentacao, estatisticas e pushdown

Objetivo: transformar a replica colunar em vantagem real para relatorios.

Tarefas:

- [ ] Dividir colunas em segmentos.
- [ ] Persistir stats por segmento: min, max, null_count, row_count.
- [ ] Pular segmentos por predicate pushdown.
- [ ] Medir TTFR e throughput de scans seletivos.
- [ ] Comparar row follower vs columnar follower incluindo custo do row sombra.

Criterio de pronto:

- Pelo menos um workload analitico mostra ganho reproduzivel e explicado apos
  contabilizar disco e apply extras.

### Fase I -- Compactacao e retencao

Objetivo: controlar crescimento por append/tombstone.

Tarefas:

- [ ] Implementar merge/compactacao offline por tipo.
- [ ] Preservar leitores da ultima versao selada ou recusar compactacao com
      erro claro.
- [ ] Medir tombstone ratio e bytes por objeto vivo.
- [ ] Adicionar comando de diagnostico/reparacao.

Criterio de pronto:

- A replica colunar nao cresce indefinidamente sob churn comum.

### Fase J -- Operacao, check e observabilidade

Objetivo: tornar o recurso operavel.

Tarefas:

- [ ] Documentar backup/restauracao de follower colunar.
- [ ] Expor metrics: applied_lsn, lag, projection_lsn, row_count, tombstones,
      segment_count, bytes por tipo/campo, bytes do row sombra.
- [ ] Adicionar `modb replicate check` para follower colunar.
- [ ] Definir checksum logico row vs columnar considerando apenas escopo MVP.
- [ ] Documentar rebootstrap row como caminho de saida/desfazer.
- [ ] Adicionar exemplos no treinamento/guia operacional.

Criterio de pronto:

- Um operador consegue saber se a replica colunar esta correta, atrasada,
  compactavel, corrompida ou precisa ser reconstruida.

### Fase K -- Replica parcial, se necessario

Objetivo: permitir `include_types`/`include_fields` sem quebrar check/query.

Tarefas:

- [ ] Persistir escopo parcial no manifest.
- [ ] Expor escopo em `status`.
- [ ] Retornar erro de dominio para query fora do escopo.
- [ ] Implementar checksum parcial.
- [ ] Definir rebuild/backfill ao mudar escopo.

Criterio de pronto:

- Replica parcial tem comportamento explicito e verificavel.

### Fase L -- WAL logico, se necessario

Objetivo: remover a dependencia do row sombra quando o custo justificar.

Tarefas:

- [ ] Criar ADR propria para WAL logico coexistindo com recovery fisico.
- [ ] Adicionar registros logicos versionados.
- [ ] Manter compatibilidade com replicas row existentes.
- [ ] Medir apply colunar direto vs row sombra.

Criterio de pronto:

- O follower colunar aplica mudancas logicas sem depender de diff de page image
  nem de row sombra persistente.

## 11. Metricas obrigatorias

| Metrica | Motivo |
|---|---|
| custo do spike page-image diff | decide se row sombra e obrigatorio |
| bootstrap objetos/s e bytes/s | custo inicial de criar replica colunar |
| tempo de barreira no primary | garante que bootstrap nao trava writer |
| apply fisico ms/commit | custo de manter row sombra |
| projection ms/commit | custo incremental colunar |
| replication lag por tempo/LSN | saude operacional |
| query TTFR | vantagem para relatorios interativos |
| scan MB/s e objetos/s | throughput analitico |
| bytes row sombra | custo extra do MVP |
| bytes colunares por objeto vivo | custo do layout colunar |
| tombstone ratio | necessidade de compactacao |
| segmentos pulados por pushdown | prova de uso colunar |
| checksum logico row vs columnar | corretude |
| crash recovery replay time | custo de retomar apos falha |

## 12. Riscos

1. **WAL fisico nao revelar mudancas logicas**: page images podem exigir row
   sombra permanente. Mitigacao: Fase A0 antes da ADR.
2. **Custo dobrado mascarar ganho de query**: row sombra + colunas consomem
   mais disco e apply. Mitigacao: metricas separadas de disco, apply e query.
3. **Bootstrap logico travar primary**: varredura sob barreira longa seria
   pior que copia de arquivo. Mitigacao: capturar epoch/LSN e soltar barreira.
4. **Leitor ver apply parcial**: append/tombstone sem manifest selado rasga
   visibilidade. Mitigacao: contrato de manifest e `applied_lsn` atomico.
5. **Indice `ObjectId -> posicao` virar ponto fragil**: ele e caminho quente e
   precisa recovery. Mitigacao: trata-lo como entregavel da Fase C.
6. **Dois formatos virarem dois bancos**: a replica colunar deve ser derivada e
   read-only. Mitigacao: proibir write/failover no MVP.
7. **Replica parcial quebrar check**: escopo parcial muda corretude. Mitigacao:
   tirar do MVP e entregar fase propria.
8. **Schema evolution complexa**: defaults e campos removidos podem complicar
   leitura. Mitigacao: suportar additive primeiro e recusar o resto.
9. **Crescimento por tombstone**: append-only simplifica apply, mas acumula
   lixo. Mitigacao: Fase I de compactacao antes de producao.

## 13. Criterio de aceite do plano completo

O plano estara concluido quando:

- `--storage row` mantiver comportamento atual;
- `--storage columnar` criar follower read-only com identidade e LSN corretos;
- a ADR-021 registrar explicitamente row sombra ou WAL logico como escolha do
  MVP;
- bootstrap colunar nao segurar o writer durante a varredura completa;
- query colunar reutilizar `QueryDescription`/planner em vez de criar API nova;
- bootstrap e catch-up colunares forem idempotentes e crash-safe;
- primary `wal_only` puder alimentar follower colunar sem transferir snapshot,
  com materializacao row sombra documentada se o WAL continuar fisico;
- leitores enxergarem apenas manifests selados;
- pelo menos um benchmark mostrar ganho real do layout colunar apos contabilizar
  row sombra, apply e disco;
- status/check/backup/rebootstrap estiverem documentados;
- limitacoes do MVP forem erros explicitos, nao fallback silencioso.

## 14. Ordem recomendada

1. Fase A0: spike de derivacao a partir de page images.
2. Fase A: ADR-021 e contrato.
3. Fase B: controle, CLI `--storage` e durabilidade minima.
4. Fase C: catalogo colunar e indice `ObjectId -> posicao`.
5. Fase D: bootstrap colunar por snapshot logico.
6. Fase E: query minima pelo contrato existente.
7. Fase F: apply incremental idempotente e crash-safe.
8. Fase G: seed de `wal_only` sem transferencia de snapshot.
9. Fase H: stats e pushdown.
10. Fase I: compactacao.
11. Fase J: operacao, check e observabilidade.
12. Fase K: replica parcial, se necessario.
13. Fase L: WAL logico, se necessario.

Essa ordem preserva a decisao de produto, mas tira a ambiguidade tecnica: com o
WAL fisico atual, a replica colunar e uma projecao derivada de um follower row.
O spike existe para tentar reduzir esse custo; se ele nao provar viabilidade, o
MVP segue correto, medido e operavel.
