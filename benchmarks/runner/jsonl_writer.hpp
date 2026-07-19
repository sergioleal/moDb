#pragma once

#include "runner/sha256.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace modb::bench {

// Writer JSONL crash-safe: grava em `.partial` e promove ao nome final.
class JsonlWriter {
public:
    JsonlWriter() = default;
    JsonlWriter(const JsonlWriter&) = delete;
    JsonlWriter& operator=(const JsonlWriter&) = delete;
    ~JsonlWriter();

    // Abre o arquivo `.partial` no diretório. Falha se o destino final já existir.
    [[nodiscard]] bool open(const std::filesystem::path& final_path);

    // Acrescenta uma linha JSON completa (sem '\n' no argumento) e faz flush.
    [[nodiscard]] bool write_line(std::string_view json_object);

    // Finaliza: fecha, calcula hash do conteúdo (sem run_end), escreve run_end
    // se `append_run_end` for true via callback externo — aqui só promove.
    // O caller escreve run_end antes de chamar finish.
    [[nodiscard]] bool finish();

    // Abandona sem promover (mantém `.partial`).
    void abandon();

    [[nodiscard]] const std::filesystem::path& partial_path() const noexcept {
        return partial_path_;
    }
    [[nodiscard]] const std::filesystem::path& final_path() const noexcept {
        return final_path_;
    }
    [[nodiscard]] std::uint64_t sequence() const noexcept {
        return sequence_;
    }
    [[nodiscard]] std::uint64_t next_sequence() noexcept {
        return ++sequence_;
    }
    [[nodiscard]] std::size_t bytes_written() const noexcept {
        return bytes_written_;
    }
    // SHA-256 hex do conteúdo já gravado (linhas completas).
    [[nodiscard]] std::string content_sha256_hex() const;

private:
    std::filesystem::path final_path_;
    std::filesystem::path partial_path_;
    std::ofstream stream_;
    std::uint64_t sequence_{0};
    std::size_t bytes_written_{0};
    Sha256Digest running_{};
    bool hashing_{false};
    // Recomputamos do arquivo no finish; mantemos buffer acumulado para hash incremental simples.
    std::string content_buffer_;
    bool open_{false};
};

// Monta o nome canônico do relatório.
[[nodiscard]] std::filesystem::path make_result_filename(std::string_view utc_stamp,
                                                         std::string_view commit_short,
                                                         std::string_view host_token);

} // namespace modb::bench
