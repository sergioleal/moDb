# ADR-025 — Servidor de aplicação com procedures compiladas

- Estado: **aceito**
- Data: 2026-09-27
- Plano: [PLANO_SERVIDOR_PROCS.md](../../docs-process/PLANO_SERVIDOR_PROCS.md)
- Relacionados: [ADR-010](ADR-010-protocolo-binario-proximo-do-armazenamento.md) (protocolo),
  [ADR-011](ADR-011-concorrencia-do-servidor.md) (concorrência do servidor),
  [ADR-012](ADR-012-runtime-de-modulos-no-processo.md) (módulos de domínio)

## Contexto

A primeira aplicação real sobre o moDb (`biblioteca`, uma aplicação web)
embutia o motor no próprio processo. Isso junta a aplicação e o banco num só
executável: só esse processo pode abrir o arquivo, e qualquer outro cliente
precisa passar pela API que a aplicação decidir expor.

A decisão foi separar os dois: **o banco é um processo servidor, e as regras de
negócio rodam dentro dele como stored procedures em C++**; a aplicação vira
cliente e só chama procedures pelo nome.

O motor já tinha as peças da Fase 9 (ADR-012):
- `OpCall`/`OpResult` no protocolo;
- `OperationRegistry::dispatch`, que abre a transação, executa e faz commit, ou
  rollback em erro ou exceção;
- `ModuleLoader`, com manifesto e allowlist por hash.

Faltava um servidor que as usasse. O `modb serve` não registra operação
nenhuma, e cada exemplo montava o servidor à mão num `main`.

## Decisão

1. **Procs compiladas junto com o servidor.** Cada aplicação gera o próprio executável de servidor (`<app>-server`) com os seus módulos linkados. Não há carga de `.dll`/`.so` em tempo de execução.
2. **`modb::server_host`** monta o servidor a partir de uma lista de `modb::server::Module`:
   - cada módulo informa id, versão, `prepare` (tipos e índices, rodando na abertura sem transação), `register_procs` e os métodos exportados;
   - o host abre o banco e roda os `prepare`;
   - monta o manifesto de cada módulo com a baseline que o `prepare` acabou de produzir e admite o hash dele: o módulo veio no próprio binário, e a allowlist do ADR-012 existe para módulos de outra origem;
   - carrega o módulo pelo `ModuleLoader`, que confere métodos contra o registro;
   - serve com `serve_forever`, parando limpo em SIGINT/SIGTERM.
   - Imprime `READY <porta>` quando aceita conexões.
3. **`modb_add_server(<alvo> MODULES <lib>...)`** (`cmake/ModbServer.cmake`, instalado com o pacote) gera o `main`. Cada `<lib>` define `modb::server::Module modb_module_<lib>()`.
4. **Isolamento:**
   - a aplicação fica isolada do banco: outro processo, e outra máquina se quiser;
   - as procs rodam **no processo do banco, sem sandbox**. Um erro ou exceção desfaz só a transação da chamada. Um crash de verdade derruba o servidor; o supervisor reinicia, e o WAL garante o que foi confirmado. Um teste mata o processo com `TerminateProcess`/`SIGKILL` e confere isso;
   - escolhemos isso em vez de um processo separado para as procs porque evita um IPC em cada acesso da proc ao banco.
5. **Um banco por servidor**, como no ADR-011: uma instância de servidor por aplicação.

## Fora do escopo

- Carga de módulos em tempo de execução.
- Sandbox de procs.
- Vários bancos por servidor.
- Várias transações de escrita simultâneas.

Cada um vira uma decisão própria, se aparecer necessidade.

## Consequências

- Uma aplicação servidor é um `main` gerado mais os módulos; o exemplo `examples/server_procs` (`notas-server`) é o modelo, e `tests/server_host_test.cpp` o sobe como processo à parte.
- Atualizar uma regra é recompilar e trocar o executável do servidor. O arquivo do banco continua o mesmo; mudanças de esquema seguem o catálogo e a baseline.
- Encontrado ao implementar: o construtor de movimento de `net::Server` não levava o registro de procs, o catálogo de facades nem os limites configurados. Um servidor configurado e devolvido por valor respondia "server has no operation registry". Corrigido no mesmo commit.
- Pendente, no plano:
  - argumentos e resultados autodescritos (S2);
  - procs declaradas como função, com códigos de erro de regra (S3);
  - consulta, índice e coleções dentro das procs (S4);
  - operação como serviço (S5).
