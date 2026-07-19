# Execução de Código de Domínio no ODB++

## Arquitetura de Módulos C++ Executáveis

Versão: 0.2

---

# Objetivo

O ODB++ permite que aplicações registrem código C++ executável dentro do servidor do banco.

O objetivo não é reproduzir Stored Procedures SQL, mas permitir que regras de domínio, algoritmos e operações transacionais sejam executados próximas dos dados.

Esse modelo reduz latência, elimina múltiplas chamadas de rede e permite que operações complexas sejam executadas dentro de uma única transação.

---

# Filosofia

O banco não executa SQL.

O banco executa **operações de domínio**.

A aplicação não envia consultas SQL nem código executável ao servidor. Ela chama
um método C++ registrado e recebe seu resultado tipado. A decisão completa está
registrada na [ADR-012](docs/decisions/ADR-012-runtime-de-modulos-no-processo.md).

Em vez de:

```sql
BEGIN;

UPDATE account
SET balance = balance - 100
WHERE id = 10;

UPDATE account
SET balance = balance + 100
WHERE id = 20;

COMMIT;
```

A aplicação simplesmente executa

```cpp
TransferFunds{
    source,
    destination,
    Money{100}
};
```

Todo o restante é responsabilidade do banco.

---

# Consultas Internas

O banco continua possuindo mecanismo interno de consulta, planejamento, índices
e streaming. Operações de domínio usam essas capacidades por uma API C++ tipada
exposta pelo `ExecutionContext`.

```cpp
ctx.objects()
    .query<Invoice>()
    .where(field<&Invoice::due_date>() < today)
    .stream();
```

SQL não é a interface pública do produto. A ausência de SQL público não implica
abrir mão de um motor de consultas eficiente.

---

# Objetivos

* reduzir round-trips de rede
* executar lógica próxima dos dados
* reutilizar regras de negócio
* manter atomicidade
* permitir algoritmos complexos
* facilitar migrações
* permitir processamento batch

---

# Modelo Arquitetural

```text
                 Client

                    │

             RPC / Binary Protocol

                    │

                    ▼

          Database Server Process

 ┌─────────────────────────────────────┐

 │ Storage Engine                      │

 │ Buffer Pool                         │

 │ WAL                                 │

 │ Transaction Manager                 │

 │ Object Manager                      │

 │ Schema Manager                      │

 │                                     │

 │ Domain Module Runtime               │

 └─────────────────────────────────────┘
```

Todo código de domínio executa dentro do Database Server.

---

# Rationale

A instância do banco é dedicada a uma única aplicação.

Não existe compartilhamento entre clientes diferentes.

Logo, não existe necessidade de isolamento entre tenants.

A prioridade passa a ser desempenho.

---

# Unidade de Execução

Uma operação representa uma unidade transacional.

Exemplo

```cpp
TransferFunds

GenerateInvoice

CloseMonth

CreateOrder

AllocateResources

RunMigration
```

Cada operação possui uma única responsabilidade.

---

# Interface Base

```cpp
class Operation
{
public:

    virtual ~Operation() = default;

    virtual OperationResult execute(
        ExecutionContext&
    ) = 0;
};
```

---

# Registro

Durante a inicialização

```cpp
registry.register<
    TransferFunds
>(
    "account.transfer"
);

registry.register<
    CloseMonth
>(
    "finance.close-month"
);
```

---

# Execução

Cliente

```cpp
client.call<
    TransferFunds
>(
    source,
    destination,
    amount
);
```

No protocolo

```text
OperationId

account.transfer

Arguments

ObjectId
ObjectId
Money
```

Servidor

```text
Operation Registry

↓

TransferFunds

↓

execute()

↓

Transaction

↓

Commit
```

---

# Rationale

Nenhum código C++ trafega pela rede.

Apenas:

* identificador da operação
* parâmetros

---

# Facades e handles (Fase 11)

As operações continuam sendo a unidade de execução (Fase 9). A Fase 11 as
agrupa em **facades** descobríveis e dá ao consumidor um **handle** tipado
para invocar métodos daquela facade.
([ADR-014](docs/decisions/ADR-014-catalogo-de-facades-e-handles.md)).

O servidor mantém um catálogo heterogêneo:

```cpp
std::vector<FacadeDescriptor> catalog;
// cada FacadeDescriptor: FacadeId estável, versão, vector<MethodDescriptor>
// a posição no vetor NÃO é identidade pública
```

Fluxo do consumidor:

```text
Cliente
  → open_facade<"accounts">(versão)
  → FacadeHandle
  → invoke<TransferFunds>(args...)
  → OperationRegistry / OpCall
  → Transaction → Commit | Rollback
```

Exemplo:

```cpp
auto accounts = client.open_facade<Accounts>("accounts", 1);
auto r = accounts.invoke<TransferFunds>(source, destination, amount);
```

Descoberta lista facades e métodos. Versão incompatível,
facade ausente ou método que não pertence à facade são rejeitados
(`incompatible_facade_version`, `facade_not_found`,
`facade_method_not_found`). O handle não substitui o registry: apenas
resolve o método e delega a invocação.

---

# ExecutionContext

Toda operação recebe um contexto.

```cpp
class ExecutionContext
{
public:

    Transaction& transaction();

    ObjectAccess& objects();

    Logger& logger();
};
```

---

# Responsabilidade

ExecutionContext representa a única porta de entrada para o banco.

A operação nunca conversa diretamente com componentes internos.

---

# API Transacional

Toda alteração persistente deve ocorrer através da API transacional.

Exemplos

```cpp
handle.set(...);

handle.get(...);

tx.create<T>();

tx.remove(...);

collection.push_back(...);
```

Nunca

```cpp
Page*

BufferPool*

ObjectStore*

Index*

WalManager*
```

---

# Princípio Fundamental

> O servidor confia no código de domínio, mas o código só pode alterar dados persistentes através da API transacional.

---

# Motivação

O código C++ é considerado confiável.

Porém a consistência do banco pertence ao Storage Engine.

Isso garante:

* WAL
* rollback
* commit
* MVCC
* índices
* constraints
* relacionamentos
* cascatas
* observabilidade

sem depender da disciplina do desenvolvedor.

---

# Exemplo

```cpp
class TransferFunds
    : public Operation
{
public:

OperationResult execute(
    ExecutionContext& ctx
)
{
    auto& tx =
        ctx.transaction();

    auto source =
        tx.get<Account>(10);

    auto destination =
        tx.get<Account>(20);

    auto balance =
        source.get<
            &Account::balance
        >(tx);

    if(balance < 100)
        throw
            InsufficientFunds{};

    source.set<
        &Account::balance
    >(tx,balance-100);

    destination.set<
        &Account::balance
    >(
        tx,
        destination.get<
            &Account::balance
        >(tx)+100
    );

    return Success{};
}

};
```

---

# Commit

Caso execute normalmente

```text
execute()

↓

commit
```

Caso ocorra exceção

```text
execute()

↓

rollback
```

---

# Persistência

As operações nunca manipulam páginas.

Elas manipulam objetos.

```text
Handle

↓

Transaction

↓

ObjectManager

↓

Storage
```

---

# Benefícios

O domínio permanece completamente desacoplado do Storage Engine.

---

# Objetos

Operações trabalham apenas com

```cpp
Handle<T>

PersistentVector<T>

Ref<T>

OwnedRef<T>
```

Nunca

```cpp
Page

RID

Offset

Buffer

Bytes
```

---

# Segurança

Como a instância do banco é dedicada a uma única aplicação

o código C++ é considerado confiável.

Logo

não existe sandbox.

Isso não significa aceitar binários enviados pelo cliente. Módulos são carregados
somente de uma origem confiável configurada pelo operador, fora do controle da
aplicação remota.

---

# Origem Confiável e Carregamento

O primeiro runtime carrega módulos nativos de uma localização administrativa
configurada para a instância. Cada módulo possui manifesto e hash conhecido.

Na carga, o servidor valida:

* identificador e versão do módulo
* versão da API do runtime
* baseline compatível
* métodos exportados e modo `read_only` ou `read_write`
* hash presente no manifesto ou em allowlist administrativa

Pela rede trafegam apenas `OperationId`, versão, argumentos e resultados
serializados. O cliente nunca escolhe um caminho de arquivo nem envia um
binário para execução.

---

# O que isso significa

O desenvolvedor é responsável por:

* ponteiros
* memória
* concorrência
* comportamento indefinido
* performance

Assim como qualquer outro componente C++ da aplicação.

---

# O que continua protegido

Mesmo sendo confiável

o código não possui acesso ao armazenamento físico.

Não pode modificar

* páginas
* índices
* WAL
* Buffer Pool

Apenas através da API pública.

---

# Rationale

O objetivo não é proteger o desenvolvedor dele mesmo.

O objetivo é preservar os invariantes do banco.

---

# Processo

Os módulos executam dentro do mesmo processo da instância.

Essa é uma decisão da primeira implementação. Workers isolados, sandbox e
atualização a quente serão avaliados depois que o contrato de métodos estiver
validado.

```text
Database Process

├── Storage

├── Transactions

├── Object Runtime

└── Domain Modules
```

---

# Motivação

Vantagens

* zero IPC
* máxima performance
* acesso direto às APIs
* menor latência
* menor consumo de memória

---

# Falhas

Caso uma operação gere

```text
segmentation fault
```

a instância será encerrada.

---

# Recuperação

A recuperação utiliza

```text
Supervisor

↓

Restart

↓

WAL Recovery

↓

Ready
```

Como cada instância pertence a apenas uma aplicação

não existe impacto em outros clientes.

---

# Supervisor

A recomendação é executar cada instância sob supervisão.

Exemplos

* systemd
* Kubernetes
* Windows Service Recovery
* Docker Restart Policy

---

# Compatibilidade

Os módulos possuem manifesto.

```cpp
struct ModuleManifest
{
    ModuleId id;

    Version version;

    BaselineId baseline;

    ApiVersion api;

    BinaryHash hash;

    std::vector<ExportedMethod> methods;
};
```

---

# Validação

Na carga

o banco verifica

* API
* versão
* baseline
* compatibilidade

---

# Migrações

As migrações também são operações.

```text
MigrationOperation

↓

ExecutionContext

↓

Transaction

↓

Projection

↓

Persistência
```

Assim

todo o mecanismo de migração reutiliza exatamente a mesma infraestrutura.

---

# Biblioteca de Compatibilidade

A biblioteca de compatibilidade pode registrar

```text
Migration Operations

Compatibility Operations

Schema Upgrade Operations

Import Operations
```

Tudo utilizando a mesma API.

---

# Benefícios

Uma única infraestrutura suporta

* regras de domínio
* processamento batch
* migrações
* manutenção
* importação
* exportação
* consistência
* compactação
* operações administrativas

---

# Responsabilidades

## Storage Engine

Persistência.

---

## Transaction Manager

Atomicidade.

---

## Object Manager

Materialização.

---

## Domain Module

Regras de negócio.

---

## Compatibility Module

Retrocompatibilidade.

---

## Migration Module

Conversão de versões.

---

# Princípios Fundamentais

* O banco executa operações de domínio e não SQL.
* Toda operação é executada dentro de uma transação.
* O código C++ é considerado confiável.
* A instância do banco é dedicada a uma única aplicação.
* Não existe necessidade de sandbox.
* O código nunca manipula armazenamento físico diretamente.
* Toda alteração persistente ocorre através da API transacional.
* O Storage Engine permanece proprietário absoluto da persistência.
* Falhas do módulo encerram apenas a instância da aplicação.
* A recuperação ocorre automaticamente através de WAL Recovery.
* Migrações utilizam exatamente a mesma infraestrutura de execução das operações de domínio.
