# moDb

`moDb` será um banco de dados relacional embutido, persistente em arquivo e
implementado em C++26.

O desenvolvimento será incremental. O primeiro objetivo é completar um fluxo
vertical pequeno e confiável:

```text
criar tabela -> inserir linha -> salvar -> fechar -> reabrir -> consultar
```

## Escopo inicial

- execução no mesmo processo da aplicação;
- um processo acessando o banco por vez;
- armazenamento paginado em um único arquivo;
- subconjunto de SQL implementado gradualmente;
- testes automatizados desde a primeira etapa;
- correção, durabilidade e formato de arquivo bem definido antes de otimizações.

## Primeiro marco funcional

```sql
CREATE TABLE users (id INT, name TEXT);
INSERT INTO users VALUES (1, 'Ana');
SELECT * FROM users;
```

Após fechar e abrir novamente o programa, a consulta deverá continuar
retornando a linha inserida.

## Documentação

- [Plano de desenvolvimento](docs/PLANO_DE_DESENVOLVIMENTO.md)
- [Escopo do MVP](docs/ESCOPO_MVP.md)
- [Glossário](docs/GLOSSARIO.md)
- [Formato de arquivo](docs/FORMATO_DE_ARQUIVO.md)
- [ADR 0001 — formato de armazenamento](docs/decisions/0001-formato-de-armazenamento.md)
- [ADR 0002 — tipos e erros](docs/decisions/0002-tipos-e-erros.md)

## Build

Requisitos: CMake 3.30 ou superior, Ninja e um compilador com suporte ao modo
C++26.

```powershell
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

O projeto seleciona C++26 sempre que o compilador oferece esse recurso. O
toolchain GCC 13.1 fornecido pelo CLion desta máquina ainda não oferece esse
modo, portanto o CMake usa C++23 como fallback local e exibe um aviso. O preset
abaixo permite reproduzir essa configuração explicitamente:

```powershell
cmake --preset local-gcc13
cmake --build --preset local-gcc13
ctest --preset local-gcc13
```

Esse fallback não deve ser usado para validar funcionalidades que dependam de
C++26. Para exigir C++26 e rejeitar compiladores antigos, configure com
`-DMODB_ALLOW_CXX23_FALLBACK=OFF`.

O tamanho da página também é definido em tempo de compilação. O padrão é
4 KiB; os presets abaixo produzem builds otimizadas com símbolos e páginas de
8 ou 16 KiB:

```powershell
cmake --preset profile-8k
cmake --build --preset profile-8k
ctest --preset profile-8k

cmake --preset profile-16k
cmake --build --preset profile-16k
ctest --preset profile-16k
```

Cada executável abre somente bancos com o tamanho de página para o qual foi
compilado. O valor fica gravado no superbloco e é validado durante a abertura.

### Testes de 10.000 registros

O teste de carga insere 10.000 registros, sincroniza o arquivo, remove todos,
sincroniza novamente, valida o heap vazio e reabre o banco. Ele imprime os
tempos e as taxas de cada fase. A variante original remove na ordem crescente:

A quantidade pode ser alterada em builds de carga dedicadas com
`-DMODB_CHURN_RECORD_TOTAL=50000`, sem mudar o padrão dos presets normais.

```powershell
cmake --build cmake-build-debug --target modb_table_heap_churn_tests
.\cmake-build-debug\modb_table_heap_churn_tests.exe
```

A segunda variante mantém a mesma carga e remove os `RecordId` em ordem inversa:

```powershell
cmake --build cmake-build-debug --target modb_table_heap_reverse_churn_tests
.\cmake-build-debug\modb_table_heap_reverse_churn_tests.exe
```

O terceiro cenário insere 10.000 registros de exatamente 63 bytes, apaga os
5.000 IDs pares e insere outros 5.000 registros do mesmo tamanho. Ele exige que
cada reinserção use um `PageId/SlotId` liberado, avance a geração e não aloque
nenhuma página física nova:

```powershell
cmake --build cmake-build-debug --target modb_table_heap_space_reuse_tests
.\cmake-build-debug\modb_table_heap_space_reuse_tests.exe
```

A quarta variante mantém os 10.000 registros iniciais com 63 bytes, mas usa 65
bytes somente nos 5.000 substitutos. Ela mede o consumo adicional do espaço
livre compactado e verifica se novas páginas foram necessárias:

```powershell
cmake --build cmake-build-debug --target modb_table_heap_space_reuse_65_tests
.\cmake-build-debug\modb_table_heap_space_reuse_65_tests.exe
```

Também pode ser executado pelo CTest:

```powershell
ctest --test-dir cmake-build-debug -L performance --output-on-failure -V
```

A medição é diagnóstica e não exige uma taxa mínima, porque o resultado varia
conforme máquina, toolchain e perfil Debug/Release. O CTest aplica somente um
limite de segurança de 300 segundos.

### CLion

O projeto deve ser executado pelo alvo CMake, não como um arquivo C++ isolado:

1. use **Tools | CMake | Reload CMake Project**;
2. no seletor de execução, escolha **modb_cli**;
3. execute o alvo selecionado.

Se o CLion mostrar `Building main.cpp` e chamar `g++.exe` diretamente, ele está
usando uma configuração de arquivo isolado. Remova essa configuração e selecione
o alvo `modb_cli` gerado pelo CMake.

Para executar a CLI:

```powershell
.\build\debug\modb.exe --version
```

Quando o projeto é compilado pelo perfil padrão do CLion, o executável fica em:

```powershell
.\cmake-build-debug\modb.exe --version
```

## Comandos da CLI

Os exemplos abaixo assumem o build do CLion:

```powershell
$modb = ".\cmake-build-debug\modb.exe"
```

Mostrar todos os comandos:

```powershell
& $modb --help
```

Mostrar um roteiro passo a passo que percorre todos os comandos:

```powershell
& $modb demo
```

O roteiro usa um arquivo novo chamado `demo.modb` e deve ser seguido na ordem
apresentada, pois os identificadores das páginas dependem das etapas anteriores.

Para executar automaticamente o roteiro inteiro:

```powershell
& $modb demo run
```

O comando recusa sobrescrever um `demo.modb` existente. Quando todas as etapas
terminam com sucesso, a última delas valida e remove o banco de demonstração.

Para apagar automaticamente um `demo.modb` existente antes de iniciar:

```powershell
& $modb demo run -force
```

Criar um banco paginado novo:

```powershell
& $modb db create exemplo.modb
```

Abrir, validar e mostrar os metadados:

```powershell
& $modb db info exemplo.modb
```

Validar o formato sem mostrar todos os metadados:

```powershell
& $modb db check exemplo.modb
```

Acrescentar uma página zerada:

```powershell
& $modb page create exemplo.modb
```

Mostrar uma página em hexadecimal, usando seu `PageId` decimal:

```powershell
& $modb page info exemplo.modb 0
& $modb page info exemplo.modb 1
```

Alocar e formatar uma slotted page para registros:

```powershell
& $modb record page-create exemplo.modb
```

O comando mostra o `PageId` criado. Usando a página `1`, inserir duas linhas
persistentes no formato `(INTEGER, TEXT)`:

```powershell
& $modb record insert exemplo.modb 1 1 "Ana"
& $modb record insert exemplo.modb 1 2 "Beatriz Souza"
```

Inserir uma linha com quantidade e tipos escolhidos na CLI:

```powershell
& $modb record insert-values exemplo.modb 1 integer:1 text:"Ana" boolean:true real:85.5
```

Também é possível usar `NULL` e textos com espaços:

```powershell
& $modb record insert-values exemplo.modb 1 integer:2 "text:Beatriz Souza" boolean:false null
```

Sintaxes aceitas:

```text
null
boolean:true
boolean:false
integer:-42
real:85.5
text:qualquer conteúdo
```

Os nomes dos tipos são escritos em minúsculas. `REAL` precisa ser finito e
`INTEGER` precisa caber em 64 bits com sinal.

Ler um registro pelo `SlotId` retornado:

```powershell
& $modb record read exemplo.modb 1 0
```

Listar todos os registros da página:

```powershell
& $modb record list exemplo.modb 1
```

Explicar o layout da página sem interpretar bytes manualmente:

```powershell
& $modb record page-layout exemplo.modb 1
```

O comando mostra os intervalos do cabeçalho, diretório, espaço livre e registros,
além do offset e tamanho apontados por cada `SlotId`.

Esses comandos de baixo nível persistem linhas, mas exigem `PageId` e `SlotId`
manuais. Para deixar o `TableHeap` escolher e encadear páginas automaticamente,
crie uma raiz:

```powershell
& $modb heap create exemplo.modb
```

Guarde o `Root page` mostrado pelo comando. Se ele for `2`, insira linhas sem
escolher a página física:

```powershell
& $modb heap insert-values exemplo.modb 2 integer:1 text:"Ana" boolean:true
& $modb heap insert-values exemplo.modb 2 integer:2 "text:Beatriz Souza" boolean:false
```

O `Root page` identifica uma página `THRP` dedicada; os registros começam em
outra página física. A raiz persiste a primeira e a última página de dados, além
das quantidades de páginas e registros. O heap procura espaço e cria outra
página automaticamente quando necessário. Ao abrir, ele reconstrói um mapa de
capacidade de inserção: páginas antigas com espaço recuperado são reutilizadas e
o append usa diretamente a última página persistida.
Para ler todas as linhas e inspecionar a cadeia:

```powershell
& $modb heap scan exemplo.modb 2
& $modb heap layout exemplo.modb 2
```

O scan mostra cada endereço no formato `página:slot:geração`. Se a raiz for a
página 2, a primeira página de dados normalmente será a 3. Para atualizar
`3:0:1`:

```powershell
& $modb heap update-values exemplo.modb 2 3 0 1 integer:1 text:"Ana Maria" boolean:true
```

Para remover essa ocupação e liberar sua capacidade:

```powershell
& $modb heap delete exemplo.modb 2 3 0 1
```

Cada registro ocupa exatamente seu tamanho lógico. Um `UPDATE` preserva o slot e
a geração quando o novo conteúdo ainda cabe na mesma página; caso contrário, o
registro pode ser realocado e o comando mostra seu novo endereço. Após `DELETE`,
a página é compactada e o slot pode ser reutilizado com uma geração diferente.

O formato atual usa a versão `3` da slotted page e uma raiz `THRP` versão `1`.
Heaps criados pelo formato anterior, cuja raiz também armazenava registros, são
rejeitados como incompatíveis; durante esta fase de desenvolvimento, crie
novamente os bancos usados em testes manuais.

Uma linha individual ainda precisa caber em uma página. O limite do registro
codificado é 4.060 bytes na build padrão de 4 KiB, 8.156 bytes na variante de
8 KiB e 16.348 bytes na variante de 16 KiB; registros maiores não são divididos.

Quando terminar os exemplos persistentes, validar e apagar definitivamente o
arquivo moDb:

```powershell
& $modb db delete exemplo.modb
```

Por segurança, `delete` abre e valida o banco antes da remoção. O comando não
apaga arquivos comuns nem arquivos moDb corrompidos.

Exercitar `Row -> bytes -> Row` somente em memória:

```powershell
& $modb codec
```

Exercitar criação de tabela, inserção e scan somente em memória:

```powershell
& $modb catalog
```

`codec` e `catalog` não alteram o arquivo. A persistência relacional
ainda será implementada sobre as páginas existentes.

## Estado

O projeto possui a fundação de build e testes, o modelo de dados, um catálogo em
memória, armazenamento paginado, codecs binários, slotted pages persistentes e
um `TableHeap` que insere e faz scan através de várias páginas. A próxima etapa
é persistir os schemas e metadados do catálogo.
