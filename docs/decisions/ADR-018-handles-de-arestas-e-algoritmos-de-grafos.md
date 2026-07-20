# ADR-018 — Handles de arestas e algoritmos de grafos

- Estado: aceito para a Fase 12
- Data: 2026-07-19

## Contexto

O Ring0 já persiste relacionamentos tipados por `Ref<T>` e `OwnedRef<T>`,
coleções de referências e objetos embutidos. A Fase 4 comprovou criação,
reabertura, resolução e cascata, mas o consumidor ainda precisa percorrer
esses relacionamentos manualmente e não dispõe de algoritmos de grafos.

Um relacionamento entre classes precisa poder ser tratado como uma aresta
tipada sem transformar identidade de runtime em formato persistente. Em
particular, `DatabaseId` identifica uma instância aberta e não é estável após
reabertura.

## Decisão

**A Fase 12 mantém `Ref<T>`/`OwnedRef<T>` como representação persistente e
introduz `EdgeHandle<From, To, Kind>` como visão tipada runtime da aresta.**

```cpp
enum class EdgeKind : std::uint8_t { association, ownership };

template <class From, class To,
          EdgeKind Kind = EdgeKind::association>
class EdgeHandle {
public:
    DatabaseId database() const noexcept;
    ObjectId source_id() const noexcept;
    ObjectId target_id() const noexcept;
    FieldId field() const noexcept;

    Result<From> source(const Snapshot&) const;
    Result<To> target(const Snapshot&) const;
    Result<bool> dangling(const Snapshot&) const;
};
```

1. **Runtime-only.** O handle contém `DatabaseId`, origem, alvo e `FieldId`;
   não recebe tag de codec e nunca é persistido. O objeto continua gravando
   apenas `Ref<T>` ou `OwnedRef<T>`.
2. **Construção validada.** Apenas `Database`/`GraphView` constroem handles,
   a partir de um membro de relacionamento tipado. `Embedded<T>` não forma
   aresta porque não possui `ObjectId`.
3. **Sem quarta categoria de relacionamento.** `EdgeHandle` representa em
   runtime associação ou composição já existentes; não altera a semântica da
   ADR-008.
4. **Arestas com propriedades.** Devem ser objetos persistentes próprios,
   com referências para as extremidades. Arestas paralelas sem objeto próprio
   não ganham identidade artificial.
5. **Snapshot único por travessia.** Todos os vértices e arestas de uma
   execução são resolvidos sob o mesmo `Snapshot`, evitando misturar épocas.
6. **Algoritmos básicos.** A fase entrega BFS, DFS, caminho mínimo sem peso,
   detecção de ciclo, ordenação topológica e componentes conexos para uma view
   explicitamente não direcionada.
7. **Streaming e limites.** BFS/DFS produzem `Generator<Result<GraphVisit<T>>>`
   e aceitam cancelamento, profundidade e máximo de vértices. O conjunto de
   visitados é O(V), ainda que a saída seja lazy.
8. **Referências órfãs.** A travessia escolhe política explícita `fail`, `skip`
   ou `yield_error`; nunca ignora silenciosamente uma `Ref` pendente.
9. **Direção.** Arestas de saída usam os campos do objeto. Arestas de entrada
   exigem índice no campo `Ref`; não há varredura reversa implícita ilimitada.
   A view declara `outgoing`, `incoming` ou `both`.
10. **Ownership.** `OwnedRef` pode ser incluída por opção, mas sua topologia
    válida permanece uma árvore. Retarget de composição fica fora da fase.

## Consequências

- Relacionamentos entre classes podem ser consumidos como arestas tipadas sem
  mudar o formato do banco.
- O handle preserva contexto suficiente para resolver extremidades, detectar
  refs órfãs e localizar arestas de entrada indexadas.
- Grafos heterogêneos exigem que a view conheça os tipos/bindings envolvidos;
  `ObjectId` sozinho não seleciona uma classe C++.
- Coleções de `Ref<T>` podem fornecer adjacência, embora o cursor por página
  continue uma otimização separada.
- Novos erros da Fase 12: `invalid_edge`, `graph_limit_exceeded`,
  `graph_cycle` e `edge_target_not_found`.
- Operações de grafo podem ser expostas por facades sem alterar o contrato
  persistente.
