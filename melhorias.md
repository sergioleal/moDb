# Propostas de melhorias — moDb

Documento gerado a partir de uma auditoria do código em quatro dimensões (performance,
segurança/robustez, uso do C++ e build/testes), com cada achado verificado contra o código
real. Achados que não resistiram à verificação foram descartados e estão listados no
apêndice ao final.

O contexto foi respeitado: o moDb é um MVP educacional, single-thread e single-process por
decisão de escopo ([ESCOPO_MVP.md](docs/ESCOPO_MVP.md)), com WAL e transações explicitamente
fora do MVP. As propostas abaixo não pedem que o projeto vire um banco de produção; elas
atacam (a) armadilhas latentes que vão custar caro depois, (b) desperdício estrutural de
I/O/CPU que contradiz o propósito didático de um motor de armazenamento, e (c) lacunas de
tooling que reduzem a rede de segurança do próprio desenvolvimento.

## Sumário priorizado

| # | Status | Prioridade | Tema | Item | Esforço |
|---|--------|-----------|------|------|---------|
| S1 | ✅ Feito | **P0** | Robustez | Crash no meio de `allocate_page` deixa o arquivo permanentemente inabrível | baixo |
| S2 | ✅ Feito | **P0** | Robustez | Superbloco reconstruído do zero apaga o campo `catalog_root` (armadilha latente) | baixo |
| S3 | ✅ Feito | **P0** | Robustez | `static_assert` permite `MODB_PAGE_SIZE` entre 29 e 36, que faz `slotted_page_max_record_size` dar wrap unsigned | baixo |
| S4 | ✅ Feito | **P0** | Segurança | `decode_row` faz `reserve` por contagem não validada e não aplica `max_columns_per_table` | baixo |
| S5 | ✅ Feito | **P0** | Segurança | Injeção de sequências de escape de terminal ao imprimir `TEXT` de um `.db` não confiável | baixo |
| B1 | ✅ Feito | **P0** | Tooling | Sanitizers dão `FATAL_ERROR` justamente no Windows, a plataforma primária | baixo |
| P1 | 🟡 Parcial | **P1** | Performance | Amplificação de escrita: `persist_root` a cada insert/erase + superbloco reescrito a cada alocação | médio |
| P2 | ⏸️ Adiado (design) | **P1** | Performance | `TableHeap::open` valida a cadeia inteira e a CLI reabre tudo a cada comando | médio |
| P3 | ✅ Feito | **P1** | Performance | Scan lê cada página O(registros) vezes em vez de O(páginas) | baixo |
| P4 | ✅ Feito (parcial) | **P1** | Performance | Toda operação copia a página 2× (4 KB) e reexecuta `validate_page` (sort + alocação) | médio |
| P5 | ✅ Feito | **P1** | Performance | `TableHeap::update` lê e valida a mesma página duas vezes | baixo |
| P6 | ✅ Feito (parcial) | **P1** | Performance | Busca de página com espaço percorre o `unordered_map` inteiro, em ordem não determinística, mutando-o durante a iteração | médio |
| S6 | ✅ Feito | **P1** | Robustez | `flush()` não garante durabilidade: falta `FlushFileBuffers`/`fsync` | médio |
| S7 | ✅ Feito (parcial) | **P1** | Robustez | `database_check`: custo quadrático com múltiplas raízes THRP e não detecta cadeias compartilhadas nem payloads corrompidos | médio |
| S11 | ✅ Feito | **P1** | Robustez | `modb db repair`: modo de reparo/recuperação que reconstrói a raiz a partir da cadeia e reconcilia páginas órfãs | médio |
| P7 | ⬜ Pendente | **P2** | Performance | `erase` compacta sempre, e `compact` copia os 4096 bytes inteiros | médio |
| P8 | ⬜ Pendente | **P2** | Performance | `BinaryWriter` cresce byte a byte, sem `reserve` | baixo |
| P9 | ⬜ Pendente | **P2** | Performance | Varreduras O(slots) redundantes em `record_count`/`insertion_capacity`/`insert` | baixo |
| P10 | ✅ Feito | **P2** | Performance | `ScratchPagePool` paga mutex + condition_variable por load em motor single-thread | baixo |
| C1 | ✅ Feito | **P2** | C++ | Codec little-endian implementado três vezes | médio |
| C2 | ✅ Feito | **P2** | C++ | Cadeias de `get_if` onde `std::visit` exaustivo seria verificado em compilação | baixo |
| C3 | ✅ Feito (parcial) | **P2** | C++ | `Value{ptr}` converte qualquer ponteiro em BOOLEAN silenciosamente; faltam accessors tipados | baixo |
| C4 | ✅ Feito | **P2** | C++ | `Page::operator[]` usa `array::at()` e lança exceção, contrariando a política de `std::expected` | baixo |
| C5 | ⏸️ Adiado (design) | **P2** | C++ | `PageFile` sem `close()`: o destrutor engole erros de flush; move `noexcept` forçado | baixo |
| C6 | ✅ Feito | **P2** | C++ | `ScratchPagePool` lança exceção no construtor e `acquire()` bloqueia sem timeout | baixo |
| C7 | ⬜ Pendente | **P2** | C++ | `main.cpp` monolítico com 1514 linhas e ~65 linhas de forward declarations | alto |
| B2 | ⬜ Pendente | **P2** | Build | 12 blocos de alvo de teste copiados e colados no CMakeLists | baixo |
| S8 | ⏸️ Adiado (formato) | **P3** | Segurança | Sem checksum por página: corrupção de payload passa por todas as validações | médio |
| S9 | ⏸️ Adiado (plataforma) | **P3** | Segurança | Sem lock de arquivo: dois processos podem corromper o mesmo `.db` | médio |
| S10 | ⏸️ Pós-MVP | **P3** | Robustez | Operações multi-página sem atomicidade e sem modo de reparo | alto |
| B3 | ⏸️ Bloqueado (clang) | **P3** | Testes | Fuzzing ausente para o parsing de arquivo não confiável | médio |
| B4 | ⬜ Pendente | **P3** | Testes | Framework de teste artesanal sem isolamento nem granularidade | médio |
| B5 | ✅ Feito (parcial) | **P3** | Build | Preset release não roda testes; sem LTO nem flags de hardening | baixo |
| B6 | ✅ Feito (parcial) | **P3** | Tooling | `.clang-tidy` desabilita o `clang-analyzer-*` e não é executado pelo build | baixo |
| B7 | ⏸️ Bloqueado (infra) | **P3** | Build | CI ausente; cobertura ausente; biblioteca não instalável (`install()`/export) | médio |
| B8 | ✅ Feito | **P3** | Higiene | `.gitignore` não cobre `*.db` (há um `loja.db` solto na raiz) | baixo |

Legenda de prioridade: **P0** = corrigir antes de qualquer coisa (perda de dados/armadilha
latente, esforço baixo). **P1** = maiores ganhos estruturais. **P2** = qualidade e ergonomia.
**P3** = investimento contínuo/pós-MVP.

Legenda de status: **✅ Feito** = implementado, testado, suíte verde. **✅ Feito (parcial)** /
**🟡 Parcial** = núcleo do item entregue e testado; um sub-item de menor valor ou maior risco
fica anotado como continuação. **⏸️ Adiado** = não feito por depender de decisão de design,
de infraestrutura externa (CI/remote), de toolchain ausente (clang/libFuzzer) ou por ser
pós-MVP; a razão está na seção de detalhe. **⬜ Pendente** = ainda não iniciado.

Todos os itens marcados como feitos foram compilados com `-Werror` e validados pela suíte
completa (26 testes, incluindo a bateria de churn de 10k registros); vários também têm
validação ponta a ponta na CLI. Nada foi commitado — as mudanças estão na working tree.

---

## 1. Segurança e robustez

### S1. Crash no meio de `allocate_page` torna o arquivo permanentemente inabrível — **P0** — ✅ Feito

**Status:** implementado em [page_file.cpp](src/storage/page_file.cpp) (`validate_superblock`
agora tolera páginas físicas além da contagem declarada) com regressão em
[storage_test.cpp](tests/storage_test.cpp) ("orphan trailing page"). Suíte completa passa.

**Onde:** [page_file.cpp:297](src/storage/page_file.cpp:297) (ordem das escritas) e
[page_file.cpp:188](src/storage/page_file.cpp:188) (validação).

**Problema:** `allocate_page` primeiro estende o arquivo escrevendo a página nova e só
depois grava o superbloco com a contagem atualizada. Se o processo cair entre as duas
escritas, o arquivo fica com N+1 páginas físicas e o superbloco declarando N.
`validate_superblock` exige igualdade exata (`stored_page_count * page_size != file_size`)
e recusa abrir com `corrupt_file` — para sempre, pois não existe ferramenta de reparo. Um
único crash em momento inoportuno bloqueia o acesso a todos os dados íntegros.

**Proposta:** tolerar `file_size >= stored_page_count * page_size` (mantendo a exigência de
alinhamento a `page_size`) e tratar páginas de cauda além da contagem como não alocadas —
truncando na abertura ou reutilizando na próxima alocação. Complementarmente, oferecer um
modo de reparo no `modb db check` que reconcilie a contagem com o tamanho físico.

### S2. Superbloco reconstruído do zero apaga o `catalog_root` — **P0** — ✅ Feito

**Status:** implementado. `make_superblock` passou a receber a raiz do catálogo; `PageFile`
guarda um membro `catalog_root_` lido no `open` e preservado por `write_superblock`. Foi
adicionada a API pública `catalog_root()` / `set_catalog_root()` (com validação de página 0
e inexistente e rollback em falha de escrita), coberta por regressão em
[storage_test.cpp](tests/storage_test.cpp) ("catalog root").

**Onde:** [page_file.cpp:141](src/storage/page_file.cpp:141) (`make_superblock`) e
[page_file.cpp:384](src/storage/page_file.cpp:384) (`write_superblock`).

**Problema:** `make_superblock` sempre grava `no_page` no offset 20 (`catalog_root_offset`),
e `write_superblock` — chamado por **cada** `allocate_page` — reconstrói a página zero
inteira por esse caminho. No dia em que alguém gravar a raiz do catálogo nesse campo
(previsto no [FORMATO_DE_ARQUIVO.md](docs/FORMATO_DE_ARQUIVO.md)), a primeira alocação
seguinte o zera silenciosamente, e `validate_superblock` nem lê o campo para detectar.
É uma armadilha de perda de dados embutida no design da API interna.

**Proposta:** fazer o superbloco ser lido-modificado-escrito (ou manter um espelho em
memória com todos os campos) e atualizar apenas `page_count`; alternativamente, tornar
`catalog_root` membro de `PageFile`, passar a `make_superblock(page_count, catalog_root)` e
expor `Result<void> set_catalog_root(PageId)`. Resolver junto com S1, pois ambos tocam a
mesma rotina.

### S3. `MODB_PAGE_SIZE` entre 29 e 36 causa wrap unsigned no limite de registro — **P0** — ✅ Feito

**Status:** implementado. Adicionado `static_assert(page_size >= slotted_page_header_size +
slotted_page_slot_size + 1)` em [slotted_page.hpp](include/modb/storage/slotted_page.hpp),
onde as constantes existem, antes do cálculo que dava wrap. Verificado por build: page size
32 agora **falha na compilação** com a mensagem do assert; 37 (o novo mínimo) e o padrão 4096
compilam e a suíte passa (22/22). O `static_assert` de [page.hpp](include/modb/storage/page.hpp:22)
foi mantido como piso do superbloco (offsets de 16 bits e ≥ 29 bytes).

**Onde:** [slotted_page.hpp:30](include/modb/storage/slotted_page.hpp:30) e
[page.hpp:22](include/modb/storage/page.hpp:22).

**Problema:** `page.hpp` exige apenas `page_size > 28`, mas
`slotted_page_max_record_size = page_size - 28 - 8` — para qualquer `MODB_PAGE_SIZE` entre
29 e 36, a subtração em `std::size_t` dá wrap e o limite vira ~1.8×10¹⁹, invalidando os
guardas de `record_too_large`. O CMake expõe `MODB_PAGE_SIZE` como cache variable
justamente para builds experimentais, então a configuração inválida é alcançável sem tocar
em código.

**Proposta:** endurecer o `static_assert` onde as constantes existem:
`static_assert(page_size >= slotted_page_header_size + slotted_page_slot_size + 1 && page_size <= 65535)`
em `slotted_page.hpp`, mantendo em `page.hpp` apenas o teto de 65535 dos offsets de 16 bits.

### S4. `decode_row` reserva memória por contagem não validada — **P0** — ✅ Feito

**Status:** implementado. `decode_row` agora rejeita `*count > max_columns_per_table` antes
do `reserve` (mesma guarda do `decode_schema`); por simetria, `encode_row` passou a usar o
mesmo teto (evita produzir um blob que codifica mas não decodifica). Regressões em
[codec_test.cpp](tests/codec_test.cpp): "Row declaring more values than the column limit"
(decode com cabeçalho forjado) e "encode_row rejects a Row with more values than the column
limit" (encode com 257 valores). Suíte completa passa, inclusive a bateria de churn.

**Onde:** [codec.cpp:284](src/storage/codec.cpp:284).

**Problema:** `decode_row` lê um `uint16_t` de contagem e chama `values.reserve(*count)`
antes de qualquer validação — um registro forjado com `count = 65535` força ~2–3 MB de
alocação por chamada mesmo que a decodificação falhe no byte seguinte. `decode_schema` já
valida contra `max_columns_per_table` antes do `reserve` ([codec.cpp:339](src/storage/codec.cpp:339));
`decode_row` não impõe o limite de 256 colunas, aceitando linhas estruturalmente impossíveis
para qualquer tabela válida.

**Proposta:** espelhar `decode_schema`: rejeitar `*count > max_columns_per_table` antes do
`reserve` — ou, no mínimo, limitar o reserve a `std::min<std::size_t>(*count, reader.remaining())`,
já que cada `Value` consome pelo menos 1 byte.

### S5. Injeção de sequências de escape de terminal via `TEXT` — **P0** — ✅ Feito

**Status:** implementado. A sanitização foi extraída para uma função de biblioteca
`modb::escape_for_terminal` ([text_escape.hpp](include/modb/text_escape.hpp) /
[text_escape.cpp](src/text_escape.cpp)) — escapa bytes de controle C0 (0x00–0x1f, incluindo
`\n`/`\t`), o DEL (0x7f) e a barra invertida como `\xHH`/`\\`, preservando bytes UTF-8
(≥ 0x80). O `print_value` da CLI passou a chamá-la. Cobertura: teste unitário dedicado
[text_escape_test.cpp](tests/text_escape_test.cpp) (ESC, OSC, `\n`/`\t`, DEL, NUL embutido,
UTF-8) registrado como `modb.text_escape`, mais validação ponta a ponta rodando a CLI real
com um byte ESC inserido via `record insert-values` e confirmando a saída escapada. Suíte
completa passa (22/22).

**Onde:** [main.cpp:1479](apps/modb_cli/main.cpp:1479) (`print_value`).

**Problema:** `print_value` imprime os bytes de `TEXT` crus no stdout, e o codec aceita
qualquer sequência de bytes em `TEXT` (sem validação UTF-8 nem filtragem de controle).
Um `.db` malicioso pode embutir sequências ANSI/OSC que, ao rodar `modb record read`/
`heap scan`, manipulam o terminal do usuário — de spoofing de saída a exploração de
vulnerabilidades conhecidas de emuladores de terminal.

**Proposta:** sanitizar em `print_value`: escapar bytes de controle (`< 0x20`, `0x7f`) fora
de `\t`/`\n` em forma visível (ex.: `\x1b`), e aplicar o mesmo tratamento a nomes de colunas
decodificados de schemas.

### S6. `flush()` não garante durabilidade — **P1** — ✅ Feito

**Status:** implementado via o refator de I/O que estava adiado. Nova classe
[NativeFile](include/modb/storage/native_file.hpp)/[native_file.cpp](src/storage/native_file.cpp)
encapsula um descritor nativo do SO com I/O posicional (`ReadFile`/`WriteFile` com `OVERLAPPED`
no Windows; `pread`/`pwrite` no POSIX) e, o ponto central, sincronização real ao dispositivo:
`FlushFileBuffers` no Windows e `fsync` no POSIX. Todo o código específico de plataforma fica
isolado nessa classe (`#ifdef _WIN32`). O `PageFile` trocou o `std::fstream` pelo `NativeFile`:
`read`/`write_at`/`write_page_count` viraram leituras/escritas posicionais (sem cursor
compartilhado, some o par `clear()`+`seek`), `create` grava o superbloco e sincroniza, e
`PageFile::flush()` agora chama `NativeFile::sync()` — depois de um flush bem-sucedido os dados
sobrevivem a uma queda de energia. Verificado: build limpo com `-Werror -Wconversion
-Wsign-conversion`, suíte 26/26 (incluindo os testes de reabertura), e smoke test end-to-end
pelo CLI — registro inserido num processo é lido de volta em processos separados após reabrir.
Durabilidade real contra queda de energia não é verificável em teste unitário, como esperado.

**Onde:** [page_file.cpp:372](src/storage/page_file.cpp:372).

**Problema:** `PageFile::flush` chama apenas `std::fstream::flush`, que move os bytes do
buffer da biblioteca para o cache do SO. Queda de energia após um flush "bem-sucedido" pode
perder ou rasgar escritas já confirmadas ao usuário pela CLI. Não há `FlushFileBuffers`/
`_commit` (Windows) ou `fsync` (POSIX) em nenhum ponto do código.

**Proposta:** após `stream_.flush()`, sincronizar com o dispositivo — no Windows,
`FlushFileBuffers` (exige manter um descritor nativo, o que conversa com a proposta P1 de
trocar o fstream por I/O posicional). Documentar que `flush()` passa a significar
durabilidade e chamá-lo nos pontos de commit. Observação: enquanto não houver fsync, a
escrita ansiosa da raiz do heap (item P1 abaixo) não compra durabilidade nenhuma — é puro
overhead, o que reforça as duas mudanças em conjunto.

### S7. `database_check`: custo quadrático e verificação incompleta — **P1** — ✅ Feito (parcial)

**Status:** feitas (b) e (c). **Detecção de cadeia compartilhada:** o L3 agora mantém um
conjunto global de páginas reivindicadas; quando duas raízes THRP reivindicam a mesma página,
reporta `corrupt_page` "data page N is shared by more than one TableHeap chain". **Nível L4:**
novo passo que lê cada slot ocupado e roda `decode_row`, reportando em `report.record_errors`
o endereço (`page`/`slot`) e o erro de cada registro indecodificável — payloads corrompidos
que passavam pela validação estrutural agora são detectados. `DatabaseCheckReport` ganhou
`record_errors` e o `ok()` passou a considerá-lo. Cobertura: teste L4 em
[database_check_test.cpp](tests/database_check_test.cpp) (corrompe a tag de um valor mantendo
a página estruturalmente válida → check reporta `invalid_encoding` com o endereço). Suíte
26/26. **Parte (a) não totalmente feita:** o custo O(R×P) por arquivo malicioso com muitas
raízes compartilhando uma cadeia longa não foi eliminado — a detecção é pós-`open`, então cada
raiz ainda traça sua cadeia; um corte real exigiria rastrear as páginas reivindicadas *durante*
a travessia (dentro do `TableHeap`). Fica como continuação.

**Onde:** [database_check.cpp:114-126](src/storage/database_check.cpp:114).

**Problema (a):** o nível L3 chama `TableHeap::open` para cada página THRP, e cada open
percorre a cadeia inteira. Um arquivo forjado com R raízes apontando para a mesma cadeia
longa de P páginas custa O(R × P) — DoS por entrada não confiável no próprio verificador.
**Problema (b):** duas raízes THRP apontando para a mesma cadeia passam limpas (cada
travessia é validada isoladamente), mas em uso real escrever por um heap corrompe o outro.
**Problema (c):** o verificador para na estrutura — não roda `decode_row` nos registros,
então payloads corrompidos só explodem depois, na leitura, apesar do check ter reportado
o arquivo como saudável.

**Proposta:** manter um conjunto global de páginas reivindicadas por raiz; ao encontrar
página já reivindicada, abortar com erro de "página compartilhada entre heaps" — isso torna
o custo O(páginas) e detecta a sobreposição de cadeias de uma vez. Adicionar um nível L4
opcional que percorra os slots ocupados e rode `decode_row`, reportando o endereço
(`page:slot`) dos que falharem, e reportar páginas SLPG órfãs.

### S8. Sem checksum por página — **P3** (mudança de formato)

**Onde:** formato de página em geral ([page_file.cpp](src/storage/page_file.cpp),
[FORMATO_DE_ARQUIVO.md](docs/FORMATO_DE_ARQUIVO.md)).

**Problema:** nenhuma página carrega CRC. As validações são puramente estruturais: um
bit-flip ou torn write dentro do payload de um registro que continue estruturalmente
plausível é aceito por `validate_page` e `decode_row`, devolvendo dados adulterados como
válidos.

**Proposta:** reservar 4 bytes no cabeçalho de cada página para CRC32C (ou xxHash32)
calculado sobre os demais bytes; gravar em `write_at`, verificar em `read`, retornar
`corrupt_page` na divergência. Exige subir a versão do formato — bom candidato para a
próxima quebra planejada de formato, com cobertura no `database_check`.

### S9. Sem lock de arquivo entre processos — **P3**

**Onde:** [page_file.cpp:274](src/storage/page_file.cpp:274).

**Problema:** o escopo do MVP declara "um processo", mas nada *impõe* isso: `std::fstream`
abre com share mode permissivo, e duas instâncias da CLI podem intercalar escritas no
superbloco/raiz/dados, produzindo exatamente as corrupções que o resto do código se esforça
para detectar.

**Proposta:** adquirir lock exclusivo na abertura (`LockFileEx` no Windows, `flock`/`fcntl`
em POSIX) e retornar um `ErrorCode` dedicado (ex.: `database_locked`). Transforma a
premissa de escopo em invariante verificada.

### S10. Operações multi-página sem atomicidade — **P3** (pós-MVP)

**Onde:** [table_heap.cpp:484-494](src/storage/table_heap.cpp:484) (update com realocação),
[table_heap.cpp:299-333](src/storage/table_heap.cpp:299) (insert com página nova).

**Problema:** `update` que não cabe na página faz insert da nova versão e depois erase da
antiga — crash entre os dois deixa o registro duplicado. No insert com página nova, um
crash entre as 3–5 escritas deixa os contadores da raiz divergentes da cadeia, e
`TableHeap::open`/`layout` então **recusa o heap inteiro** ("root counters or chain
endpoints are inconsistent") — uma falha parcial vira indisponibilidade total.

**Proposta:** *prevenção* de estados inconsistentes exige atomicidade multi-página —
write-ahead log ou shadow paging —, que é pós-MVP por decisão de escopo e de esforço alto.
O que torna o dano recuperável **dentro do MVP** é o modo de reparo do item S11, que trata a
consequência (contadores divergentes, registro duplicado) sem exigir a máquina de
atomicidade. Ou seja: S11 é o mitigador de curto prazo; S10 (WAL) é a solução definitiva.

### S11. `modb db repair`: reparo e recuperação após corrupção — **P1** — ✅ Feito (parcial)

**Status:** implementado o núcleo — reconstrução dos contadores da raiz. Nova função de
biblioteca `repair_table_heap(PageFile&, PageId root)` ([table_heap.hpp](include/modb/storage/table_heap.hpp))
percorre a cadeia de dados autodescritiva (com detecção de ciclo) sem confiar nos contadores
persistidos, recomputa `first`/`last`/`page_count`/`record_count` e reescreve a raiz THRP;
retorna `TableHeapRepairReport` e só reescreve se algo mudou (idempotente). Exposto na CLI
como `modb heap repair <file> <root>`. Cobertura: teste unitário em
[table_heap_test.cpp](tests/table_heap_test.cpp) (corrompe o `record_count` da raiz →
`open` falha com `corrupt_page` → reparo reconstrói → `open`/`scan` voltam a funcionar; reparo
de heap saudável não reescreve nada) + validação ponta a ponta na CLI. Suíte 26/26. **Isto
destrava o P1** (persistência adiada da raiz agora tem rede de segurança).

**Ainda pendente neste item (não bloqueia P1):** (a) reconciliação/truncamento da página
órfã de cauda no nível do `PageFile` — hoje o `open` a tolera (S1), falta a poda; (b) um
`modb db repair` que percorra **todas** as raízes THRP do arquivo (o atual repara uma raiz por
vez); (c) remoção de registro duplicado de `update` interrompido — deixada de fora por não
ser bem-definida num heap sem chave primária. Estas ficam como continuação.

**Onde:** consequências em [page_file.cpp:187](src/storage/page_file.cpp:187) (contagem vs.
tamanho físico), [table_heap.cpp:443-448](src/storage/table_heap.cpp:443) (contadores da
raiz recusam a abertura), [table_heap.cpp:484-494](src/storage/table_heap.cpp:484) (registro
duplicado por update). Complementa o verificador de [database_check.cpp](src/storage/database_check.cpp).

**Problema:** o motor é bom em **detectar** corrupção (`validate_superblock`, `validate_page`,
travessia da cadeia com detecção de ciclo, e o S7 amplia isso), mas não oferece nenhuma
forma de **recuperar** um banco que já ficou inconsistente. Hoje toda inconsistência
estrutural é terminal: o arquivo simplesmente se recusa a abrir e não há ferramenta para
consertá-lo. Os cenários já mapeados nesta auditoria que caem exatamente aqui:

- página órfã de cauda de um `allocate_page` interrompido — a validação de abertura foi
  relaxada em **S1**, mas a reconciliação plena (truncar/registrar a órfã) fecha o ciclo;
- contadores da raiz THRP (`page_count`/`record_count`/`first`/`last`) divergentes da cadeia
  após um insert/erase interrompido (**S10**), que hoje inutilizam o heap inteiro;
- registro duplicado deixado por um `update` com realocação interrompido (**S10**);
- páginas marcadas como corrompidas pelo nível L4 proposto em **S7**.

**Proposta:** um subcomando `modb db repair` (e uma API `TableHeap::repair`/
`PageFile::reconcile` por baixo) que, em vez de rejeitar, **conserta a partir da fonte de
verdade autodescritiva**:

1. **Superbloco:** reconciliar `page_count` com o tamanho físico do arquivo (fonte de
   verdade), truncando ou registrando páginas de cauda órfãs — a outra metade do S1.
2. **Raiz do heap:** percorrer a cadeia de páginas de dados (que já é autodescritiva via
   `next`/`previous`) e **reconstruir** `first`/`last`/`page_count`/`record_count`, em vez de
   comparar e recusar. Como `layout()` já faz a travessia, o reparo reaproveita quase toda a
   lógica — muda apenas a ação em caso de divergência (corrigir, não falhar).
3. **Duplicatas:** ao detectar o registro fantasma de um `update` interrompido, remover a
   cópia órfã segundo uma política determinística (ex.: manter o menor `RecordId`).
4. **Relatório:** listar o que foi corrigido e o que foi descartado, sem apagar nada
   silenciosamente; oferecer um modo `--dry-run` que só reporta.

**Dependência inversa importante:** este item **destrava o P1** (persistência adiada da
raiz). Hoje a raiz é reescrita a cada operação justamente porque um crash com contadores
divergentes é fatal; com o reparo capaz de reconciliar, os contadores podem ser mantidos em
memória e persistidos com preguiça sem risco de inutilizar o banco. Por isso está em P1, e
não em P3: é barato (reusa `layout()`), tem valor imediato (bancos deixam de morrer por
falhas parciais) e desbloqueia um ganho de performance.

---

## 2. Performance

O padrão dominante: **cada operação paga custos de abertura/validação/persistência que
deveriam ser pagos uma vez por sessão ou por página**. Os itens P1–P6 se reforçam
mutuamente e, juntos, mudam a complexidade das operações básicas.

### P1. Amplificação de escrita: raiz + superbloco reescritos por operação — **P1** — 🟡 Parcial

**Status:** feita a parte do superbloco. `allocate_page` deixou de reescrever a página zero
inteira (4096 bytes via `make_superblock`) a cada alocação e agora atualiza apenas os 8 bytes
de `page_count` com uma escrita direcionada (`PageFile::write_page_count`), preservando
magic/versão/tamanho-de-página/`catalog_root` sem tocá-los. Suíte 26/26, churn com as mesmas
88 páginas. **Não feita (decisão de design pendente):** adiar o `persist_root` por
insert/erase. Isso está entrelaçado com a semântica do `open()` — adiar exige que o `open()`
*reconcilie* contadores divergentes (via S11) em vez de recusar, enfraquecendo uma checagem de
corrupção, e a CLI (um comando por processo) precisaria de um `flush` explícito do heap ou
todo banco ficaria permanentemente "sujo". É uma mudança do modelo de durabilidade que não
deve ser feita sem decisão explícita; ver a discussão ao final.

**Onde:** [table_heap.cpp:253](src/storage/table_heap.cpp:253) (e 224, 330, 534, 602);
[page_file.cpp:306-313](src/storage/page_file.cpp:306).

**Problema:** cada insert bem-sucedido escreve a página de dados **e** reescreve a página
raiz THRP inteira via `persist_root` (idem erase). Quando há alocação de página nova,
somam-se a escrita da página zerada e a reescrita do superbloco. Inserir um registro de
30 bytes custa de 2 a 5 escritas de 4 KB com seeks alternados entre o início (raiz,
superbloco) e o fim (dados) do arquivo. Como não há fsync (S6), essa escrita ansiosa nem
compra durabilidade — é overhead puro.

**Proposta:** manter os contadores da raiz em memória com flag dirty e persistir em
`flush()`/fechamento e em fronteiras (alocação de página nova). **Atenção à dependência:**
`layout()` hoje *recusa* abrir um heap cujos contadores divergem da cadeia
([table_heap.cpp:443-448](src/storage/table_heap.cpp:443)) — a persistência adiada exige
antes o modo de reparo/reconciliação do item **S11**, ou relaxar essa validação. Alternativa
incremental: API `insert_many` que persiste a raiz uma vez ao final do lote.

### P2. `TableHeap::open` valida a cadeia inteira; a CLI reabre tudo a cada comando — **P1** — ⏸️ Adiado (decisão de design)

**Status:** não implementado por ser estruturalmente entrelaçado. `layout()` (chamado por
`open`) não faz só validação: constrói `page_ids_` e `insertion_capacity_by_page_`, dos quais
`insert`/`read`/`erase` dependem. Um `open(verify=false)` **não** economiza a travessia porque
esses índices precisam dela. Fazer o `open` barato de verdade exige (a) construção preguiçosa
dos índices sob demanda, ou (b) um free-space map persistido na raiz — ambos mudam a
arquitetura de indexação. Fica para uma iteração dedicada, com decisão explícita. Nota: parte
do custo por comando da CLI já caiu com P3 (scan não retravessa) e P4 (leituras sem
revalidação).

**Onde:** [table_heap.cpp:165](src/storage/table_heap.cpp:165); comandos em
[main.cpp:725](apps/modb_cli/main.cpp:725) (insert), 757 (scan), 794 (layout), 832 (update),
862 (erase).

**Problema:** todo `TableHeap::open` executa `layout()` completo — lê e valida TODAS as
páginas da cadeia, com detecção de ciclo. Inserir 1 registro em um heap de 1000 páginas
custa ~1000 leituras+validações só na abertura. Cada comando da CLI é um processo novo que
paga isso de novo; o custo de qualquer comando cresce linearmente com o tamanho do banco.

**Proposta:** confiar nos metadados da raiz THRP em `open()` (validando apenas a raiz) e
deixar a travessia completa para o `db check` e para um `open(verify=true)`. Ressalva
estrutural: `layout()` também constrói os índices `page_ids_` e
`insertion_capacity_by_page_`, dos quais insert/read/erase dependem — a mudança exige
construção preguiçosa desses índices (ou um free-space map persistido), não é só remover a
chamada.

### P3. Scan lê cada página O(registros) vezes — **P1**, esforço baixo — ✅ Feito

**Status:** implementado. Novo `TableHeap::scan_records()` percorre a cadeia uma única vez,
carrega cada página uma vez e lê os bytes de cada slot ocupado da cópia em memória (sem novo
acesso ao arquivo por registro). O `scan()` também foi reescrito para passada única (antes
chamava `layout()` e depois recarregava cada página). A CLI `heap scan` passou a usar
`scan_records()`, eliminando o `read(id)` por registro. Regressão em
[table_heap_test.cpp](tests/table_heap_test.cpp) ("scan_records matches scan address order",
"scan_records returns the persisted Row bytes"). Suíte completa 26/26.

**Onde:** [main.cpp:770](apps/modb_cli/main.cpp:770); [table_heap.cpp:360-384](src/storage/table_heap.cpp:360).

**Problema:** a CLI faz `heap->scan()` e depois `heap->read(id)` por registro; cada `read`
relê e revalida a página inteira do disco. Uma página com ~50 registros é lida ~50 vezes.
Pior: o próprio `scan()` chama `layout()` (que já carregou cada página) e depois recarrega
cada página de novo — somando o `open`, a cadeia inteira é lida 3 vezes antes do loop
por registro.

**Proposta:** uma API de scan que entregue os registros junto com os endereços (por página:
carregar uma vez, extrair todos os slots ocupados, avançar) — por exemplo
`Result<std::vector<std::pair<RecordId, std::vector<std::byte>>>>` ou um iterador por
página. Fazer `layout()` coletar os `SlotInfo` na mesma passada. A CLI troca o par
`scan()+read()` por essa API.

### P4. Toda operação copia a página 2× e reexecuta `validate_page` — **P1** — ✅ Feito (parcial)

**Status:** implementada a parte (a) — o caminho confiável. Novo
`SlottedPage::from_trusted_page` (sem `validate_page`) e `TableHeap::load_trusted`, usados por
`insert`/`read`/`update`/`erase`/`scan`/`scan_records` (páginas já validadas ao abrir e
mantidas pelas próprias operações). O `load()` validante ficou só no `layout()` (a passada de
validação de `open`/`db check`). Isso elimina o `std::sort` + alocação de `std::vector` +
varredura completa do diretório **por leitura** nos caminhos quentes. Suíte 26/26, incluindo
churn. **Parte (b) não feita:** fazer `SlottedPage` operar sobre um `std::span` emprestado do
scratch (eliminar a cópia de 4 KB) — é refatoração maior e fica como continuação; a cópia
por leitura permanece, mas o custo dominante de CPU (sort/alocação) foi removido.

**Onde:** [table_heap.cpp:179-189](src/storage/table_heap.cpp:179) (`load`);
[slotted_page.cpp:268](src/storage/slotted_page.cpp:268) (`from_page`);
[slotted_page.cpp:134-241](src/storage/slotted_page.cpp:134) (`validate_page`).

**Problema:** `load` lê no buffer do scratch pool, mas `SlottedPage::from_page` recebe
`Page` **por valor** — e "mover" um `std::array<std::byte, 4096>` copia elemento a
elemento, então são duas cópias de 4 KB por load, anulando o benefício do pool. Além disso,
`from_page` roda `validate_page` integralmente em toda leitura: percorre o diretório,
aloca um `std::vector<std::pair>` e faz `std::sort` — mesmo para páginas que o próprio heap
escreveu e validou segundos antes.

**Proposta:** (a) um caminho confiável (`from_trusted_page` ou construtor privado usado
pelo `TableHeap`) que pula `validate_page` para páginas já validadas em open/layout,
reservando a validação completa para open e `db check`; (b) idealmente, fazer `SlottedPage`
operar sobre um `std::span<std::byte, page_size>` emprestado do scratch buffer (view
não proprietária), eliminando as cópias.

### P5. `TableHeap::update` lê e valida a mesma página duas vezes — **P1**, esforço baixo — ✅ Feito

**Status:** implementado. `update` deixou de chamar `read(id)` (que fazia leitura física +
`validate_page` + cópia descartada dos bytes antigos) e agora carrega a página **uma única
vez**, validando pertencimento (`page_ids_.contains`) e geração na instância carregada — o
mesmo padrão do `erase()`. Passou a metade das leituras/validações e eliminou a cópia
descartada por `update`. As guardas reimplementadas ganharam regressão em
[table_heap_test.cpp](tests/table_heap_test.cpp) ("update rejects a stale generation",
"update rejects a page outside the heap", "rejected updates leave the record intact"). Suíte
completa 26/26, incluindo churn.

**Onde:** [table_heap.cpp:463-467](src/storage/table_heap.cpp:463).

**Problema:** `update` chama `read(id)` só para validar existência/geração — o que já faz
leitura física + `validate_page` + copia os bytes antigos para um vector imediatamente
descartado — e na linha seguinte chama `load(id.page)` de novo, repetindo tudo.

**Proposta:** carregar uma vez e validar a geração na instância carregada, como `erase()`
já faz ([table_heap.cpp:499-516](src/storage/table_heap.cpp:499)) — preservando a checagem
`page_ids_.contains` que hoje vem embutida no `read`.

### P6. Busca de página com espaço: O(P) por insert, ordem não determinística, mutação durante iteração — **P1** — ✅ Feito (parcial)

**Status:** corrigidos os dois problemas de correção. (b) determinismo: o índice
`insertion_capacity_by_page_` passou de `std::unordered_map` para `std::map` — a iteração
agora é por PageId crescente (idade de alocação), tornando **verdadeiro** o comentário
"prioriza páginas antigas" e reprodutível entre plataformas. (c) UB latente: o loop de insert
agora coleta as candidatas num `std::vector` **antes** de chamar `try_insert`, eliminando a
mutação do mapa durante a iteração. Os testes `space_reuse` confirmam que a política de reuso
foi preservada. **Parte (a) não feita:** a varredura ainda é O(P) por insert (só filtra sem
I/O); um índice por capacidade (`std::map<capacidade, set<PageId>>` com `lower_bound`, best-fit)
mudaria a política de seleção e arriscaria os testes de reuso, então fica como continuação.

**Onde:** [table_heap.cpp:260](src/storage/table_heap.cpp:260).

**Problema (a):** o loop itera sobre TODAS as páginas do heap a cada insert — O(N·P) para N
inserções em heap com P páginas majoritariamente cheias. **Problema (b):** o comentário diz
"prioriza páginas antigas", mas `std::unordered_map` não tem ordem definida — o layout
físico não é reprodutível entre plataformas (MinGW vs MSVC). **Problema (c):** o lambda
`try_insert` escreve em `insertion_capacity_by_page_` *durante* a iteração sobre o mesmo
mapa (linhas 239/251) — hoje seguro por acidente (a chave sempre existe, sem rehash), mas
qualquer refatoração que insira chave nova nesse caminho vira comportamento indefinido.

**Proposta:** substituir por estrutura indexada por capacidade (ex.:
`std::map<std::size_t, std::set<PageId>>` consultado com `lower_bound(record.size())`, ou
buckets de fill-factor estilo FSM do PostgreSQL): consulta O(log P), ordem determinística
por PageId, e a coleta de candidatas antes do loop elimina a mutação durante iteração.

### P7. `erase` compacta sempre; `compact` copia a página inteira — **P2**

**Onde:** [slotted_page.cpp:425](src/storage/slotted_page.cpp:425) e
[slotted_page.cpp:432](src/storage/slotted_page.cpp:432). `update` com mudança de tamanho
também compacta ([slotted_page.cpp:400](src/storage/slotted_page.cpp:400)).

**Problema:** toda remoção paga `const Page source = page_;` (cópia integral de 4 KB) +
varredura completa do diretório, mesmo quando bastaria avançar `free_end`.

**Proposta:** compactação preguiçosa — marcar o slot livre, acumular "bytes fragmentados"
no cabeçalho e compactar só quando uma inserção falhar por fragmentação (padrão
SQLite/PostgreSQL). No `compact`, eliminar a cópia processando os slots em ordem
decrescente de offset físico com `std::memmove`/`copy_backward` (exige ordenar por offset
antes, pois a ordem de SlotId não corresponde à física).

### P8. `BinaryWriter` sem `reserve`, um `push_back` por byte — **P2**, esforço baixo

**Onde:** [binary.cpp:31-61](src/storage/binary.cpp:31); [codec.cpp:253-270](src/storage/codec.cpp:253).

**Problema:** `write_u16/u32/u64` chamam `write_u8` em loop e `encode_row` constrói a linha
sem nenhum `reserve` (o caminho inverso, `decode_row`, já reserva). A classe nem expõe
`reserve` para os chamadores.

**Proposta:** adicionar `BinaryWriter::reserve(std::size_t)` e usá-lo em
`encode_row`/`encode_schema` com estimativa; nos `write_uNN`, escrever em bloco
(`resize`+`memcpy` ou `insert` de um array local convertido, com `std::byteswap`
condicional quando `std::endian::native != little`).

### P9. Varreduras O(slots) redundantes na slotted page — **P2**, esforço baixo

**Onde:** [slotted_page.cpp:460-469](src/storage/slotted_page.cpp:460) (`record_count`),
[487-497](src/storage/slotted_page.cpp:487) (`insertion_capacity`),
[294-301](src/storage/slotted_page.cpp:294) (busca de slot livre no `insert`).

**Problema:** três varreduras independentes do diretório inteiro por operação; em
`TableHeap::layout`, `record_count()` é chamado duas vezes por página.

**Proposta:** persistir um contador de slots ocupados no cabeçalho (há folga de layout)
mantido por insert/erase → `record_count` O(1); no `insert`, uma única varredura acha o
slot livre e memoriza o que `insertion_capacity` precisaria; em `layout`, chamar
`record_count` uma vez e reutilizar.

### P10. `ScratchPagePool`: mutex + condition_variable por load em motor single-thread — **P2** — ✅ Feito

**Status:** resolvido junto com o C6. O `std::mutex` e o `std::condition_variable` foram
removidos do `ScratchPagePool` (o motor é single-thread por escopo), eliminando lock/wait/notify
por `load`. Detalhes na seção C6.

**Onde:** [scratch_page_pool.cpp](src/storage/scratch_page_pool.cpp);
capacidade padrão 1 em [table_heap.hpp:53](include/modb/storage/table_heap.hpp:53).

**Problema:** cada `TableHeap::load` paga lock + wait em CV e lock + notify no release,
num motor single-thread por escopo. Como `from_page` copia a página para fora (P4), o pool
hoje só reutiliza o buffer de leitura — benefício que um único `Page` membro daria de
graça. E com capacidade 1, dois handles aninhados no futuro seriam deadlock silencioso
(`acquire()` bloqueante sem timeout — ver também C6).

**Proposta:** enquanto o motor for single-thread, substituir por um único buffer `Page`
membro do `TableHeap` (ou free-list sem locks). Se a concorrência vier, medir antes e fazer
o `SlottedPage` trabalhar in-place (P4b) para o pooling ter efeito real.

Dois itens menores confirmados, para quando estiver mexendo nos arquivos: o overload
`PageFile::read(PageId)` por valor custa cópias extras de 4 KB nos chamadores da CLI e em
`TableHeap::open` (usar o overload `read(PageId, Page&)` já existente); e
`Catalog::table`/`mutable_table` fazem `find(std::string{name})` — habilitar lookup
heterogêneo (`hash` transparente + `std::equal_to<>`) elimina a string temporária por
consulta ([catalog.cpp:65, 92](src/catalog/catalog.cpp:65)).

---

## 3. Uso do C++

O código já é bastante idiomático — `std::expected` consistente, `std::byte`, tipos fortes
(`PageId`, `SlotId`), spans, `[[nodiscard]]` nas APIs de storage. As propostas abaixo
fecham as lacunas restantes.

### C1. Codec little-endian implementado três vezes — **P2** — ✅ Feito

**Status:** implementado. Novo header [endian.hpp](include/modb/storage/endian.hpp) com
`store_le<T>`/`load_le<T>` `constexpr`, restritos a `std::unsigned_integral` e operando sobre
`std::span<std::byte>`. Os três sítios passaram a delegar: `BinaryWriter/Reader`
([binary.cpp](src/storage/binary.cpp)), `encode_/decode_uNN` ([page_file.cpp](src/storage/page_file.cpp))
e `write_/read_u16/u64` ([slotted_page.cpp](src/storage/slotted_page.cpp)) — as ~180 linhas
duplicadas viraram wrappers de uma linha, mantendo assinaturas e semântica. Verificado: build
limpo com `-Werror -Wconversion -Wsign-conversion` e suíte 26/26 (round-trips de codec cobrem
os dois caminhos).

**Onde:** [binary.cpp](src/storage/binary.cpp) (`write_/read_uNN`),
[page_file.cpp:48-126](src/storage/page_file.cpp:48) (`encode_/decode_uNN`),
[slotted_page.cpp:51-89](src/storage/slotted_page.cpp:51) (`write_/read_u16/u64`).

**Problema:** ~180 linhas de lógica byte a byte duplicada, cada cópia com sua combinação de
casts; qualquer mudança precisa ser repetida em três lugares.

**Proposta:** um header interno (ex.: `include/modb/storage/endian.hpp`) com
`store_le<T>(span, offset, value)` / `load_le<T>(span, offset)` `constexpr`, restritas a
`std::unsigned_integral`; os três arquivos delegam. Em C++23, `std::byteswap` +
`std::bit_cast` quando `std::endian::native == little` dá a versão otimizada de graça.

### C2. Cadeias de `get_if` onde `std::visit` exaustivo é verificado em compilação — **P2** — ✅ Feito

**Status:** implementado. `Overloaded` movido para
[include/modb/detail/overloaded.hpp](include/modb/detail/overloaded.hpp). `encode_value_to`
([codec.cpp](src/storage/codec.cpp)) e `Value::data_type` ([value.cpp](src/model/value.cpp))
reescritos com `std::visit` — uma alternativa nova em `Value::Storage` sem caso vira erro de
compilação, eliminando o `std::get<std::string>` que arriscava `bad_variant_access`. Suíte
26/26 (round-trips do codec cobrem os dois caminhos).

**Onde:** [codec.cpp:102-121](src/storage/codec.cpp:102) (`encode_value_to`);
[value.cpp](src/model/value.cpp) (`data_type` com `holds_alternative`).

**Problema:** o `std::get<std::string>` final ("única alternativa restante") compila
silenciosamente se `Value::Storage` ganhar uma alternativa nova — e lança
`bad_variant_access` em runtime. A ironia: [main.cpp:39-45](apps/modb_cli/main.cpp:39) já
define o helper `Overloaded` e o usa em `print_value`, mas ele vive no CLI.

**Proposta:** mover `Overloaded` para um header da biblioteca (ex.:
`include/modb/detail/overloaded.hpp`) e reescrever `encode_value_to` e `Value::data_type`
com `std::visit` — alternativa nova no variant vira erro de compilação nos dois pontos.

### C3. `Value{ptr}` vira BOOLEAN silenciosamente; faltam accessors tipados — **P2** — ✅ Feito (parcial)

**Status:** feita a guarda de segurança. Adicionado
`template <typename Pointer> requires(!std::same_as<Pointer, char>) Value(Pointer*) = delete;`
em [value.hpp](include/modb/value.hpp) — `Value{&x}` para qualquer ponteiro (exceto
`char*`/`const char*`, que continuam indo ao construtor de texto) agora é erro de compilação
em vez de virar BOOLEAN. Garantido por `static_assert` em [model_test.cpp](tests/model_test.cpp).
**Não feito:** os accessors tipados (`as_boolean()`/`as_integer()`/…) — reduziriam o
acoplamento dos call sites ao variant, mas mexeriam em vários chamadores (CLI, codec); ficam
como continuação de menor prioridade.

**Onde:** [value.hpp:38](include/modb/value.hpp:38).

**Problema:** `Value(bool)` não é `explicit` e não há guarda contra ponteiros: `Value{&x}`
compila e produz um BOOLEAN via conversão ponteiro→bool. Além disso, sem accessors tipados,
os consumidores fazem `std::get<std::int64_t>(value.storage())`, expondo o variant interno
em todos os call sites.

**Proposta:** `template <typename T> Value(T*) = delete;` (o overload exato de
`const char*` continua vencendo para literais). Complementar com
`as_boolean()/as_integer()/as_real()/as_text()` retornando `std::optional`/`const T*` via
`get_if`, alinhados à política de erros.

### C4. `Page::operator[]` lança exceção, contrariando a política de `std::expected` — **P2** — ✅ Feito

**Status:** implementado em `Page::operator[]` ([page.hpp](include/modb/storage/page.hpp)):
`data_.at()` (bounds check + throw por byte no caminho quente) virou `assert(index < page_size)`
+ indexação direta, precondição no estilo `std::span` — os offsets vêm de campos já validados
por `validate_page`. Suíte 26/26. **Nota:** `Row::operator[]` ([row.hpp:34](include/modb/row.hpp:34))
foi deixado com `.at()` de propósito: é um acessor raramente chamado (não caminho quente) e a
checagem defensiva ali é aceitável; mudá-lo é baixo valor.

**Onde:** [page.hpp:49-51](include/modb/storage/page.hpp:49); mesmo padrão em
`Row::operator[]` ([row.hpp:34](include/modb/row.hpp:34)).

**Problema:** todo o projeto retorna `Result<T>` e nenhum caminho trata exceções, mas
`operator[]` delega a `data_.at()` — bounds check + throw por acesso a byte em código
quente, e um bug de offset vira `terminate` em vez de `Error{corrupt_page}`.

**Proposta:** indexação direta precedida de `assert(index < page_size)` (precondição
documentada, padrão de `std::span`), mantendo a validação estrutural onde já existe
(`validate_page`).

### C5. `PageFile` sem `close()`; destrutor engole erros de flush — **P2**

**Onde:** [page_file.hpp:30-34](include/modb/storage/page_file.hpp:30).

**Problema:** `~PageFile() = default` fecha o fstream engolindo qualquer falha do flush
final (disco cheio, mídia removida) — num banco, a última escrita perdida é exatamente a
que importa. Nota adjacente: o move está `noexcept = default`, mas o move de
`std::basic_fstream` não é `noexcept` — uma exceção ali vira `std::terminate`.

**Proposta:** `[[nodiscard]] Result<void> close()` (flush + close com erro reportado),
destrutor documentado como best-effort, e a CLI chamando `close()` explicitamente ao fim
dos comandos de escrita.

### C6. `ScratchPagePool` lança exceção e `acquire()` bloqueia sem timeout — **P2** — ✅ Feito

**Status:** implementado. O construtor virou privado e a validação de capacidade passou para a
fábrica `static Result<std::unique_ptr<ScratchPagePool>> create(std::size_t)` — capacidade zero
agora retorna `ErrorCode::invalid_argument` em vez de lançar `std::invalid_argument`, e as
checagens duplicadas em `TableHeap::create/open` foram removidas (a fábrica é o ponto único).
O `acquire()` bloqueante foi eliminado: sobra `try_acquire()`, e `TableHeap::load/load_trusted`
tratam a ausência de buffer como erro explícito. Como o motor é single-thread por escopo, o
`std::mutex` + `std::condition_variable` (peso morto e origem do deadlock sem diagnóstico) foram
removidos — resolvendo junto o P10. Teste em [storage_test.cpp](tests/storage_test.cpp) atualizado
para a fábrica/`try_acquire`. Verificado: suíte 26/26.

**Onde:** [scratch_page_pool.cpp](src/storage/scratch_page_pool.cpp) (construtor);
validação duplicada nos chamadores em [table_heap.cpp:122 e 144](src/storage/table_heap.cpp:122).

**Problema:** é o único ponto do motor que usa exceção em vez de `Result` —
`TableHeap::create/open` pré-validam `scratch_page_count == 0` exatamente para a exceção
não escapar (validação duplicada). E `acquire()` bloqueante sem timeout + capacidade 1 =
deadlock sem diagnóstico para qualquer uso futuro com dois handles.

**Proposta:** factory `[[nodiscard]] static Result<ScratchPagePool> create(std::size_t)`
(elimina a duplicação) e `try_acquire()` com fallback/erro em vez de `acquire()`
bloqueante. Ou simplesmente o buffer único do item P10.

### C7. `main.cpp` monolítico: 1514 linhas — **P2**, esforço alto

**Onde:** [main.cpp](apps/modb_cli/main.cpp) (forward declarations nas linhas 48–113).

**Problema:** 8 `print_*_help`, ~20 `command_*`, 5 `run_*_command`, parsers e utilitários
num único arquivo, com ~65 linhas de forward declarations mantidas em sincronia manual e o
mesmo padrão de dispatch `argc/argv/--help` repetido quatro vezes.

**Proposta:** dividir em `cli_util.{hpp,cpp}` + `commands_{db,page,record,heap,demo}.cpp`,
cada um expondo `run(argc, argv)`, e um `main.cpp` fino com tabela
`std::array<std::pair<std::string_view, handler>>` para o dispatch.

---

## 4. Build, testes e tooling

### B1. Sanitizers dão `FATAL_ERROR` justamente no Windows — **P0**, esforço baixo — ✅ Feito

**Status:** implementado. O bloco `MODB_ENABLE_SANITIZERS` em [CMakeLists.txt](CMakeLists.txt)
agora ramifica por toolchain e nunca aborta a configuração: MSVC → `/fsanitize=address /Zi`
(+ `/INCREMENTAL:NO`); Clang no Windows e Clang/GCC fora do Windows → `-fsanitize=address,undefined`;
MinGW GCC → hardening viável (`_GLIBCXX_ASSERTIONS` + `-fstack-protector-strong`) com aviso
explicando que ASan/UBSan não existem nesse toolchain; qualquer outro → aviso e segue. Verificado:
o preset `sanitizers` configura, compila e roda a suíte (26/26) no MinGW, onde antes dava
`FATAL_ERROR`.

**Onde:** [CMakeLists.txt:74-81](CMakeLists.txt:74); preset `sanitizers` em
[CMakePresets.json](CMakePresets.json).

**Problema:** o bloco exige `Clang|GNU AND NOT WIN32`, então o preset `sanitizers` é
inatingível em 100% dos ambientes declarados do projeto (MinGW GCC 13 e MSVC no Windows) —
e o código de paginação é exatamente o tipo de código que mais se beneficia de ASan/UBSan.

**Proposta:** MSVC: `/fsanitize=address` (+ `/Zi`). Clang no Windows (clang-cl/clang-mingw):
`-fsanitize=address`. Nota: o **GCC do MinGW-w64 não suporta ASan** — para esse toolchain a
rede de segurança viável é `-D_GLIBCXX_ASSERTIONS` (bounds checks da libstdc++) +
`-fstack-protector-strong` em Debug, que funcionam hoje. Trocar o `FATAL_ERROR` por
mensagem informando qual sanitizer ficou ativo.

### B2. 12 blocos de alvo de teste copiados e colados — **P2**, esforço baixo

**Onde:** [CMakeLists.txt:91-180](CMakeLists.txt:91); bloco MINGW nas linhas 136–149
(13 repetições contando o `modb_cli` na linha 88).

**Proposta:** `function(modb_add_test)` com `cmake_parse_arguments`
(NAME/SOURCE/DEFINITIONS/LABELS/TIMEOUT) fazendo `add_executable` + link + `add_test`;
link estático MinGW centralizado num alvo INTERFACE
(`target_link_options(... $<$<BOOL:${MINGW}>:-static;...>)`). Reduz o bloco de ~90 para
~30–35 linhas e elimina o risco de esquecer a linha MINGW num alvo novo.

### B3. Fuzzing ausente para parsing de entrada não confiável — **P3**

**Onde:** decoders em [codec.cpp](src/storage/codec.cpp), superbloco em
[page_file.cpp](src/storage/page_file.cpp), `validate_page` em
[slotted_page.cpp](src/storage/slotted_page.cpp).

**Contexto:** o parsing é sistematicamente defensivo (o `BinaryReader` valida limites antes
de todo acesso; não encontramos OOB óbvio) — mas fuzzing é exatamente o que dá confiança
nessa afirmação, e o próprio [PLANO_DE_DESENVOLVIMENTO.md](docs/PLANO_DE_DESENVOLVIMENTO.md)
lista fuzzing como item pendente. Os decoders operam sobre `std::span<const std::byte>`,
alvos triviais para libFuzzer.

**Proposta:** alvos `LLVMFuzzerTestOneInput` para `decode_value`/`decode_row`/
`decode_schema` e para o `database_check` sobre buffer tratado como arquivo, compilados com
`-fsanitize=fuzzer,address` num preset `fuzz` (clang; no Windows via clang-cl). Corpus
mínimo em `tests/fuzz/corpus`.

### B4. Framework de teste artesanal — **P3**

**Onde:** [test_support.hpp](tests/test_support.hpp); `TemporaryDatabase` duplicado em 6
arquivos de teste.

**Problema:** `check(bool, mensagem)` não reporta esperado/obtido; um segfault aborta todas
as verificações seguintes do binário; o ctest só enxerga binários inteiros (sem filtro por
caso); não há fixtures.

**Proposta:** doctest (header-only, coerente com o espírito educacional) ou Catch2 v3 via
FetchContent, com `*_discover_tests` para registrar cada `TEST_CASE` no ctest; migrar
`TemporaryDatabase` para um helper compartilhado único.

### B5. Preset release não roda testes; sem LTO nem hardening — **P3**, esforço baixo — ✅ Feito (parcial)

**Status:** feitas as flags de hardening na biblioteca `modb` (onde vive o parsing de entrada
não confiável): GCC/MinGW `-fstack-protector-strong` + `_GLIBCXX_ASSERTIONS` em Debug; MSVC
`/sdl` (o `/GS` já é padrão). Build Debug e suíte 26/26. **Não feito:** LTO em Release
(`CheckIPOSupported`) e rodar testes na configuração Release (mudar `BUILD_TESTING` do preset
release + testPreset) — mudanças de preset que interagem com CI; ficam como continuação.


**Onde:** [CMakePresets.json:34](CMakePresets.json) (`BUILD_TESTING=OFF` no release).

**Proposta:** manter `BUILD_TESTING=ON` no release com testPreset correspondente (excluindo
o label `performance` via filter se necessário); habilitar LTO
(`CheckIPOSupported` + `CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE`); hardening: MinGW
`-fstack-protector-strong`, MSVC `/sdl /guard:cf` — relevante porque o `.db` é entrada não
confiável.

### B6. `.clang-tidy` desabilita o `clang-analyzer-*` e não é executado por ninguém — **P3**, esforço baixo — ✅ Feito (parcial)

**Status:** feita a config — [.clang-tidy](.clang-tidy) agora habilita `clang-analyzer-*`
(a análise de fluxo que o `-*` desligava) e define `WarningsAsErrors: 'clang-analyzer-*'`.
**Não feito:** integrar o clang-tidy ao build/CI via `CMAKE_CXX_CLANG_TIDY` (depende de ter
clang-tidy no toolchain e de um pipeline) e habilitar famílias mais amplas
(`cppcoreguidelines-*`/`misc-*`) com supressões — precisa rodar a ferramenta para calibrar o
ruído, o que este ambiente (MinGW GCC) não permite verificar. Continuação.


**Onde:** [.clang-tidy:1](.clang-tidy).

**Problema:** a lista `-*` desliga o `clang-analyzer-*` (null-deref, leaks, dead-stores —
habilitado por padrão) e omite `cppcoreguidelines-*`/`misc-*`/`readability-*`. Além disso,
nada no build executa o clang-tidy (`CMAKE_CXX_CLANG_TIDY` não é configurado) — a aderência
depende de invocação manual/IDE.

**Proposta:** acrescentar `clang-analyzer-*` (no mínimo), `cppcoreguidelines-*` e `misc-*`
com supressões pontuais; expor `option(MODB_ENABLE_CLANG_TIDY)` que define
`CMAKE_CXX_CLANG_TIDY`; definir `WarningsAsErrors: bugprone-*,clang-analyzer-*` quando
houver CI.

### B7. CI, cobertura e install/export ausentes — **P3**

**Contexto:** o repositório ainda é local (sem remote, sem commits), então CI só terá
efeito quando publicado — até lá, um script de verificação local/pre-commit é o equivalente
prático.

**Propostas:** (a) ao publicar, workflow com matriz Windows MSVC + MinGW + um job Linux
rodando o preset `sanitizers` (ASan/UBSan funcionam lá hoje sem esforço) e
`-DMODB_WARNINGS_AS_ERRORS=ON`; (b) `option(MODB_ENABLE_COVERAGE)` com `--coverage` +
gcovr (funciona no Windows/MinGW) para descobrir quais ramos de corrupção do codec/
slotted_page estão de fato exercitados; (c) o trio `install(TARGETS/DIRECTORY/EXPORT)` +
config-file via `CMakePackageConfigHelpers` — hoje o `$<INSTALL_INTERFACE:include>` é
código morto e `find_package(modb)` é impossível; guardar os testes com
`if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING)` para consumo limpo via FetchContent.

### B8. `.gitignore` não cobre `*.db` — **P3**, esforço mínimo — ✅ Feito

**Status:** implementado. Adicionados `*.db` e `*.db-wal` ao [.gitignore](.gitignore);
`git check-ignore` confirma que o `loja.db` da raiz agora casa com a regra `*.db`. O arquivo
já estava untracked, então não foi preciso removê-lo do índice (nem o apaguei — é artefato
de uso manual do próprio dono). Fica a sugestão opcional (não aplicada) de `WORKING_DIRECTORY
${CMAKE_CURRENT_BINARY_DIR}` nos testes de CLI como defesa extra; hoje nenhum teste cria
arquivo no source tree.

### Nota sobre C++26

O projeto se declara C++26 mas não usa nenhum recurso exclusivo de C++26 (`std::expected` é
C++23; `std::span` é C++20) — GCC 13 cai sempre no ramo de WARNING do fallback. Como a meta
C++26 é intencional e documentada no README, a recomendação é apenas formalizá-la como ADR
em `docs/decisions/` (hoje a justificativa vive só no README), e não rebaixar a baseline.

---

## Roadmap sugerido

1. **Semana de correções P0** (tudo esforço baixo): S1 + S2 juntos (mesma rotina do
   superbloco), S3, S4, S5, B1, B8. Nenhum muda formato de arquivo; S1 muda apenas a
   *validação* de abertura.
2. **Robustez + performance estrutural (P1)**: começar pelos de esforço baixo com ganho
   imediato — P5 (update) e P3 (scan por página). Fazer **S11 (`db repair`)** cedo: reusa
   `layout()`, faz bancos pararem de morrer por falhas parciais e é pré-requisito do P1.
   Depois P4 (from_trusted_page/span), P6 (índice de capacidade) e, por último, **P1 sobre o
   S11** (persistência adiada da raiz, que só é segura com o reparo pronto) e P2 (open sem
   travessia completa).
3. **Qualidade C++ (P2)**: C1, C2, C3, C4 e C6 são pequenos e independentes — bons para
   intercalar; C5 junto com S6 (ambos tocam o ciclo de vida do PageFile); C7 quando a CLI
   for ganhar comandos novos; B2 na primeira mexida no CMake.
4. **Investimento contínuo (P3)**: B3–B7 conforme o projeto ganhe CI/publicação; S8
   (checksum) na próxima quebra planejada de formato; S9 e S10 (WAL/atomicidade — a
   *prevenção* que o reparo do S11 não substitui) pós-MVP, conforme o escopo já previa.

---

## Apêndice — achados avaliados e descartados

Para transparência, itens levantados na auditoria que **não** sobreviveram à verificação
contra o código:

- **"`MODB_PAGE_SIZE` como define PUBLIC causa ODR/incompatibilidade silenciosa"** — o
  superbloco já grava o page_size ([page_file.cpp:137](src/storage/page_file.cpp:137)) e o
  `open` já valida ([page_file.cpp:178-183](src/storage/page_file.cpp:178)); mismatch entre
  builds é erro de runtime detectável, e sem install/export não há consumo fora da árvore
  que crie o cenário de ODR.
- **"`modb db delete` remove qualquer arquivo sem verificar"** — falso: o comando valida o
  arquivo via `PageFile::open` antes de remover ([main.cpp:395-403](apps/modb_cli/main.cpp:395)).
- **"Testes de CLI criam arquivos no source tree"** — os testes registrados são
  `--help`/`--version`/`demo` (só imprime texto) e `codec`/`catalog` (em memória);
  `demo run`, que cria arquivo, não está registrado como teste.
