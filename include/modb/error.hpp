#pragma once

// Disponibiliza std::expected, usado para retornar um valor ou um erro.
#include <expected>
// Disponibiliza std::string para armazenar mensagens de erro.
#include <string>

namespace modb {

// Identifica cada tipo de erro sem depender do texto da mensagem.
// O número de cada código viaja no OpResult e é lido por clientes em outras
// linguagens (docs/COMPATIBILIDADE.md, "Códigos de erro"): cada código tem valor
// explícito; um código novo entra no fim, com o próximo número; nenhum é
// renumerado nem reusado. Ao acrescentar, atualize tests/error_code_test.cpp e o
// Apêndice A do docs/PROTOCOLO_CLIENTES.md (o teste modb.error_codes confere).
enum class ErrorCode {
    // O nome de uma tabela ou coluna é inválido.
    invalid_identifier = 0,
    // Um argumento fornecido para uma operação é inválido.
    invalid_argument = 1,
    // Foi criado um schema sem colunas.
    empty_schema = 2,
    // Duas colunas possuem o mesmo nome.
    duplicate_column = 3,
    // A coluna procurada não existe.
    column_not_found = 4,
    // A quantidade de valores não corresponde à quantidade de colunas.
    value_count_mismatch = 5,
    // O tipo de um valor não corresponde ao tipo da coluna.
    type_mismatch = 6,
    // Uma coluna NOT NULL recebeu NULL.
    null_constraint_violation = 7,
    // Já existe uma tabela com o mesmo nome.
    duplicate_table = 8,
    // A tabela procurada não existe.
    table_not_found = 9,
    // A criação não pode sobrescrever um arquivo existente.
    file_already_exists = 10,
    // O arquivo solicitado não foi encontrado.
    file_not_found = 11,
    // O sistema operacional informou uma falha de entrada ou saída.
    io_error = 12,
    // O arquivo não possui a assinatura do moDb.
    invalid_file_format = 13,
    // A versão do arquivo não é suportada.
    incompatible_format_version = 14,
    // O arquivo está truncado ou possui metadados inconsistentes.
    corrupt_file = 15,
    // O identificador aponta para uma página que não existe.
    page_not_found = 16,
    // A operação tentou alterar diretamente uma página reservada.
    reserved_page = 17,
    // Os bytes terminaram antes que o valor estivesse completo.
    unexpected_end_of_input = 18,
    // Os bytes não representam uma codificação reconhecida.
    invalid_encoding = 19,
    // Restaram bytes depois que o objeto completo foi decodificado.
    trailing_data = 20,
    // O valor não cabe nos campos de tamanho do formato.
    value_too_large = 21,
    // O schema ultrapassa o limite de colunas do produto.
    too_many_columns = 22,
    // A página não contém a assinatura esperada para seu tipo.
    invalid_page_format = 23,
    // A versão da estrutura interna da página não é suportada.
    incompatible_page_version = 24,
    // Os offsets ou tamanhos internos da página são inconsistentes.
    corrupt_page = 25,
    // A página não possui espaço livre suficiente para o registro.
    page_full = 26,
    // O identificador aponta para um slot que não existe.
    slot_not_found = 27,
    // O registro é maior que a capacidade de uma página vazia.
    record_too_large = 28,
    // Uma cadeia de páginas aponta novamente para uma página já visitada.
    page_chain_cycle = 29,
    // O RecordId não pertence ao heap consultado.
    record_not_found = 30,
    // Modelo de objetos (ODB++, ver docs/decisions/ADR-00X e PROTOCOLO_FASES.md):
    // Duas colunas/atributos de um mesmo tipo usam o mesmo FieldId.
    duplicate_field = 31,
    // O FieldId consultado não existe no tipo.
    field_not_found = 32,
    // Já existe um tipo registrado com o mesmo nome.
    duplicate_type = 33,
    // O tipo consultado não existe no registro.
    type_not_found = 34,
    // Um ObjectId/TypeDefinitionId/BaselineId igual a zero foi usado onde um
    // identificador válido (não nulo) era exigido.
    invalid_object_id = 35,
    // Um tipo C++ já possui outro binding ativo na instância.
    binding_mismatch = 36,
    // Uma projeção não pôde reconciliar o tipo persistido com o binding atual
    // (conversão de tipo não permitida sem migração registrada).
    incompatible_projection = 37,
    // Uma escrita foi tentada sem uma transação ativa (Fase 5).
    transaction_required = 38,
    // Uma segunda transação foi iniciada com uma já em andamento (single-writer).
    transaction_active = 39,
    // A transação já alcançou o ponto de commit durável e não pode mais reverter.
    transaction_committed = 40,
    // O commit está durável no WAL, mas a aplicação local falhou; reabra o banco
    // para a recuperação refazer as páginas pendentes.
    commit_recovery_required = 41,
    // Esta instância observou uma falha depois de um commit durável e só pode
    // voltar a ser usada após reabrir o banco.
    database_recovery_required = 42,
    // O WAL presente não pode ser interpretado com segurança; ele é preservado
    // para diagnóstico e a abertura do banco é interrompida.
    wal_corrupt = 43,
    // Uma segunda alteração do mesmo objeto foi tentada enquanto a versão
    // anterior ainda é visível a um snapshot aberto (Fase 6B: só há uma
    // posição `previous` por objeto — limitação documentada no ADR-009).
    snapshot_conflict = 44,
    // Frame de protocolo inválido, inconsistente ou hostil (Fase 8).
    protocol_error = 45,
    // length do frame excede o máximo negociado / 16 MiB (Fase 8).
    frame_too_large = 46,
    // A conexão de rede foi fechada pelo peer ou pelo transporte (Fase 8).
    connection_closed = 47,
    // Operação de domínio não registrada (Fase 9).
    operation_not_found = 48,
    // Manifesto/módulo incompatível com o runtime ou a allowlist (Fase 9).
    incompatible_module = 49,
    // Major de protocolo incompatível na negociação Hello (Fase 10E).
    incompatible_protocol_version = 50,
    // Facade ausente no catálogo (Fase 11).
    facade_not_found = 51,
    // Método invocado não pertence à facade do handle (Fase 11).
    facade_method_not_found = 52,
    // Versão de facade incompatível na negociação/lookup (Fase 11).
    incompatible_facade_version = 53,
    // Campo/membro não forma aresta tipada válida (Fase 12).
    invalid_edge = 54,
    // Alvo da aresta ausente sob o Snapshot (ref órfã; Fase 12).
    edge_target_not_found = 55,
    // Travessia excedeu profundidade/máximo de vértices (Fase 12).
    graph_limit_exceeded = 56,
    // Ciclo detectado onde a topologia não permite (Fase 12).
    graph_cycle = 57,
    // Escrita/begin/GC em follower read-only (Fase 14).
    replica_read_only = 58,
    // Pedido de LSN abaixo da retenção / gap no stream (Fase 14).
    replication_gap = 59,
    // timeline_id diverge entre primary e follower (Fase 14).
    timeline_mismatch = 60,
    // DatabaseUuid diverge entre primary e follower (Fase 14).
    database_uuid_mismatch = 61,
    // Follower precisa de novo bootstrap (Fase 14).
    bootstrap_required = 62,
    // Combinação inválida de papel/parâmetro de instância (Fase 15).
    invalid_instance_config = 63,
    // Operação exige arquivo de dados; primary está em wal_only (Fase 15).
    data_files_disabled = 64,
    // Commit wal_only exige réplica de dados e nenhuma está disponível (Fase 15).
    no_data_replica = 65,
    // Timeout aguardando ACK de réplica de dados (Fase 15).
    commit_await_replica_timeout = 66,
    // Transição ou estado de catch-up inválido para a réplica (Fase 16).
    invalid_replica_state = 67,
    // Falha ao baixar/spoolar segmentos WAL para catch-up (Fase 16).
    replica_download_failed = 68,
    // Manifesto ou segmento WAL não bate com o hash/tamanho declarado (Fase 16).
    manifest_hash_mismatch = 69,
    // A operação é válida, mas conflita com o estado atual (regra de negócio de
    // uma stored procedure: ex. exemplar já emprestado). Servidor de aplicação, S3.
    conflict = 70,
    // Falha interna de uma stored procedure (exceção): a transação foi desfeita.
    internal_error = 71,
    // A stored procedure passou do tempo máximo configurado: a transação foi desfeita.
    operation_timeout = 72,
    // O cliente não se autenticou, ou a credencial foi recusada (proxy, ADR-028).
    unauthenticated = 73,
    // O cliente autenticado não pode fazer o que pediu (política do proxy, ADR-028).
    permission_denied = 74,
};

// Reúne o código estável do erro e uma mensagem explicativa.
struct Error {
    // Permite que o programa trate o erro sem comparar textos.
    ErrorCode code;
    // Explica o erro para uma pessoa.
    std::string message;

    // Permite comparar dois erros em testes.
    friend bool operator==(const Error&, const Error&) = default;
};

// Result<T> contém um T quando há sucesso ou Error quando há falha.
template <typename T>
using Result = std::expected<T, Error>;

} // namespace modb
