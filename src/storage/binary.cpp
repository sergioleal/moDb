// Importa BinaryWriter e BinaryReader.
#include "modb/storage/binary.hpp"

// Disponibiliza std::to_integer.
#include <cstddef>
// Disponibiliza std::to_string para mensagens de erro.
#include <string>

namespace modb::storage {
namespace {

// Cria o erro usado quando não existem bytes suficientes.
Error incomplete_input(std::size_t requested, std::size_t available) {
    // Inclui as quantidades para facilitar o diagnóstico.
    return Error{
        ErrorCode::unexpected_end_of_input,
        "binary input requires " + std::to_string(requested) + " bytes but only " +
            std::to_string(available) + " remain",
    };
}

} // namespace

// Acrescenta um único byte ao vetor.
void BinaryWriter::write_u8(std::uint8_t value) {
    // Converte explicitamente o inteiro para std::byte.
    bytes_.push_back(std::byte{value});
}

// Escreve os dois bytes do inteiro do menos significativo para o mais significativo.
void BinaryWriter::write_u16(std::uint16_t value) {
    // Repete uma vez para cada byte do tipo.
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        // Mantém somente o byte menos significativo atual.
        write_u8(static_cast<std::uint8_t>(value & 0xffU));
        // Prepara o próximo byte.
        value = static_cast<std::uint16_t>(value >> 8U);
    }
}

// Escreve os quatro bytes do inteiro em little-endian.
void BinaryWriter::write_u32(std::uint32_t value) {
    // Repete uma vez para cada byte do tipo.
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        // Mantém somente o byte menos significativo atual.
        write_u8(static_cast<std::uint8_t>(value & 0xffU));
        // Prepara o próximo byte.
        value >>= 8U;
    }
}

// Escreve os oito bytes do inteiro em little-endian.
void BinaryWriter::write_u64(std::uint64_t value) {
    // Repete uma vez para cada byte do tipo.
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        // Mantém somente o byte menos significativo atual.
        write_u8(static_cast<std::uint8_t>(value & 0xffU));
        // Prepara o próximo byte.
        value >>= 8U;
    }
}

// Copia uma sequência de bytes para o final do vetor.
void BinaryWriter::write_bytes(std::span<const std::byte> bytes) {
    // insert preserva a ordem do primeiro ao último byte.
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
}

// Lê um único byte.
Result<std::uint8_t> BinaryReader::read_u8() {
    // Reutiliza a verificação de limites de read_bytes.
    auto data = read_bytes(1);
    // Propaga o erro quando não existe um byte disponível.
    if (!data) {
        return std::unexpected(data.error());
    }
    // Converte o byte lido para um inteiro sem sinal.
    return std::to_integer<std::uint8_t>((*data)[0]);
}

// Lê e reconstrói um inteiro de dezesseis bits.
Result<std::uint16_t> BinaryReader::read_u16() {
    // Lê exatamente os dois bytes necessários.
    auto data = read_bytes(sizeof(std::uint16_t));
    // Propaga uma entrada truncada.
    if (!data) {
        return std::unexpected(data.error());
    }
    // Começa com todos os bits desligados.
    std::uint16_t value = 0;
    // Recoloca cada byte em sua posição original.
    for (std::size_t index = 0; index < data->size(); ++index) {
        const auto shifted = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(std::to_integer<std::uint8_t>((*data)[index]))
            << (index * 8U));
        value = static_cast<std::uint16_t>(value | shifted);
    }
    // Retorna o inteiro reconstruído.
    return value;
}

// Lê e reconstrói um inteiro de trinta e dois bits.
Result<std::uint32_t> BinaryReader::read_u32() {
    // Lê exatamente os quatro bytes necessários.
    auto data = read_bytes(sizeof(std::uint32_t));
    // Propaga uma entrada truncada.
    if (!data) {
        return std::unexpected(data.error());
    }
    // Começa com todos os bits desligados.
    std::uint32_t value = 0;
    // Recoloca cada byte em sua posição original.
    for (std::size_t index = 0; index < data->size(); ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>((*data)[index]))
                 << (index * 8U);
    }
    // Retorna o inteiro reconstruído.
    return value;
}

// Lê e reconstrói um inteiro de sessenta e quatro bits.
Result<std::uint64_t> BinaryReader::read_u64() {
    // Lê exatamente os oito bytes necessários.
    auto data = read_bytes(sizeof(std::uint64_t));
    // Propaga uma entrada truncada.
    if (!data) {
        return std::unexpected(data.error());
    }
    // Começa com todos os bits desligados.
    std::uint64_t value = 0;
    // Recoloca cada byte em sua posição original.
    for (std::size_t index = 0; index < data->size(); ++index) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>((*data)[index]))
                 << (index * 8U);
    }
    // Retorna o inteiro reconstruído.
    return value;
}

// Entrega uma parte da entrada e avança a posição atual.
Result<std::span<const std::byte>> BinaryReader::read_bytes(std::size_t size) {
    // A subtração evita overflow que poderia ocorrer em position_ + size.
    if (size > remaining()) {
        return std::unexpected(incomplete_input(size, remaining()));
    }
    // Cria uma visão que começa na posição atual e possui o tamanho pedido.
    const auto result = bytes_.subspan(position_, size);
    // Avança a próxima leitura para depois da parte entregue.
    position_ += size;
    // Retorna a visão sem copiar os bytes.
    return result;
}

} // namespace modb::storage
