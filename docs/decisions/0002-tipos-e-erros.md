# ADR 0002 — Tipos SQL e tratamento de erros

> ⚠️ **Parcialmente supersedida pelo pivô OO.** A política de erros
> (`std::expected`, sem exceções no fluxo normal, código estável + mensagem)
> segue válida. Os tipos SQL são superados pelos tipos de atributo de objeto em
> [ADR-003](ADR-003-tipos-e-encoding.md).

- Estado: superseção parcial (ver aviso acima); política de erros ainda aceita
- Data: 2026-07-13

## Contexto

Tipos e erros atravessam todas as camadas. Decisões tardias aqui causariam
mudanças no catálogo, formato de linhas, parser e API pública.

## Decisão

- `INTEGER` usa `std::int64_t`.
- `REAL` usa `double` no formato IEEE 754.
- `BOOLEAN` possui apenas `true` e `false` quando não é nulo.
- `TEXT` contém bytes UTF-8 válidos e preserva seu conteúdo.
- `NULL` é um estado separado do valor e não um valor sentinela.
- Conversões implícitas com perda de informação são rejeitadas.
- Comparações envolvendo `NULL` seguirão lógica ternária quando o SQL for
  implementado.
- Falhas recuperáveis serão representadas por um tipo de erro do projeto e
  retornadas com `std::expected`; exceções não serão usadas para fluxo normal.
- Erros incluem uma categoria estável e uma mensagem contextual para humanos.

## Consequências

O modelo lógico permanece explícito e testável. A semântica completa de
comparação, ordenação e coerção será detalhada antes da implementação de
expressões SQL.

