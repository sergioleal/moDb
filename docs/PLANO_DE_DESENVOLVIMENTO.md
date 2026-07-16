> ⚠️ **Documento supersedido.** Este é o plano do modelo **relacional**
> abandonado. O plano vigente (banco Orientado a Objetos) está em
> [PLANO_ODB.md](PLANO_ODB.md), detalhado por fase em
> [PROTOCOLO_FASES.md](PROTOCOLO_FASES.md). Mantido apenas para histórico.

# Plano de desenvolvimento do moDb

## 1. Objetivo

Construir um banco de dados relacional embutido em C++26, com armazenamento
persistente, uma interface SQL incremental e garantias de transação e
recuperação adicionadas por etapas.

O plano privilegia entregas verticais: cada marco deve produzir algo testável e
observável, evitando construir muitos subsistemas isolados antes do primeiro
resultado funcional.

## 2. Premissas iniciais

- Linguagem: C++26.
- Build: CMake.
- Produto inicial: biblioteca C++ e executável de linha de comando.
- Plataforma inicial: ambiente de desenvolvimento atual; portabilidade será
  verificada posteriormente.
- Concorrência inicial: um processo e uma thread executando operações no banco.
- Persistência: arquivo local com páginas de tamanho fixo.
- SQL inicial: `CREATE TABLE`, `INSERT` e `SELECT` simples.
- Dependências externas: poucas e justificadas.
- Compatibilidade do formato: arquivos incompatíveis devem ser rejeitados com
  erro claro, nunca interpretados silenciosamente.

## 3. Definição geral de pronto

Uma tarefa só pode ser marcada como concluída quando:

- o código compila sem warnings nos compiladores suportados;
- os testes relevantes foram escritos e estão passando;
- erros esperados são retornados de forma explícita, sem encerrar o processo;
- não há comportamento indefinido conhecido;
- interfaces públicas e decisões não óbvias estão documentadas;
- o código foi formatado e revisado;
- o critério de aceite do marco foi demonstrado.

## 4. Estrutura pretendida

```text
moDb/
|-- CMakeLists.txt
|-- CMakePresets.json
|-- README.md
|-- cmake/
|-- docs/
|-- include/modb/
|-- src/
|   |-- catalog/
|   |-- execution/
|   |-- sql/
|   `-- storage/
|-- apps/modb_cli/
|-- tests/
`-- benchmarks/
```

A estrutura poderá mudar quando houver evidência concreta, mas as dependências
devem apontar das camadas externas para o núcleo, nunca o contrário.

## 5. Marcos e tarefas

### Marco 0 — Requisitos e decisões fundamentais

Objetivo: remover ambiguidades que afetariam o formato de arquivo ou as
interfaces centrais.

Tarefas:

- [x] Definir oficialmente o MVP e os itens que ficam fora dele.
- [ ] Escolher compilador e versão mínima com suporte adequado ao modo C++26.
- [ ] Definir sistemas operacionais inicialmente suportados.
- [x] Definir representação e semântica dos tipos `INTEGER`, `REAL`, `BOOLEAN`,
      `TEXT` e `NULL`.
- [ ] Definir regras iniciais de conversão e comparação entre tipos.
- [x] Definir tamanho da página e limites para texto, linha, tabela e banco.
- [x] Definir byte order, larguras inteiras e regras de alinhamento no arquivo.
- [x] Definir o comportamento para arquivo truncado, corrompido ou de versão
      desconhecida.
- [x] Registrar decisões arquiteturais relevantes em `docs/decisions/`.
- [x] Criar um glossário dos termos usados pelo projeto.

Entregáveis:

- documento de escopo do MVP;
- primeira decisão arquitetural sobre formato e armazenamento;
- matriz de plataformas e compiladores.

Critério de aceite: duas pessoas conseguem ler as decisões e chegar à mesma
interpretação dos tipos, limites e garantias do MVP.

### Marco 1 — Fundação do projeto

Objetivo: obter um projeto mínimo, reproduzível e continuamente testável.

Tarefas:

- [x] Criar o `CMakeLists.txt` raiz exigindo o recurso `cxx_std_26`.
- [x] Separar a biblioteca `modb` do executável `modb_cli`.
- [x] Criar presets de desenvolvimento, debug, release e sanitizers.
- [x] Ativar warnings rigorosos por compilador.
- [ ] Escolher e integrar um framework de testes.
- [x] Configurar `CTest`.
- [x] Adicionar `.gitignore`, `.clang-format` e `.clang-tidy`.
- [x] Criar testes triviais que comprovem build, link e execução.
- [ ] Configurar integração contínua para build e testes.
- [x] Documentar comandos de configuração, compilação e teste.

Entregáveis:

- biblioteca vazia utilizável;
- CLI com comando de versão ou ajuda;
- pipeline executando build e testes.

Critério de aceite: um clone limpo pode ser configurado, compilado e testado
somente com os comandos documentados.

### Marco 2 — Modelo de dados e catálogo em memória

Objetivo: representar valores, linhas, schemas, tabelas e seus metadados sem
persistência.

Tarefas:

- [x] Implementar `DataType`.
- [x] Implementar `Value`, incluindo `NULL`.
- [ ] Definir igualdade, ordenação e hashing de valores.
- [x] Implementar `ColumnDefinition` e restrição `NOT NULL`.
- [x] Implementar `Schema` com nomes de coluna únicos.
- [x] Implementar representação lógica de `Row`.
- [ ] Criar identificadores fortes para banco, tabela, coluna, página e linha.
- [x] Implementar catálogo em memória para criar e localizar tabelas.
- [x] Validar linhas contra o schema antes da inserção.
- [x] Criar testes unitários para tipos, nulos, limites e schemas inválidos.

Entregáveis:

- API em memória para criar tabela e inserir/ler linhas;
- suíte de testes do modelo lógico.

Critério de aceite: uma tabela em memória rejeita valores incompatíveis e
preserva corretamente tipos e `NULL`.

### Marco 3 — Armazenamento paginado

Objetivo: ler e escrever páginas com formato binário explícito e validável.

Tarefas:

- [x] Definir cabeçalho global do arquivo com magic number e versão.
- [ ] Definir cabeçalho comum das páginas.
- [x] Implementar abstração segura de arquivo com leitura e escrita posicionais.
- [x] Implementar criação, abertura, fechamento e sincronização do arquivo.
- [x] Implementar alocação e leitura de páginas por `PageId`.
- [x] Implementar serialização sem copiar a representação interna de objetos C++.
- [x] Detectar leituras curtas, offsets inválidos e cabeçalhos corrompidos.
- [ ] Implementar cache inicial de páginas com política simples de substituição.
- [ ] Definir ownership, pin/unpin e páginas sujas.
- [x] Criar testes com arquivos temporários e reabertura.
- [x] Criar testes de corrupção e limites do arquivo.

Entregáveis:

- gerenciador de arquivo paginado;
- documentação do formato binário inicial.

Critério de aceite: páginas escritas em uma execução são recuperadas sem perda
após o arquivo ser fechado e reaberto.

### Marco 4 — Primeiro caminho vertical sem SQL

Objetivo: completar persistência de catálogo e linhas usando uma API C++ direta.

Tarefas:

- [x] Definir o layout de slotted page para registros de tamanho variável.
- [x] Implementar codificação e decodificação de valores, linhas e schemas.
- [x] Implementar identificação estável de registro (`RecordId`).
- [x] Implementar inserção e leitura de linhas em slotted pages.
- [x] Armazenar registros sem capacidade excedente ao tamanho lógico.
- [x] Implementar atualização, remoção, compactação e reutilização segura de slots.
- [x] Persistir uma raiz `THRP` dedicada com extremos e contadores do `TableHeap`.
- [x] Remover qualquer página de dados vazia da cadeia lógica do `TableHeap`.
- [x] Implementar varredura sequencial das linhas de uma tabela.
- [ ] Persistir schemas e metadados do catálogo.
- [ ] Reconstruir o catálogo ao abrir o banco.
- [ ] Tratar tabela inexistente, duplicada e schema incompatível.
- [ ] Criar teste de integração com centenas de linhas e múltiplas páginas.
- [x] Criar teste de reabertura do banco.

Entregáveis:

- API C++ para criar tabela, inserir e fazer scan;
- arquivo de banco reabrível.

Critério de aceite: o teste cria uma tabela, insere dados, destrói a instância,
reabre o arquivo e recupera exatamente os mesmos dados.

### Marco 5 — Front-end SQL mínimo

Objetivo: transformar texto SQL em uma árvore sintática validada.

Tarefas:

- [ ] Definir formalmente a gramática SQL inicial.
- [ ] Implementar tokens, lexer e posição de origem.
- [ ] Implementar parser e nós da AST.
- [ ] Suportar `CREATE TABLE` com os tipos do MVP.
- [ ] Suportar `INSERT INTO ... VALUES ...`.
- [ ] Suportar `SELECT * FROM ...`.
- [ ] Suportar lista de colunas em `SELECT`.
- [ ] Suportar literais, `NULL` e terminador opcional.
- [ ] Produzir erros com linha, coluna, trecho e descrição.
- [ ] Adicionar testes positivos, negativos e de fuzzing do parser.

Entregáveis:

- biblioteca de parsing independente do armazenamento;
- gramática documentada.

Critério de aceite: todos os comandos do primeiro marco funcional geram ASTs
corretas, e entradas inválidas falham sem crash.

### Marco 6 — Binder, planejamento e execução

Objetivo: validar semanticamente a AST e executar o SQL usando o armazenamento.

Tarefas:

- [ ] Implementar resolução de tabelas e colunas contra o catálogo.
- [ ] Implementar validação e coerção explícita de tipos.
- [ ] Criar representação de plano lógico.
- [ ] Implementar operadores `CreateTable`, `Insert`, `TableScan` e `Projection`.
- [ ] Implementar expressões literais e referências a colunas.
- [ ] Adicionar `WHERE` com comparações e operadores booleanos.
- [ ] Implementar operador `Filter`.
- [ ] Padronizar resultados e diagnósticos da execução.
- [ ] Conectar parser, binder, plano e executor na CLI.
- [ ] Criar testes de ponta a ponta usando comandos SQL.

Entregáveis:

- CLI capaz de executar o primeiro marco funcional;
- pipeline completo de SQL até armazenamento.

Critério de aceite: o exemplo do `README.md` funciona, inclusive depois de
fechar e reabrir o banco.

### Marco 7 — Índices e integridade

Objetivo: evitar scans para buscas por chave e garantir restrições básicas.

Tarefas:

- [ ] Projetar páginas internas e folhas de uma B+ tree.
- [ ] Implementar busca e inserção.
- [ ] Implementar split de páginas e propagação até a raiz.
- [ ] Persistir e recuperar a raiz do índice.
- [ ] Implementar `PRIMARY KEY`.
- [ ] Implementar `UNIQUE`.
- [ ] Completar validação de `NOT NULL`.
- [ ] Fazer o planejador escolher índice para predicados elegíveis.
- [ ] Criar testes de invariantes estruturais da árvore.
- [ ] Criar testes com inserções aleatórias, duplicatas e reabertura.

Entregáveis:

- índice B+ tree persistente;
- restrições básicas de integridade.

Critério de aceite: buscas por chave usam o índice, duplicatas são rejeitadas e
a árvore permanece válida após reabrir o arquivo.

### Marco 8 — Transações e recuperação

Objetivo: oferecer atomicidade e durabilidade mesmo diante de interrupções.

Tarefas:

- [ ] Definir estados e API de transação.
- [ ] Adicionar `BEGIN`, `COMMIT` e `ROLLBACK`.
- [ ] Definir formato do write-ahead log (WAL).
- [ ] Garantir que o WAL seja sincronizado antes das páginas de dados.
- [ ] Implementar recuperação ao abrir o banco.
- [ ] Implementar rollback das operações suportadas.
- [ ] Definir estratégia de checkpoint.
- [ ] Definir política de lock inicial.
- [ ] Testar falhas simuladas em cada ponto crítico de escrita.
- [ ] Testar atomicidade, durabilidade e repetição da recuperação.

Entregáveis:

- gerenciador de transações;
- WAL, recuperação e checkpoints;
- relatório das garantias oferecidas.

Critério de aceite: após interrupção simulada, toda transação aparece completa ou
ausente; nunca parcialmente aplicada.

### Marco 9 — SQL relacional ampliado

Objetivo: tornar o banco útil para consultas relacionais básicas.

Tarefas:

- [ ] Implementar `DELETE`.
- [ ] Implementar `UPDATE`.
- [ ] Implementar aliases.
- [ ] Implementar `INNER JOIN` inicialmente por nested loop.
- [ ] Implementar `ORDER BY`.
- [ ] Implementar `LIMIT` e `OFFSET`.
- [ ] Implementar `COUNT`, `MIN`, `MAX`, `SUM` e `AVG`.
- [ ] Implementar `GROUP BY`.
- [ ] Implementar regras simples de otimização de planos.
- [ ] Expandir testes de semântica de `NULL`.

Entregáveis:

- subconjunto SQL documentado;
- executor com operadores relacionais essenciais.

Critério de aceite: uma suíte de cenários relacionais documentados produz
resultados determinísticos e compatíveis com a semântica definida pelo projeto.

### Marco 10 — Desempenho e estabilização

Objetivo: medir, otimizar e preparar interfaces estáveis.

Tarefas:

- [ ] Criar benchmarks reproduzíveis de inserção, scan e busca indexada.
- [ ] Adicionar métricas do buffer pool e do executor.
- [ ] Fazer profiling antes de cada otimização relevante.
- [ ] Otimizar serialização e representação de linhas com base em medições.
- [ ] Testar bancos maiores que a memória configurada para o cache.
- [ ] Estabelecer limites de memória e tempo para consultas.
- [ ] Definir política de compatibilidade do formato de arquivo.
- [ ] Estabilizar a API pública C++.
- [ ] Documentar backup, restauração e diagnóstico.
- [ ] Criar guia de contribuição e estratégia de releases.

Entregáveis:

- relatório de benchmarks;
- API e formato versionados;
- documentação para usuários e contribuidores.

Critério de aceite: os resultados são reproduzíveis, regressões relevantes são
detectadas automaticamente e as interfaces públicas estão documentadas.

## 6. Itens deliberadamente fora do MVP

- protocolo de rede cliente/servidor;
- múltiplos processos escrevendo simultaneamente;
- replicação e alta disponibilidade;
- execução distribuída;
- otimização baseada em custos e estatísticas sofisticadas;
- procedures, triggers e extensões completas de SQL;
- criptografia transparente do arquivo;
- compatibilidade integral com outro banco de dados.

Esses itens só devem entrar no plano depois que persistência, recuperação e
integridade do núcleo estiverem comprovadas.

## 7. Estratégia de testes

Cada camada terá testes proporcionais ao risco:

- testes unitários para tipos, layouts, parser e estruturas de dados;
- testes de propriedades para serialização, valores e B+ tree;
- fuzzing para parser e decodificação de arquivos não confiáveis;
- testes de integração para o caminho SQL completo;
- testes de reabertura para toda informação persistida;
- testes de corrupção para entradas inválidas e arquivos truncados;
- testes de falha para WAL, commit e recuperação;
- sanitizers em builds dedicados;
- benchmarks separados dos testes funcionais.

Arquivos temporários devem ser isolados por teste. Um teste nunca deve depender
da ordem de execução ou de resíduos deixados por outro.

## 8. Regras arquiteturais iniciais

- O formato em disco não deve depender de padding, endianness ou ABI do C++.
- Dados vindos do arquivo ou do SQL são sempre considerados não confiáveis.
- O parser não acessa o armazenamento.
- O executor não interpreta texto SQL diretamente.
- O catálogo é a fonte de verdade para nomes, schemas e objetos persistentes.
- Operações de I/O retornam erros ricos; não encerram o processo.
- Otimizações não podem alterar a semântica observável.
- Recursos experimentais de C++26 devem ficar isolados se afetarem
  portabilidade entre compiladores.

## 9. Ordem recomendada para iniciar

1. Concluir as decisões do Marco 0.
2. Montar build e testes do Marco 1.
3. Implementar o modelo lógico do Marco 2.
4. Construir armazenamento e o caminho vertical dos Marcos 3 e 4.
5. Somente então conectar o front-end SQL nos Marcos 5 e 6.

Não iniciar índice ou transações antes de existir um teste confiável de
persistência e reabertura. Cada novo marco deve preservar os testes e garantias
dos anteriores.
