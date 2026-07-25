# Guia da pasta `docs-process/`

Esta pasta reúne a documentação **de processo**: como e em que ordem o Ring0
foi construído, fase a fase. Nada aqui é necessário para usar o produto —
para isso, veja [`docs/`](../docs/README.md), especialmente o
[DEVELOPER_GUIDE.md](../docs/DEVELOPER_GUIDE.md). Esta pasta existe para
quem quer entender a história, o roadmap, ou revisitar como uma capacidade
específica foi entregue e testada.

## Linha do tempo em uma frase

```text
Visão (3 MDs na raiz do repo) → Plano OO → Protocolo por fase → ADRs (em docs/decisions/)
                                                                      ↑
                              (substituem o Plano/Escopo/ADRs relacionais legados)
```

## Quer saber só "onde estamos agora"?

**[RASTREADOR.md](RASTREADOR.md)** — o rastreador de andamento. Lista as
~165 tarefas das 16 fases (0–15) com status (`⬜`/`🔄`/`✅`/`🚫`), o
teste automatizado de cada fase e o painel geral de progresso. É o único
documento desta pasta que reflete estado vivo; os demais (Plano, Protocolo)
definem escopo e não mudam a cada tarefa concluída.

## 1. Plano e protocolo

- **[PLANO_ODB.md](PLANO_ODB.md)** — o plano de desenvolvimento do Ring0.
  Traduz os três documentos de visão da raiz do repositório
  (`arquitetura.md`, `codigo-local.md`, `streaming.md`) em **16 fases
  verticais** (0–15), cada uma com objetivo, tarefas, entregáveis e critério
  de aceite. Define o MVP OO (fases 0–3) e a ordem de execução seguida.
- **[PROTOCOLO_FASES.md](PROTOCOLO_FASES.md)** — o mesmo plano, mas no nível
  de execução: para cada fase do `PLANO_ODB.md`, especifica os arquivos
  criados, os layouts binários byte a byte, as assinaturas de API e os testes
  automatizados caso a caso.

- **[PLANO_IMPLEMENTACAO_CARGA.md](PLANO_IMPLEMENTACAO_CARGA.md)** — o mesmo
  par "o quê / como", mas para os testes de carga: a especificação vive em
  [`docs/PLANO_TESTES_DE_CARGA.md`](../docs/PLANO_TESTES_DE_CARGA.md) e este
  documento levanta o que já está implementado, o que falta, em que ordem, e
  serve de rastreador das subfases (A–R). Trabalho iniciado depois da Fase 16,
  por isso fora da numeração de fases do `PLANO_ODB.md`.

Relação entre os dois: `PLANO_ODB.md` é o "o quê e por quê" (nível de
gerência/arquitetura); `PROTOCOLO_FASES.md` é o "como", fase a fase (nível de
implementação). As fases e a numeração são as mesmas nos dois documentos. As
decisões arquiteturais que os dois pressupõem estão em
[`docs/decisions/`](../docs/README.md) (ADRs) — essas ficaram em `docs/` por
serem atemporais, não histórico puro.

## 2. Relatórios de fechamento e medição

- **[FECHAMENTO_10F.md](FECHAMENTO_10F.md)** — matriz final da Fase 10 (tag
  `0.0.10f`): build/teste, demo operacional, benchmark vs. baseline 10A.
- **[BASELINE_DESEMPENHO.md](BASELINE_DESEMPENHO.md)** — baseline de
  desempenho pré-otimização coletada na Fase 10A.
- **[OTIMIZACOES_10C.md](OTIMIZACOES_10C.md)** — otimizações medidas no
  `read_hotpath`, entregável da Fase 10C.
- **[RELATORIO_CHECK_RECOVERY_FASES_5_6.md](RELATORIO_CHECK_RECOVERY_FASES_5_6.md)**
  — relatório de implementação de check/recovery (Fases 5 e 6).

## 3. Curso de treinamento (`training/`)

**[training/en/README.md](training/en/README.md)** — 14 lições em inglês, uma
por fase (`00-version-compatibility` a `13-async-io`), cada uma com um
exemplo standalone em `examples/server/by_phase/`. É um complemento mais
lento e mais profundo ao [DEVELOPER_GUIDE.md](../docs/DEVELOPER_GUIDE.md) —
prefira o guia se você só quer aprender a usar o produto; venha para cá se
quiser o histórico fase a fase com exercícios.

Renderizar em HTML (`training/html/`):

```powershell
py scripts/render_training.py
```

## 4. Documentos supersedidos (modelo relacional abandonado)

Mantidos apenas para histórico — cada um tem um aviso ⚠️ no topo apontando
para o documento vigente que o substitui:

- **[PLANO_DE_DESENVOLVIMENTO.md](PLANO_DE_DESENVOLVIMENTO.md)** — plano de
  marcos do banco relacional. Substituído por `PLANO_ODB.md`.
- **[ESCOPO_MVP.md](ESCOPO_MVP.md)** — escopo do MVP relacional
  (`CREATE TABLE`/`INSERT`/`SELECT`). Substituído pelo MVP OO (fases 0–3 do
  `PLANO_ODB.md`) e pelos limites da
  [ADR-007](../docs/decisions/ADR-007-limites-mvp-oo.md).

**`docs/FORMATO_DE_ARQUIVO.md` não é supersedido** — foi reescrito na Fase
10F e descreve o formato físico vigente do Ring0; por isso fica em `docs/`,
não aqui.

## Qual documento ler primeiro?

- **Quer usar o produto?** Não é aqui — vá para
  [docs/DEVELOPER_GUIDE.md](../docs/DEVELOPER_GUIDE.md).
- **Quer saber o status atual, o que já foi feito?** `RASTREADOR.md`.
- **Quer saber em que ordem cada fase foi executada e por quê?** `PLANO_ODB.md`.
- **Vai revisitar como uma fase específica foi implementada?**
  `PROTOCOLO_FASES.md`, seção da fase; consulte a ADR correspondente em
  `docs/decisions/` para o porquê de cada decisão.
- **Leu algo sobre tabelas, SQL ou `Catalog`/`Schema`/`Row`?** Você caiu em um
  documento supersedido — confira o aviso no topo e vá para o equivalente OO
  em `docs/` ou `docs-process/PLANO_ODB.md`.
