# Relatório de implementação — check e recovery nas Fases 5 e 6

## Objetivo

Evoluir a verificação e a recuperação para diagnosticar corretamente WAL,
formatos DBRT/IDMP e, depois, as versões MVCC. Diagnóstico deve ser somente
leitura; recuperação e repair são os únicos caminhos que podem mudar arquivos.

## Princípios

1. check nunca reaplica WAL, remove arquivos ou executa migração.
2. recovery reaplica somente transações com commit durável.
3. WAL corrompido falha alto e permanece no disco.
4. Epoch, metadados e dados devem ser publicados pela mesma transação WAL.
5. Arquivo sem DBRT não é um ODB++ migrável.

## Matriz de estados

| Estado | check | recovery na abertura |
|---|---|---|
| Sem WAL | banco limpo | abre normalmente |
| WAL vazio | arquivo descartável | remove o WAL |
| WAL sem commit | transação incompleta | descarta sem aplicar páginas |
| WAL com commit | redo pendente | reaplica, sincroniza e remove WAL |
| WAL ilegível | erro wal_corrupt, preserva | falha sem apagar |
| DBRT/IDMP v1 | formato legável | migra para v2 |
| DBRT/IDMP v2 | formato atual | abre normalmente |
| Sem DBRT | não é ODB++ | falha, sem inicializar raiz |

## Fase 5 — check e recovery

### Check de WAL

Adicionar inspeção somente leitura de arquivo banco.wal:

- informar ausência, vazio ou presença;
- validar cabeçalho, limites, sequência begin, imagens e commit;
- reportar tx id, número de imagens e estado: discard required ou redo required;
- classificar qualquer parse parcial como wal_corrupt;
- nunca corrigir, reaplicar ou apagar durante check.

Exemplo de saída:

~~~
WAL: present
transaction: 42
records: begin=1 page_images=7 commit=1
state: durable commit pending redo
~~~

### Recovery

1. Ler e validar o WAL inteiro antes de gravar páginas.
2. Sem commit: descartar o WAL e não aplicar imagens.
3. Com commit: reaplicar imagens, flush/sync e só depois remover WAL.
4. Falha após commit durável: manter WAL; redo futuro deve ser idempotente.
5. Falha de parse: retornar wal_corrupt e preservar a evidência.

Depois de recovery, executar as validações normais de superbloco, DBRT, heaps,
catálogo e mapa de identidade; remover o WAL não é prova suficiente de saúde.

## Fase 6A — formato e epoch

### Check

Validar:

- magic e versão de DBRT (v1 legável, v2 atual);
- epoch u64 e ausência de wraparound;
- IDMD v1 e IDMP v1/v2 suportados;
- IDMP v2 com entrada de 48 bytes, flags conhecidas e localizações válidas;
- coerência entre catalog_root do superbloco e DBRT.

Saídas sugeridas:

~~~
Format: DBRT v1, IDMP v1
Migration: required
Epoch: unavailable in v1
~~~

~~~
Format: DBRT v2, IDMP v2
Migration: not required
Epoch: 184
~~~

### Recovery e atomicidade

- A página DBRT com epoch + 1 integra as page-images da mesma transação WAL.
- Redo restaura DBRT e dados juntos; epoch observada implica commit durável.
- Rollback descarta o avanço de epoch.
- Em UINT64_MAX, recusar o commit antes de criar WAL ambíguo.

### Migração

1. Validar DBRT/IDMD/IDMP v1.
2. Criar IDMP v2 novo.
3. Copiar entradas, incluindo tombstones.
4. Sincronizar páginas v2.
5. Publicar a nova identity_dir no DBRT.
6. Sincronizar DBRT.

Queda antes do passo 5 mantém a raiz v1 e permite repetir a migração. O DBRT
v1 recebe epoch zero durante a atualização; o primeiro avanço é um commit
posterior.

## Fases 6B e 6C

Em 6B, check deve validar:

- current_epoch não excede a epoch global;
- previous_epoch é menor que current_epoch quando previous existir;
- current e previous apontam para locais válidos;
- remoção versionada preserva previous enquanto houver snapshot dependente.

Recovery precisa restaurar current, previous e epoch pela mesma transação.

Em 6C, check pode identificar previous já inelegível para snapshots, mas não
deve coletá-lo. GC e repair só podem apagar versões após política explícita e
confirmação de que não há snapshot ativo. Na abertura, snapshots não sobrevivem
ao processo; versões previous órfãs podem ser tratadas conforme essa política.

## Limite de db repair

- Pode remover WAL vazio ou sem commit depois de validação completa.
- Não pode remover WAL corrompido.
- Migração de formato deve ser explícita, por exemplo repair --upgrade-format.
- GC MVCC não deve ser disparado por repair genérico antes de 6C.

## Matriz mínima de testes

| Área | Casos |
|---|---|
| WAL | ausente, vazio, sem commit, commit pendente, cabeçalho corrompido, registro truncado |
| Recovery | redo idempotente, queda no apply, falha de flush, falha de remoção |
| DBRT | v1 para v2, reabertura v2, epoch inicial, monotonicidade, overflow recusado |
| IDMP | v1 para v2, tombstone, várias páginas, várias IDMD, sem segunda migração |
| Cruzado | WAL com DBRT epoch e dados; rollback não avança epoch |
| 6B/6C | update/remove versionado, snapshot scan, conflito, GC e reabertura |

## Ordem recomendada

1. Inspeção read-only de WAL no database_check.
2. Saída de DBRT/IDMP/epoch no db check, sem migrar.
3. Failpoints para atomicidade epoch + WAL.
4. Testes de migração interrompida e retomada.
5. Iniciar 6B somente com essa matriz como regressão.

## Critério de aceite

- check separa banco limpo, redo pendente, descarte pendente, corrupção e formato legável.
- Recovery nunca aplica transação sem commit e nunca apaga WAL ilegível.
- Cada commit durável publica uma epoch exatamente uma vez.
- Migração v1 para v2 preserva ObjectIds, RecordIds e tombstones.
- A suíte de falhas, recovery e reabertura passa nas configurações suportadas.
