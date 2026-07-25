# Relatório de cobertura de testes automatizados — linhas e branches

Data: 2026-07-25. Gerado a partir de uma execução real e completa da suíte
(128/128 testes do CTest passando: 64 executáveis unitários, 58 testes de CLI
ponta-a-ponta, 5 alvos de fuzz replay, 1 teste de consumo da lib instalada),
com o binário instrumentado por `--coverage` (GCC/gcov).

## Metodologia

1. `option(MODB_ENABLE_COVERAGE)` adicionada ao [CMakeLists.txt](../CMakeLists.txt)
   liga `--coverage` no compile e no link quando o compilador é GCC/Clang (ver
   [melhorias.md](../melhorias.md), item B7-b). Preset `coverage` em
   [CMakePresets.json](../CMakePresets.json).
2. `cmake --build --preset coverage` (build completo) + `ctest --preset coverage`
   (128/128 passou) geraram os `.gcno`/`.gcda` para todos os objetos exercitados
   — inclusive os alcançados apenas pelos testes de CLI, consumer e fuzz, não só
   pelos executáveis unitários.
3. **`gcovr` não pôde ser usado nesta máquina** — não há Python instalado (só o
   stub da Microsoft Store). Em vez disso, cada um dos 128 `.gcda` foi processado
   com `gcov -j -b -t` (formato JSON intermediário, com branches, saída em
   stdout), e os resultados foram agregados manualmente por arquivo-fonte via
   script PowerShell (soma de contagem por linha e por branch em todas as
   unidades de tradução que tocam aquele arquivo). Isso é essencialmente o que
   o `gcovr` faz internamente, então os números abaixo são equivalentes aos que
   um relatório `gcovr` produziria.
4. Escopo: apenas arquivos sob `include/modb/` e `src/` (headers de terceiros,
   `tests/`, `examples/`, `benchmarks/` e `apps/` foram excluídos da agregação).

**Limitação conhecida:** para headers puramente template (`.hpp` sem `.cpp`
correspondente), o mesmo trecho de código gera contadores *separados* por
instanciação/unidade de tradução. A agregação soma essas instâncias, o que é
correto para "quantos branches gerados existem e quantos rodaram", mas mistura
instanciações de tipos diferentes numa única porcentagem — trate os números de
`include/modb/**/*.hpp` como indicativos, não tão precisos quanto os de `src/`.

## Totais globais

| Escopo | Arquivos | Linhas cobertas/total | % linhas | Branches cobertos/total | % branches |
|---|---:|---:|---:|---:|---:|
| `src/**/*.cpp` (implementação) | 45 | 6504 / 8318 | **78,2%** | 4882 / 10938 | **44,6%** |
| `include/modb/**/*.hpp` (headers) | 63 | 1888 / 2251 | 83,9% | 7611 / 23850 | 31,9% |

O padrão mais importante do relatório: **cobertura de linhas é razoável, mas
cobertura de branches é fraca em todo o projeto (44,6% em `src/`)**. Isso
confirma numericamente o que a auditoria qualitativa anterior já suspeitava —
os testes exercitam bem o caminho feliz, mas grande parte dos desvios
condicionais (tratamento de erro, casos de borda, corrupção) não é exercitada.
Ver também a pendência de cobertura instrumentada registrada em
[melhorias.md, item B7](../melhorias.md).

## Ranking de prioridade — por onde avançar primeiro

Critério: peso maior para arquivos do núcleo do motor (storage, índice
B-tree, transações/WAL, protocolo de rede) combinado com volume absoluto de
linhas não cobertas e branch % baixo (risco de caminhos de erro não testados).

| # | Arquivo | Linhas não cobertas | % linhas | % branches | Por quê |
|---:|---|---:|---:|---:|---|
| 1 | [src/index/btree.cpp](../src/index/btree.cpp) | 199 de 644 | 69,1% | **38,7%** | Maior gap absoluto do projeto. Índice B-tree é lógica central (splits, merges, rebalanceamento) com muitos ramos condicionais — branch % baixo aqui é o maior risco silencioso do motor. |
| 2 | [src/net/protocol.cpp](../src/net/protocol.cpp) | 198 de 976 | 79,7% | 43,7% | Protocolo binário de rede — maior arquivo do projeto, superfície de desserialização exposta a dados não confiáveis (mesma categoria que os fuzz targets já cobrem parcialmente). |
| 3 | [src/net/replication_protocol.cpp](../src/net/replication_protocol.cpp) | 143 de 241 | **40,7%** | **17,9%** | Pior % de linhas *e* pior % de branches do projeto inteiro entre arquivos não triviais. Tem testes dedicados (`replication_protocol_test.cpp`), mas cobrem uma fração pequena do arquivo — provável concentração de testes no caminho feliz de um protocolo com muitos estados. |
| 4 | [src/object/object_store.cpp](../src/object/object_store.cpp) | 120 de 526 | 77,2% | 44,7% | Núcleo de armazenamento de objetos. |
| 5 | [src/storage/table_heap.cpp](../src/storage/table_heap.cpp) | 110 de 514 | 78,6% | 46,4% | Heap de páginas — apesar de ter 4 executáveis de teste dedicados (churn, reuso de espaço), ainda sobra quase 1/4 do arquivo sem cobertura. |
| 6 | [src/object/database.cpp](../src/object/database.cpp) | 105 de 524 | 80,0% | 48,5% | Fachada central do banco de dados. |
| 7 | [src/net/server.cpp](../src/net/server.cpp) | 91 de 384 | 76,3% | 44,6% | Servidor de rede — caminhos de erro/desconexão provavelmente sub-testados. |
| 8 | [src/object/identity_map.cpp](../src/object/identity_map.cpp) | 91 de 465 | 80,4% | 40,7% | Cache de identidade de objetos. |
| 9 | [src/net/client.cpp](../src/net/client.cpp) | 60 de 262 | 77,1% | 42,0% | **Confirma achado da auditoria anterior**: não tem teste unitário dedicado, só cobertura indireta via testes de servidor — 60 linhas e mais da metade dos branches nunca exercitados diretamente. |
| 10 | [src/net/native_socket.cpp](../src/net/native_socket.cpp) | 46 de 178 | 74,2% | **31,3%** | **Confirma achado da auditoria anterior**: sem teste dedicado; pior % de branches da lista (camada de socket nativo — provável que apenas o caminho de sucesso do syscall seja exercitado). |

### Menções honrosas (piores % relativos, fora do top 10 acima)

- [src/app/server_connection.cpp](../src/app/server_connection.cpp) — **56,5%** de linhas, 25,4% de branches (62 linhas ao todo — pequeno, mas a proporção não coberta é alta).
- [src/ops/operation_registry.cpp](../src/ops/operation_registry.cpp) — 63,6% de linhas.
- [src/object/type_registry.cpp](../src/object/type_registry.cpp) — 70,2% de linhas.
- [src/ops/module_manifest.cpp](../src/ops/module_manifest.cpp) — 72,3% de linhas (outro achado da auditoria anterior confirmado: sem teste dedicado).
- [src/object/database_root.cpp](../src/object/database_root.cpp) — 81,8% de linhas (melhor do que a auditoria anterior sugeria — a cobertura transitiva via `database.cpp` é razoável, mas ainda sem teste isolado da classe raiz).
- [src/object/instance_control.cpp](../src/object/instance_control.cpp) — 84,7% de linhas (também razoável via cobertura transitiva).

## Tabela completa — `src/**/*.cpp`, ordenada por linhas não cobertas (desc)

| # | Arquivo | Linhas não cobertas | % linhas | % branches | Total linhas |
|---:|---|---:|---:|---:|---:|
| 1 | `src/index/btree.cpp` | 199 | 69,1% | 38,7% | 644 |
| 2 | `src/net/protocol.cpp` | 198 | 79,7% | 43,7% | 976 |
| 3 | `src/net/replication_protocol.cpp` | 143 | 40,7% | 17,9% | 241 |
| 4 | `src/object/object_store.cpp` | 120 | 77,2% | 44,7% | 526 |
| 5 | `src/storage/table_heap.cpp` | 110 | 78,6% | 46,4% | 514 |
| 6 | `src/object/database.cpp` | 105 | 80,0% | 48,5% | 524 |
| 7 | `src/net/server.cpp` | 91 | 76,3% | 44,6% | 384 |
| 8 | `src/object/identity_map.cpp` | 91 | 80,4% | 40,7% | 465 |
| 9 | `src/net/client.cpp` | 60 | 77,1% | 42,0% | 262 |
| 10 | `src/object/blob_store.cpp` | 46 | 74,2% | 43,4% | 178 |
| 11 | `src/net/native_socket.cpp` | 46 | 74,2% | 31,3% | 178 |
| 12 | `src/object/database_root.cpp` | 45 | 81,8% | 44,0% | 247 |
| 13 | `src/storage/database_check.cpp` | 41 | 75,5% | 48,8% | 167 |
| 14 | `src/storage/slotted_page.cpp` | 37 | 85,9% | 60,3% | 263 |
| 15 | `src/object/catalog_store.cpp` | 36 | 81,7% | 45,2% | 197 |
| 16 | `src/storage/async_file_windows.cpp` | 34 | 80,8% | 43,2% | 177 |
| 17 | `src/storage/codec.cpp` | 33 | 83,5% | 49,0% | 200 |
| 18 | `src/object/projection_plan.cpp` | 33 | 77,4% | 50,5% | 146 |
| 19 | `src/tx/wal.cpp` | 32 | 84,2% | 42,7% | 203 |
| 20 | `src/storage/page_file.cpp` | 32 | 86,3% | 48,9% | 233 |
| 21 | `src/object/object_codec.cpp` | 30 | 85,1% | 48,3% | 201 |
| 22 | `src/repl/bootstrap.cpp` | 28 | 81,1% | 45,1% | 148 |
| 23 | `src/app/server_connection.cpp` | 27 | 56,5% | 25,4% | 62 |
| 24 | `src/object/index_catalog.cpp` | 24 | 82,4% | 48,0% | 136 |
| 25 | `src/ops/module_manifest.cpp` | 23 | 72,3% | 47,5% | 83 |
| 26 | `src/object/type_registry.cpp` | 20 | 70,2% | 43,3% | 67 |
| 27 | `src/ops/facade_catalog.cpp` | 18 | 73,5% | 49,0% | 68 |
| 28 | `src/storage/native_file.cpp` | 16 | 78,1% | 36,8% | 73 |
| 29 | `src/ops/operation_registry.cpp` | 16 | 63,6% | 37,1% | 44 |
| 30 | `src/object/attribute_value.cpp` | 15 | 82,4% | 48,2% | 85 |
| 31 | `src/object/binding.cpp` | 15 | 80,3% | 44,6% | 76 |
| 32 | `src/object/instance_control.cpp` | 11 | 84,7% | 46,6% | 72 |
| 33 | `src/storage/buffer_pool.cpp` | 7 | 93,8% | 69,3% | 113 |
| 34 | `src/model/data_type.cpp` | 7 | 36,4% | 20,0% | 11 |
| 35 | `src/storage/scratch_page_pool.cpp` | 7 | 82,9% | 57,7% | 41 |
| 36 | `src/tx/recovery.cpp` | 4 | 89,7% | 64,0% | 39 |
| 37 | `src/object/type_definition.cpp` | 4 | 96,1% | 69,5% | 102 |
| 38 | `src/model/value.cpp` | 4 | 77,8% | 25,0% | 18 |
| 39 | `src/model/schema.cpp` | 4 | 92,7% | 62,2% | 55 |
| 40 | `src/text_escape.cpp` | 1 | 92,9% | 70,0% | 14 |
| 41 | `src/storage/binary.cpp` | 1 | 98,2% | 56,0% | 56 |
| 42 | `src/version.cpp` | 0 | 100,0% | n/a | 2 |
| 43 | `src/object/collection.cpp` | 0 | 100,0% | 50,0% | 8 |
| 44 | `src/object/primary_storage.cpp` | 0 | 100,0% | 83,3% | 7 |
| 45 | `src/object/baseline.cpp` | 0 | 100,0% | 71,4% | 12 |

**Nota sobre `src/model/data_type.cpp` (36,4%, 11 linhas):** apesar do %
alarmante, é um arquivo minúsculo (provavelmente só um switch de enum-to-string
usado parcialmente) — baixa prioridade apesar do % ruim, por isso não entrou
no ranking de prioridade acima (que pondera por volume + criticidade).

## Tabela completa — `include/modb/**/*.hpp`, ordenada por linhas não cobertas entre headers com ≥ 15 linhas

| # | Arquivo | % linhas | % branches | Linhas não cobertas | Total linhas |
|---:|---|---:|---:|---:|---:|
| 1 | `include/modb/index/key_codec.hpp` | 63,9% | 8,8% | 22 | 61 |
| 2 | `include/modb/graph/graph_view.hpp` | 70,3% | 40,8% | 22 | 74 |
| 3 | `include/modb/graph/edge_handle.hpp` | 79,4% | 39,4% | 20 | 97 |
| 4 | `include/modb/object/database.hpp` | 80,1% | 29,8% | 134 | 672 |
| 5 | `include/modb/graph/traversal.hpp` | 80,8% | 24,3% | 5 | 26 |
| 6 | `include/modb/graph/algorithms.hpp` | 81,2% | 49,9% | 45 | 239 |
| 7 | `include/modb/ops/facade_handle.hpp` | 81,8% | 33,0% | 6 | 33 |
| 8 | `include/modb/query/planner.hpp` | 83,7% | 48,0% | 7 | 43 |
| 9 | `include/modb/net/client.hpp` | 83,9% | 40,2% | 5 | 31 |
| 10 | `include/modb/object/collection.hpp` | 84,1% | 39,6% | 55 | 346 |
| 11 | `include/modb/object/binding.hpp` | 87,2% | 33,4% | 12 | 94 |
| 12 | `include/modb/object/binding_builder.inl` | 88,9% | 36,2% | 2 | 18 |
| 13 | `include/modb/query/generator.hpp` | 92,7% | 49,4% | 3 | 41 |
| 14 | `include/modb/query/operators.hpp` | 93,8% | 13,6% | 3 | 48 |
| 15 | `include/modb/object/object_store.hpp` | 93,9% | 100,0% | 3 | 49 |

Destaque: `include/modb/index/key_codec.hpp` tem o pior branch % de todo o
projeto (**8,8%**, 49/560) — a codificação de chaves do B-tree tem uma
quantidade enorme de branches gerados (provável por causa de templates para
cada tipo de chave) e quase nenhum é exercitado. Combinado com o baixo branch %
de `btree.cpp` (#1 do ranking), a dupla `key_codec.hpp` + `btree.cpp` é o ponto
mais frágil do índice.

`include/modb/error.hpp` aparece com 0% (0/3 linhas, 0/32 branches) — mas é o
cabeçalho de definição de códigos de erro/exceções; 0% aqui provavelmente
reflete que os construtores de exceção são gerados por linha só quando o tipo
de exceção correspondente realmente é lançado em algum teste, então indica que
nenhum teste força a exceção de `error.hpp` a ser lançada via essa via
específica — vale investigar mas tem prioridade baixa (arquivo de 3 linhas).

## Recomendações concretas

1. **Branch coverage é o gargalo real, não linhas.** Qualquer plano de ação
   deve mirar branches não exercitados (condições de erro, validação de
   corrupção, casos de borda), não apenas "rodar mais uma vez a função".
2. Prioridade 1: **`src/index/btree.cpp` + `include/modb/index/key_codec.hpp`**
   — maior gap absoluto do projeto e pior branch % do índice. Adicionar testes
   direcionados a rebalanceamento/split/merge/underflow do B-tree.
3. Prioridade 2: **`src/net/replication_protocol.cpp`** — pior % de linhas e
   de branches entre arquivos não triviais, apesar de já ter testes dedicados;
   provavelmente os testes existentes cobrem só um subconjunto pequeno de
   estados do protocolo. Revisar `replication_protocol_test.cpp` e adicionar
   casos para os estados/mensagens ainda não exercitados.
4. Prioridade 3: **`src/net/protocol.cpp`** — maior arquivo do projeto com
   maior volume absoluto de linhas não cobertas; já tem fuzzing
   (`fuzz_protocol.cpp`), o que ajuda em branches de corrupção, mas os testes
   determinísticos (`protocol_test.cpp`) parecem cobrir uma fração menor do
   que o esperado para um arquivo desse tamanho.
5. Fechar a lacuna de testes dedicados identificada na auditoria qualitativa
   anterior para **`net/client.cpp`** e **`net/native_socket.cpp`** — os
   números aqui confirmam que a cobertura transitiva via testes de servidor
   não é suficiente (client.cpp: 42,0% de branches; native_socket.cpp: 31,3%,
   o pior branch % entre os "sem teste dedicado").
6. Considerar instalar Python + `gcovr` (`pip install gcovr`) para obter o
   relatório HTML navegável linha-a-linha via
   [scripts/run-coverage.ps1](../scripts/run-coverage.ps1) — os números deste
   relatório foram obtidos por agregação manual do `gcov --json-format`
   porque não há Python nesta máquina.

## Reprodução

```powershell
cmake --preset coverage
cmake --build --preset coverage
ctest --preset coverage
.\scripts\run-coverage.ps1 -SkipBuild -SkipTests
```

(o `run-coverage.ps1` usa `gcovr` se estiver disponível; caso contrário cai
para `.gcov` bruto por arquivo, sem a agregação por módulo feita manualmente
para este relatório.)
