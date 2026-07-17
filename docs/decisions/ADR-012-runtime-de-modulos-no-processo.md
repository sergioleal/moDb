# ADR-012 — Interface por métodos C++ e runtime de módulos no processo

- Estado: aceito para a Fase 9
- Data: 2026-07-17

## Contexto

O moDb é um banco orientado a objetos. Expor SQL como interface principal faria
a aplicação voltar a traduzir seu domínio para uma linguagem relacional, embora
o armazenamento, a identidade, os relacionamentos e o ciclo de vida dos dados
sejam nativamente orientados a objetos.

A aplicação precisa executar regras de domínio próximas dos dados, dentro da
mesma fronteira transacional, e receber apenas o resultado da operação. Ao mesmo
tempo, o motor continua precisando de consultas, índices, planejamento e
streaming para localizar e percorrer objetos com eficiência.

Também é necessário escolher o primeiro modelo de execução dos binários de
domínio. Isolá-los em workers oferece uma fronteira de falha mais forte, mas
introduz IPC, serialização adicional, supervisão e um protocolo interno antes de
o runtime básico estar validado.

## Decisão

**A interface de aplicação não será SQL-like.** A aplicação chama métodos C++
registrados por identificador e versão e recebe resultados tipados. Pela rede
trafegam somente o identificador do método, sua versão, argumentos serializados
e o resultado; nenhum código executável é enviado pelo cliente.

**O mecanismo de consulta permanece interno.** Operações de domínio podem
consultar, filtrar, percorrer índices e produzir streams por meio da API tipada
exposta pelo `ExecutionContext`. O motor de consulta é uma capacidade do banco,
mas não define sua interface pública nem expõe SQL.

**A primeira implementação executará os módulos dentro do processo do
servidor.** Os binários serão obtidos somente de uma origem confiável configurada
pelo operador e carregados pelo `ModuleLoader`. O MVP não terá sandbox nem
isolamento por processo.

Uma origem confiável é uma localização administrativa, não controlada pelo
cliente da aplicação, acompanhada por uma política explícita de admissão. Na
carga, o servidor valida ao menos:

- identificador e versão do módulo;
- versão da API do runtime;
- baseline de dados compatível;
- métodos exportados e seus modos (`read_only` ou `read_write`);
- hash do binário presente no manifesto ou em uma allowlist administrativa.

O `ExecutionContext` é a única porta de entrada do módulo no banco. Ele expõe
transação ou snapshot, acesso tipado a objetos, consultas internas, cancelamento
e logging. Não expõe `PageFile`, Buffer Pool, WAL, páginas, índices físicos nem
outros componentes internos.

Cada chamada constitui uma unidade de execução controlada pelo servidor:

- método `read_only` executa sob um `Snapshot`;
- método `read_write` executa sob uma `Transaction`;
- sucesso implica commit;
- `Result` de erro ou exceção capturada implica rollback;
- falha fatal do módulo pode encerrar o processo, que retorna por supervisor e
  executa WAL recovery.

Entradas e saídas não podem depender de ponteiros, referências, layout de
objetos em memória ou outros detalhes de ABI. Elas usam o codec versionado do
moDb. Essa fronteira será preservada mesmo dentro do processo para permitir que
uma versão futura mova a execução para workers isolados sem alterar o contrato
da aplicação.

## Consequências

- O moDb se posiciona como banco orientado a objetos e runtime transacional de
  domínio, não como um banco relacional com nova sintaxe.
- Consultas, índices e streaming continuam sendo componentes centrais do motor.
- A primeira implementação tem baixa latência e não exige IPC entre o método e
  o banco.
- Código nativo defeituoso pode corromper memória ou derrubar a instância; a
  origem confiável reduz exposição, mas não cria isolamento técnico.
- O processo deve operar sob supervisor, e o WAL continua sendo a fronteira de
  recuperação após crash.
- Atualização a quente, sandbox e workers isolados ficam fora do primeiro
  runtime e serão avaliados posteriormente.
- O registro por id/versão, o codec de argumentos/resultados e o manifesto são
  requisitos desde o início, pois formam a futura fronteira de isolamento.
