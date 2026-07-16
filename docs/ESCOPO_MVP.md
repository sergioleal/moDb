# Escopo do MVP

## Objetivo

O MVP prova que o moDb consegue criar uma tabela, persistir linhas e recuperar
os mesmos dados depois de fechar e reabrir o arquivo.

## Funcionalidades incluídas

- biblioteca C++ e CLI local;
- um arquivo de banco por instância;
- acesso por um processo e uma thread;
- tipos `NULL`, `BOOLEAN`, `INTEGER`, `REAL` e `TEXT`;
- criação de tabelas;
- inserção e varredura sequencial de linhas;
- SQL mínimo com `CREATE TABLE`, `INSERT` e `SELECT`;
- detecção de versão inválida, truncamento e corrupção estrutural básica;
- testes de persistência e reabertura.

## Limites iniciais

- identificadores: até 63 bytes em UTF-8;
- colunas por tabela: até 256;
- uma linha codificada deve caber na área útil de uma página;
- textos maiores que a capacidade da linha não são aceitos no MVP;
- páginas de 4096 bytes;
- nenhuma conversão implícita que possa perder dados.

Os limites serão centralizados no código e validados antes de qualquer escrita.

## Funcionalidades excluídas

- múltiplos processos ou threads concorrentes;
- índices, joins, agregações e subconsultas;
- transações e recuperação por WAL;
- protocolo de rede, replicação e execução distribuída;
- alteração de schema;
- linhas ou objetos grandes distribuídos por várias páginas.

## Critério de conclusão

Este cenário deve passar em um teste automatizado:

1. criar um arquivo de banco;
2. criar a tabela `users(id INTEGER, name TEXT)`;
3. inserir `(1, 'Ana')`;
4. fechar completamente a instância;
5. abrir novamente o mesmo arquivo;
6. consultar `users` e recuperar exatamente `(1, 'Ana')`.

