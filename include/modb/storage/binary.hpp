#pragma once

// Importa Result para informar leituras incompletas.
#include "modb/error.hpp"

// Disponibiliza std::byte e std::size_t.
#include <cstddef>
// Disponibiliza inteiros com largura fixa.
#include <cstdint>
// Disponibiliza visões de sequências de bytes.
#include <span>
// Disponibiliza std::move.
#include <utility>
// Disponibiliza o armazenamento dinâmico dos bytes escritos.
#include <vector>

namespace modb::storage {

// Constrói uma sequência binária usando inteiros little-endian.
class BinaryWriter {
public:
    // Acrescenta um inteiro de oito bits.
    void write_u8(std::uint8_t value);
    // Acrescenta um inteiro de dezesseis bits em little-endian.
    void write_u16(std::uint16_t value);
    // Acrescenta um inteiro de trinta e dois bits em little-endian.
    void write_u32(std::uint32_t value);
    // Acrescenta um inteiro de sessenta e quatro bits em little-endian.
    void write_u64(std::uint64_t value);
    // Acrescenta uma sequência de bytes sem modificá-la.
    void write_bytes(std::span<const std::byte> bytes);

    // Retorna uma visão somente leitura do conteúdo atual.
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }
    // Transfere o vetor completo para o chamador.
    [[nodiscard]] std::vector<std::byte> take() && noexcept { return std::move(bytes_); }

private:
    // Possui todos os bytes escritos na ordem de chamada.
    std::vector<std::byte> bytes_;
};

// Lê valores de uma sequência sem ultrapassar seus limites.
class BinaryReader {
public:
    // Guarda uma visão dos bytes; o chamador mantém a memória viva.
    explicit BinaryReader(std::span<const std::byte> bytes) noexcept : bytes_{bytes} {}

    // Lê um inteiro de oito bits.
    [[nodiscard]] Result<std::uint8_t> read_u8();
    // Lê um inteiro de dezesseis bits em little-endian.
    [[nodiscard]] Result<std::uint16_t> read_u16();
    // Lê um inteiro de trinta e dois bits em little-endian.
    [[nodiscard]] Result<std::uint32_t> read_u32();
    // Lê um inteiro de sessenta e quatro bits em little-endian.
    [[nodiscard]] Result<std::uint64_t> read_u64();
    // Lê exatamente a quantidade solicitada e avança a posição.
    [[nodiscard]] Result<std::span<const std::byte>> read_bytes(std::size_t size);

    // Informa quantos bytes ainda não foram consumidos.
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - position_; }
    // Informa se todos os bytes já foram consumidos.
    [[nodiscard]] bool at_end() const noexcept { return position_ == bytes_.size(); }

private:
    // Aponta para a sequência recebida sem possuir sua memória.
    std::span<const std::byte> bytes_;
    // Guarda o índice da próxima leitura.
    std::size_t position_{0};
};

} // namespace modb::storage

