# ODB++ — Arquitetura de um Banco de Dados Orientado a Objetos

> Especificação de Arquitetura (Draft)

Versão: 0.2

---

# 1. Visão Geral

O ODB++ é um banco de dados **nativamente Orientado a Objetos**.

Seu objetivo é eliminar completamente o paradigma relacional da API pública.

Para o desenvolvedor existem apenas:

* Objetos
* Atributos
* Relacionamentos
* Composição
* Coleções
* Transações

O banco é responsável por transformar automaticamente esses conceitos em armazenamento persistente.

---

# 2. Objetivos

## Modelo OO puro

O usuário nunca pensa em:

* tabelas
* linhas
* colunas
* joins
* foreign keys
* SQL

Ele trabalha exclusivamente com objetos.

---

## Identidade

Todo objeto possui identidade permanente.

```
ObjectId
```

A identidade nunca muda.

Mesmo que o objeto seja movido fisicamente.

---

## Relacionamentos

Relacionamentos são conceitos nativos.

Não existem chaves estrangeiras.

```
Employee

↓

Department
```

Internamente isso representa apenas:

```
ObjectId
```

---

## Coleções

Coleções possuem armazenamento próprio.

Elas não ficam serializadas dentro do objeto.

---

## Evolução de Schema

O banco permite coexistência de objetos criados por diferentes versões da aplicação.

Não existe necessidade de migração imediata.

---

# 3. Arquitetura Geral

```
                      Database
                          │
      ┌───────────────────┼────────────────────┐
      │                   │                    │
      ▼                   ▼                    ▼
   Catalog         TransactionMgr        BufferPool
      │                   │
      ▼                   ▼
 TypeRegistry      ObjectManager
      │                   │
      ▼                   ▼
RelationshipMgr      ObjectStore
      │                   │
      └──────────────┬────┘
                     ▼
                 BlobStore
```

---

# Rationale

Cada componente possui apenas uma responsabilidade.

Isso reduz acoplamento e facilita testes.

---

# 4. Database

Representa um banco aberto.

Responsabilidades

* abrir
* fechar
* criar transações
* acessar catálogo
* acessar ObjectManager

Não realiza persistência diretamente.

---

# 5. DatabaseRegistry

Os bancos abertos permanecem registrados.

```
DatabaseRegistry

DatabaseId

↓

Database
```

Implementação sugerida

```cpp
unordered_map<
    DatabaseId,
    shared_ptr<Database>
>
```

---

# Rationale

Permite

* múltiplos bancos
* handles pequenos
* controle de ciclo de vida
* independência entre bancos

---

# 6. Handle

Todo acesso ocorre através de um Handle.

```
Handle<Employee>
```

Internamente

```
DatabaseId

ObjectId
```

O Handle representa apenas identidade.

Nunca contém necessariamente o objeto.

---

## API

```cpp
auto employee =
    db.get<Employee>(id);
```

Leitura

```cpp
employee.get<&Employee::salary>();
```

Escrita

```cpp
employee.set<
    &Employee::salary
>(
    tx,
    15000
);
```

---

# Rationale

O Handle é extremamente leve.

Pode ser copiado livremente.

Não existe duplicação de objetos.

---

# 7. Transações

Toda escrita exige transação.

```
Handle

↓

Transaction

↓

ObjectManager
```

Responsabilidades

* WAL
* MVCC
* Locks
* Dirty Pages
* Commit
* Rollback

---

# Rationale

Evita estados intermediários.

Toda alteração é atômica.

---

# 8. ObjectManager

É o núcleo do banco.

Toda operação passa por ele.

Responsabilidades

* criar objetos
* localizar objetos
* serializar atributos
* materializar objetos
* resolver relacionamentos
* acessar BlobStore
* conversar com BufferPool

---

# Rationale

Toda lógica de persistência permanece centralizada.

---

# 9. Objetos

Exemplo

```cpp
struct Employee
{
    String name;

    Ref<Department> department;

    PersistentVector<
        Ref<Project>
    > projects;
};
```

O objeto C++ representa apenas uma projeção.

O formato persistente pertence ao banco.

---

# 10. Identidade

Todo objeto possui

```
ObjectId
```

Nunca muda.

O endereço físico pode mudar.

---

# Rationale

Permite:

* mover páginas
* compactação
* reorganização física

Sem quebrar referências.

---

# 11. Relacionamentos

Existem apenas três categorias.

## Associação

```cpp
Ref<T>
```

```
Employee

◇── Department
```

---

## Composição

```cpp
OwnedRef<T>
```

```
Employee

◆── Address
```

Ao remover o objeto pai.

↓

O filho também é removido.

---

## Objeto Embutido

```cpp
Embedded<T>
```

Não possui identidade.

É serializado junto ao objeto pai.

---

# Rationale

A semântica OO deve ser preservada.

---

## Visão de aresta em runtime

As três categorias acima continuam sendo a semântica persistente. A Fase 12
adiciona `EdgeHandle<From, To, Kind>` como visão tipada de uma associação ou
composição para travessias e algoritmos de grafos:

```cpp
auto edge = db.edge<&Employee::department>(employee, snapshot);
auto department = edge.target(snapshot);
```

O handle carrega `DatabaseId`, origem, alvo e `FieldId`; não é persistido.
`Ref<T>`/`OwnedRef<T>` permanecem no objeto. `Embedded<T>` não forma aresta
porque não possui identidade. A decisão completa está na
[ADR-015](docs/decisions/ADR-015-handles-de-arestas-e-algoritmos-de-grafos.md).

---

# 12. Coleções Persistentes

Coleções possuem identidade própria.

Tipos

```
PersistentVector<T>

PersistentSet<T>

PersistentMap<K,V>
```

Estrutura

```
Employee

↓

BlobId

↓

PersistentVector
```

---

# Rationale

Objetos permanecem pequenos.

Coleções podem crescer indefinidamente.

---

# 13. Blob Store

Armazena

* textos
* documentos
* imagens
* coleções
* binários

Objetos apenas apontam para BlobId.

---

# Rationale

Evita movimentação do objeto principal.

---

# 14. Object Store

Todo objeto possui

```
ObjectHeader

ObjectId

TypeDefinitionId

Payload
```

Payload

```
atributos

ObjectIds

BlobIds
```

---

# Rationale

O banco nunca grava diretamente objetos C++.

---

# 15. Catálogo

O catálogo também é composto por objetos.

```
Catalog

├── Baseline

├── TypeDefinition

├── AttributeDefinition

├── RelationshipDefinition

├── IndexDefinition

└── ConstraintDefinition
```

---

# Rationale

Não existem estruturas especiais.

Tudo é objeto.

---

# 16. Baseline

Representa o estado completo do catálogo.

```
Baseline

↓

TypeDefinitions

↓

AttributeDefinitions
```

É imutável.

---

# Rationale

Representa um snapshot estrutural completo.

Não apenas a versão de uma classe.

---

# 17. TypeDefinition

Cada tipo possui uma definição.

Contém

* nome
* atributos
* relacionamentos
* índices
* herança
* constraints

---

# 18. AttributeDefinition

Cada atributo possui

```
FieldId

Nome

Tipo

Nullable

Default

Collection

Embedded
```

---

# 19. Evolução de Schema

Nunca altera uma definição existente.

Sempre cria uma nova.

Antes

```
Employee

name

salary
```

Depois

```
Employee

name

salary

country
```

Nova TypeDefinition.

A antiga permanece.

---

# Rationale

Objetos antigos continuam válidos.

---

# 20. Problema da Evolução

Uma aplicação antiga pode desaparecer.

A classe C++ original também.

Logo o banco não pode depender da existência da classe para interpretar os dados.

---

# Solução

Separar

```
Classe C++

↓

Binding

↓

Representação Persistente
```

---

# 21. Codec Genérico

O banco possui apenas UM codec binário.

Ele interpreta qualquer objeto usando o catálogo.

```
Payload

↓

TypeDefinition

↓

AttributeDefinitions

↓

Objeto Persistente
```

O codec não conhece Employee.

Conhece apenas atributos persistentes.

---

# Rationale

Evita centenas de codecs históricos.

---

# 22. Binding

Cada tipo C++ atual possui um Binding.

Exemplo

```cpp
db.bind<Employee>()

.field<1>(&Employee::name)

.field<2>(&Employee::salary)

.field<3>(&Employee::department);
```

O Binding liga

```
FieldId

↓

Membro C++
```

---

# Rationale

Existe apenas um Binding para a versão atual da aplicação.

Não é necessário manter bindings para todas as versões históricas.

---

# 23. Projection Plan

Quando um objeto antigo é lido.

O banco compara

```
TypeDefinition Persistida

↓

Binding Atual
```

E gera automaticamente um plano.

Operações possíveis

```
Copy

Convert

Default

Ignore

ResolveReference
```

---

## Copy

Campo igual.

```
salary

↓

salary
```

---

## Convert

Mudança de tipo.

```
double

↓

int64
```

---

## Default

Campo novo.

```
country

↓

"BR"
```

---

## Ignore

Campo removido.

```
fax

↓

(descartado)
```

---

## ResolveReference

Converte ObjectId em Handle.

---

# Rationale

A maioria das mudanças de schema pode ser tratada automaticamente.

Sem necessidade de código de migração.

---

# 24. Cache de Projection Plans

O Projection Plan é calculado apenas uma vez.

```
TypeDefinitionId

+

Binding Atual

↓

ProjectionPlan
```

Depois permanece em cache.

---

# Rationale

Evita comparação de schemas a cada leitura.

---

# 25. Quando criar uma migração

Apenas quando a semântica mudar.

Exemplo

```
salary

↓

salary_cents
```

O desenvolvedor registra

```cpp
registerMigration(...)
```

Apenas para aquele caso.

---

# Rationale

Mudanças simples não exigem migração.

---

# 26. Materialização

Fluxo

```
Handle

↓

ObjectStore

↓

Payload

↓

Codec Genérico

↓

ProjectionPlan

↓

Classe Atual
```

---

# Rationale

Mesmo um objeto criado há 10 anos pode ser materializado pela aplicação atual.

---

# 27. Escrita

Fluxo

```
Classe Atual

↓

Binding

↓

Codec Genérico

↓

Payload

↓

ObjectStore
```

Todos os objetos novos passam a utilizar a TypeDefinition mais recente.

---

# 28. Migração Preguiçosa

Objetos antigos permanecem antigos.

Quando modificados.

↓

São gravados utilizando a definição atual.

---

# Rationale

Não existe necessidade de migrar milhões de objetos imediatamente.

---

# 29. Performance

O caminho crítico utiliza

* Binding
* ProjectionPlan cacheado
* Codec Genérico

Não existe interpretação dinâmica completa.

O caminho dinâmico permanece apenas como fallback para casos excepcionais.

---

# 30. Princípios Fundamentais

* Objetos são a unidade persistente.
* Objetos possuem identidade permanente.
* O Handle representa apenas identidade.
* Ponteiros nunca são persistidos.
* Relacionamentos utilizam ObjectId.
* Coleções possuem armazenamento próprio.
* Objetos grandes ficam na BlobStore.
* O catálogo também é composto por objetos.
* Baselines são imutáveis.
* TypeDefinitions nunca são alteradas.
* O banco possui um único codec binário genérico.
* Cada aplicação registra apenas um Binding para seus tipos atuais.
* Projection Plans são gerados automaticamente e cacheados.
* Migrações são necessárias apenas quando há mudança de semântica.
* Objetos antigos continuam legíveis indefinidamente.
* O formato persistente pertence ao banco, nunca ao layout das classes C++.
