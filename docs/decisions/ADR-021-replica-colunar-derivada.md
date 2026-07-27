# ADR-021 — Réplica de leitura com armazenamento colunar selecionável

## Contexto

As Fases 14, 15 e 16 estabeleceram o mecanismo de replicação física do Ring0 (streaming do WAL, primary `wal_only` e catch-up via manifesto). No entanto, todas as réplicas de leitura operavam exclusivamente no modo *row-store* tradicional. Consultas analíticas e relatórios requerem varreduras e projeções sobre colunas específicas, onde o arranjo em linha causa amplificação de I/O.

## Decisão

O moDb passa a oferecer armazenamento colunar selecionável no *follower*:

1. **Configuração por Follower**: O modo de armazenamento (`ReplicaStorageKind::{row,columnar}`) pertence à réplica e é persistido no arquivo de controle `control.mctl`. O *primary* permanece inalterado.
2. **Row Store Sombra + Projeção Colunar**: No MVP, a réplica colunar mantém um *row store* sombra interno atualizado pelas *page images* do WAL físico e deriva os segmentos colunares.
3. **Leitura e Query Engine**: O servidor (`net::Server`) ao operar sobre um follower colunar redireciona execuções de `net::QueryDescription` para o backend colunar.
4. **Comandos de CLI**: `modb replicate` ganha o parâmetro `--storage row|columnar` (padrão `row`).

## Consequências

- *Followers* colunares são estritamente *read-only* e otimizados para relatórios.
- O formato do *primary* e o contrato do WAL físico não são alterados.
- *Followers* colunares não servem como alvo de *failover*/promoção a *primary*.
- O status da réplica (`modb replicate status`) expõe o tipo de armazenamento e o estado da projeção.
