#pragma once

// Importa Result para reportar erros de I/O sem lançar exceções.
#include "modb/error.hpp"

// Disponibiliza std::byte e std::size_t.
#include <cstddef>
// Disponibiliza inteiros de largura fixa para offsets.
#include <cstdint>
// Disponibiliza caminhos portáveis.
#include <filesystem>
// Disponibiliza a visão contígua de bytes usada nas leituras e escritas.
#include <span>

namespace modb::storage {

// RAII sobre um descritor de arquivo nativo do sistema operacional.
//
// Oferece I/O posicional (independente de um cursor compartilhado) e, o ponto
// central, sincronização real com o dispositivo — FlushFileBuffers no Windows,
// fsync no POSIX — que o std::fstream não expõe de forma portável. Todo o código
// específico de plataforma fica isolado nesta classe; o restante do storage vê
// apenas Result e spans.
class NativeFile {
public:
    // Como o arquivo deve ser aberto.
    enum class Mode {
        // Cria um arquivo novo e falha se o caminho já existir.
        create_new,
        // Abre um arquivo existente para leitura e escrita.
        open_existing,
    };

    // Um objeto recém-construído não possui descritor aberto.
    NativeFile() noexcept = default;
    // Dois objetos nunca compartilham o mesmo descritor.
    NativeFile(const NativeFile&) = delete;
    NativeFile& operator=(const NativeFile&) = delete;
    // A propriedade do descritor pode ser transferida.
    NativeFile(NativeFile&& other) noexcept;
    NativeFile& operator=(NativeFile&& other) noexcept;
    // Fecha o descritor automaticamente, ignorando erros do fechamento.
    ~NativeFile();

    // Abre o arquivo no modo pedido, retornando erro rico em caso de falha.
    [[nodiscard]] static Result<NativeFile> open(const std::filesystem::path& path, Mode mode);

    // Lê exatamente destination.size() bytes a partir de offset.
    // Uma leitura curta indica arquivo truncado e vira erro.
    [[nodiscard]] Result<void> read_at(std::uint64_t offset, std::span<std::byte> destination);
    // Escreve exatamente source.size() bytes a partir de offset, estendendo o
    // arquivo quando necessário.
    [[nodiscard]] Result<void> write_at(std::uint64_t offset, std::span<const std::byte> source);
    // Força a persistência no dispositivo de todas as escritas já aceitas.
    [[nodiscard]] Result<void> sync();
    // Fecha explicitamente reportando erros; o destrutor apenas os ignora.
    [[nodiscard]] Result<void> close();

    // Informa se há um descritor aberto.
    [[nodiscard]] bool is_open() const noexcept;

private:
#ifdef _WIN32
    // HANDLE do Win32; nullptr representa a ausência de descritor aberto.
    void* handle_ = nullptr;
#else
    // Descritor POSIX; -1 representa a ausência de descritor aberto.
    int fd_ = -1;
#endif
};

} // namespace modb::storage
