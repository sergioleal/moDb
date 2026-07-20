# Arquitetura de Streaming Assíncrono do Ring0

## Streaming como Modelo Fundamental de Execução

Versão: 0.1

---

# Introdução

No Ring0, **streaming não é uma otimização**.

Streaming é o modelo nativo de execução de consultas.

Toda consulta é considerada um fluxo contínuo de objetos.

O banco nunca espera o término da consulta para iniciar o envio dos resultados.

---

# Filosofia

O objetivo do banco não é responder consultas.

O objetivo do banco é produzir um fluxo de objetos.

A consulta apenas descreve como esse fluxo será produzido.

---

# Modelo Mental

Não existe

```text
Consulta

↓

Lista de Objetos

↓

Cliente
```

Existe

```text
Consulta

↓

Fluxo

↓

Cliente
```

---

# Objetivos

* minimizar o tempo até o primeiro resultado
* eliminar buffers desnecessários
* reduzir consumo de memória
* permitir processamento concorrente cliente/servidor
* permitir consultas extremamente grandes
* permitir processamento contínuo

---

# Time To First Result

A principal métrica do banco deixa de ser

```text
Tempo total da consulta
```

e passa a ser

```text
TTFR
(Time To First Result)
```

Idealmente

```text
Cliente envia consulta

↓

Planner

↓

Primeiro objeto encontrado

↓

Primeiro objeto enviado
```

Sem qualquer espera artificial.

---

# Fluxo Geral

```text
Cliente

↓

Query

↓

Planner

↓

Executor

↓

Storage

↓

Objeto encontrado

↓

Projeção

↓

Serialização

↓

Rede

↓

Desserialização

↓

Aplicação
```

Todo o pipeline funciona continuamente.

---

# Pipeline

O pipeline possui diversos estágios.

```text
Storage Scan

↓

Predicate

↓

Projection

↓

Serialization

↓

Network

↓

Client

↓

Application
```

Cada estágio trabalha de maneira independente.

Nenhum estágio precisa aguardar o término do anterior.

---

# Execução Assíncrona

Cada componente do pipeline pode suspender sua execução.

Exemplo

```text
Storage

↓

produz objeto

↓

Serializer ocupado

↓

Storage suspende

↓

Serializer continua

↓

Network envia

↓

Storage retoma
```

---

# Coroutines

A implementação recomendada utiliza C++20 Coroutines.

Exemplo conceitual

```cpp
Task<void> execute(QueryPlan plan)
{
    auto cursor = co_await plan.open();

    while(auto object = co_await cursor.next())
    {
        auto result =
            co_await plan.project(*object);

        co_await output.write(result);
    }

    co_await output.complete();
}
```

---

# Rationale

Não existem threads bloqueadas aguardando I/O.

Todo o pipeline permanece naturalmente assíncrono.

---

# Resultado Individual

Cada objeto é enviado imediatamente.

Nunca

```text
Objeto

↓

Buffer

↓

Buffer

↓

Buffer

↓

Cliente
```

Sempre

```text
Objeto

↓

Cliente
```

---

# Lotes

O Ring0 não possui lotes lógicos.

O servidor nunca espera acumular

* 100 registros
* 1000 registros
* 10 MB

para iniciar o envio.

Cada resultado é elegível para transmissão imediatamente após sua produção.

---

# Framing

Embora não existam lotes lógicos

o protocolo físico continua utilizando frames.

Frames de objetos podem usar um diretório de slots e coalescer resultados que
já estejam disponíveis. Isso não cria espera para formar lote: o primeiro
resultado continua imediatamente elegível, e um frame com um único slot é
válido. O layout é definido na
[ADR-010](docs/decisions/ADR-010-protocolo-binario-proximo-do-armazenamento.md).

Cada frame pode ser comprimido de forma independente quando o codec tiver sido
negociado e houver redução material. Frames pequenos ou incompressíveis seguem
sem compressão. Não há dicionário compartilhado entre frames inicialmente, e o
servidor nunca atrasa o envio para acumular mais dados para compressão.

Isso é responsabilidade da camada de transporte.

Não do banco.

---

# Backpressure

Streaming infinito exige controle de fluxo.

Caso o cliente fique lento

o servidor deve reduzir naturalmente sua velocidade.

Modelo

```text
Cliente lento

↓

Rede bloqueia

↓

Serializer suspende

↓

Executor suspende

↓

Storage suspende
```

Todo o pipeline desacelera automaticamente.

---

# Rationale

Nenhum componente acumula memória indefinidamente.

---

# Memória

A consulta nunca materializa todos os resultados.

Modelo incorreto

```cpp
std::vector<Result> results;
```

Modelo correto

```cpp
Cursor

Objeto Atual

Estado da Consulta
```

Complexidade

```text
O(1)
```

em relação ao número de resultados produzidos.

---

# Cursor

O executor mantém apenas o estado mínimo.

```text
Cursor

↓

Página Atual

↓

Slot Atual

↓

Estado do Plano
```

Nada mais.

---

# Processamento Paralelo

Enquanto

o servidor encontra

o objeto N+1

o cliente já pode estar processando

o objeto N.

```text
Servidor

Objeto 10

Objeto 11

Objeto 12

Cliente

Processa 10

Processa 11

Processa 12
```

Não existe sincronização desnecessária.

---

# Streaming Completo

O pipeline completo funciona simultaneamente.

```text
Storage

↓

Predicate

↓

Projection

↓

Serializer

↓

Socket

↓

Client

↓

Business Logic
```

Todos os componentes trabalham em paralelo.

---

# Snapshot

Toda consulta utiliza um Snapshot MVCC.

```text
Consulta

↓

Snapshot

↓

Todos os objetos pertencem ao mesmo Snapshot
```

Mesmo durante consultas longas.

---

# Consistência

Objetos modificados durante o streaming

não alteram

os objetos já produzidos.

Toda a consulta enxerga exatamente o mesmo estado lógico.

---

# Cancelamento

O cliente pode interromper a consulta a qualquer momento.

```text
Cliente

↓

Cancel

↓

Executor

↓

Cursor

↓

Fim
```

Não existe trabalho desnecessário após o cancelamento.

---

# Timeout

Streams infinitos podem permanecer abertos durante horas.

O servidor pode aplicar políticas como

* timeout
* limite de recursos
* cancelamento administrativo

---

# Operadores Streaming

Naturalmente Streaming

* Scan
* Index Scan
* Predicate
* Projection
* Computed Functions
* Limit

---

# Operadores Parcialmente Bloqueantes

* Top K
* Merge

---

# Operadores Bloqueantes

* Sort sem índice
* Aggregate Global
* Distinct Global

---

# Planner

O Planner conhece a natureza de cada operador.

```text
Streaming

Parcialmente Bloqueante

Bloqueante
```

Isso permite estimar

* TTFR
* memória
* latência

antes da execução.

---

# Computed Functions

Funções registradas também funcionam em streaming.

```text
Objeto

↓

Computed Function

↓

Predicate

↓

Cliente
```

Nenhuma mudança no pipeline.

---

# Protocolo

O protocolo trabalha com mensagens independentes.

```text
Stream Begin

↓

Object

↓

Object

↓

Object

↓

Object

↓

Stream End
```

Não existe

```text
Lista de Objetos
```

---

# Erros

Caso ocorra erro

após alguns objetos já terem sido enviados

o servidor transmite

```text
Stream Error
```

O cliente decide como tratar os resultados já recebidos.

---

# Cliente

O cliente nunca aguarda

o término da consulta.

Modelo

```cpp
auto stream =
    query.stream();

while(auto object =
      co_await stream.next())
{
    process(object);
}
```

---

# Benefícios

* menor latência
* menor memória
* maior throughput
* paralelismo natural
* melhor experiência para o usuário
* consultas praticamente ilimitadas
* excelente comportamento em redes lentas

---

# Streaming como Fundamento

Streaming não é uma API.

Streaming não é um modo de execução.

Streaming é a arquitetura central do banco.

Todo o restante do sistema é construído sobre esse princípio.

---

# Princípios Fundamentais

* Toda consulta produz um fluxo.
* Resultados são enviados imediatamente após serem produzidos.
* O servidor nunca aguarda o término da consulta.
* O servidor nunca cria lotes lógicos.
* O pipeline é completamente assíncrono.
* Coroutines são o modelo recomendado de implementação.
* O consumo de memória é independente do número de resultados.
* Backpressure é propagado até o Storage Engine.
* Toda consulta opera sobre um Snapshot consistente.
* O cliente pode cancelar a consulta a qualquer momento.
* O protocolo transmite objetos individualmente.
* O principal indicador de desempenho é o **Time To First Result (TTFR)**.
