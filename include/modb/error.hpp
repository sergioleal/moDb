#pragma once

// Disponibiliza std::expected, usado para retornar um valor ou um erro.
#include <expected>
// Disponibiliza std::string para armazenar mensagens de erro.
#include <string>

namespace modb {

// Identifica cada tipo de erro sem depender do texto da mensagem.
enum class ErrorCode {
    // O nome de uma tabela ou coluna é inválido.
    invalid_identifier,
    // Um argumento fornecido para uma operação é inválido.
    invalid_argument,
    // Foi criado um schema sem colunas.
    empty_schema,
    // Duas colunas possuem o mesmo nome.
    duplicate_column,
    // A coluna procurada não existe.
    column_not_found,
    // A quantidade de valores não corresponde à quantidade de colunas.
    value_count_mismatch,
    // O tipo de um valor não corresponde ao tipo da coluna.
    type_mismatch,
    // Uma coluna NOT NULL recebeu NULL.
    null_constraint_violation,
    // Já existe uma tabela com o mesmo nome.
    duplicate_table,
    // A tabela procurada não existe.
    table_not_found,
    // A criação não pode sobrescrever um arquivo existente.
    file_already_exists,
    // O arquivo solicitado não foi encontrado.
    file_not_found,
    // O sistema operacional informou uma falha de entrada ou saída.
    io_error,
    // O arquivo não possui a assinatura do moDb.
    invalid_file_format,
    // A versão do arquivo não é suportada.
    incompatible_format_version,
    // O arquivo está truncado ou possui metadados inconsistentes.
    corrupt_file,
    // O identificador aponta para uma página que não existe.
    page_not_found,
    // A operação tentou alterar diretamente uma página reservada.
    reserved_page,
    // Os bytes terminaram antes que o valor estivesse completo.
    unexpected_end_of_input,
    // Os bytes não representam uma codificação reconhecida.
    invalid_encoding,
    // Restaram bytes depois que o objeto completo foi decodificado.
    trailing_data,
    // O valor não cabe nos campos de tamanho do formato.
    value_too_large,
    // O schema ultrapassa o limite de colunas do produto.
    too_many_columns,
    // A página não contém a assinatura esperada para seu tipo.
    invalid_page_format,
    // A versão da estrutura interna da página não é suportada.
    incompatible_page_version,
    // Os offsets ou tamanhos internos da página são inconsistentes.
    corrupt_page,
    // A página não possui espaço livre suficiente para o registro.
    page_full,
    // O identificador aponta para um slot que não existe.
    slot_not_found,
    // O registro é maior que a capacidade de uma página vazia.
    record_too_large,
    // Uma cadeia de páginas aponta novamente para uma página já visitada.
    page_chain_cycle,
    // O RecordId não pertence ao heap consultado.
    record_not_found,

    // Modelo de objetos (ODB++, ver docs/decisions/ADR-00X e PROTOCOLO_FASES.md):
    // Duas colunas/atributos de um mesmo tipo usam o mesmo FieldId.
    duplicate_field,
    // O FieldId consultado não existe no tipo.
    field_not_found,
    // Já existe um tipo registrado com o mesmo nome.
    duplicate_type,
    // O tipo consultado não existe no registro.
    type_not_found,
    // Um ObjectId/TypeDefinitionId/BaselineId igual a zero foi usado onde um
    // identificador válido (não nulo) era exigido.
    invalid_object_id,
    // Um tipo C++ já possui outro binding ativo na instância.
    binding_mismatch,
    // Uma projeção não pôde reconciliar o tipo persistido com o binding atual
    // (conversão de tipo não permitida sem migração registrada).
    incompatible_projection,
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
