# ADR-008 — Integridade de referências e cascata de composição

- Estado: aceito para o MVP OO (Fase 4)
- Data: 2026-07-17

## Contexto

A Fase 4 introduz três formas de relacionamento entre objetos (arquitetura.md
§9–12):

- `Ref<T>` — associação (◇): o objeto guarda a identidade do alvo e o resolve
  sob demanda;
- `OwnedRef<T>` — composição (◆): o alvo pertence ao pai;
- `Embedded<T>` — objeto sem identidade, serializado no payload do pai.

Duas perguntas precisam de resposta explícita para o formato e a API não
ficarem ambíguos:

1. O que acontece ao remover um objeto ainda referenciado por uma `Ref`?
2. O que acontece com os filhos de uma `OwnedRef` quando o pai é removido?

## Decisão

**Associação (`Ref<T>`): remoção permitida, referência pendente detectável.**
Remover um objeto referenciado por uma `Ref` é permitido e não é bloqueado nem
propagado. A `Ref` continua guardando o `ObjectId` antigo; ao resolvê-la (via
`Database::get<T>(id)`) o banco devolve `record_not_found`. Não há contagem de
referências nem varredura de quem aponta para quem no caminho de escrita: isso
custaria um índice reverso que o MVP não tem. A integridade é **detectável na
leitura**, não **imposta na escrita**. O `database_check` reporta refs órfãs
como **aviso**, nunca como erro (Fase 4, tarefa 4.8).

**Composição (`OwnedRef<T>`): remoção em cascata, profundidade-primeiro.**
Remover o pai remove os filhos apontados por `OwnedRef`, recursivamente e em
pós-ordem (os filhos primeiro, o pai por último), para que uma falha no meio
nunca deixe um pai sem seus filhos. A distinção associação/composição vive na
`AttributeDefinition.is_owned`, gravada no catálogo — o mesmo objeto decodificado
diz quais campos seguir.

**Ciclos e posse compartilhada: falha explícita.** A cascata mantém um conjunto
de ids em andamento. Um id revisitado (posse cíclica `A◆B◆A`, ou o mesmo filho
possuído por dois pais) encerra a operação com `invalid_argument` antes de
remover qualquer objeto ainda pendente na pilha de recursão. Composição saudável
é uma árvore: nenhuma revisita ocorre num grafo bem-formado.

## Consequências

- Remoções de composição são atômicas apenas na ausência de falha de I/O: sem
  transações (Fase 5) uma queda no meio da cascata pode deixar remoção parcial.
  A Fase 5 traz a `OwnedRef` para dentro do WAL e fecha essa janela.
- Não há reaproveitamento de espaço das páginas liberadas no MVP (sem free
  list); as páginas ficam órfãs e visíveis ao `database_check`.
- Uma `Ref` pendente é um estado válido do banco, não corrupção: ferramentas e
  aplicações precisam tratar `record_not_found` na resolução.
- A Fase 12 representa associações/composições com `EdgeHandle` runtime, sem
  criar uma quarta categoria persistente. Algoritmos de grafos declaram como
  tratam refs órfãs (`fail`, `skip` ou `yield_error`) e só incluem
  `OwnedRef` por opção; a topologia válida de ownership continua uma árvore
  ([ADR-015](ADR-015-handles-de-arestas-e-algoritmos-de-grafos.md)).
