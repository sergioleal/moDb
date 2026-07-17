#pragma once

// Importa o BlobStore, armazenamento próprio de toda coleção (doc §12).
#include "modb/object/blob_store.hpp"
// Importa os conversores de tipo C++ ↔ AttributeValue.
#include "modb/object/binding.hpp"
// Importa Result e os códigos de erro.
#include "modb/error.hpp"
// Importa o codec de valor único (encoding canônico dos elementos).
#include "modb/object/object_codec.hpp"
// Importa BinaryWriter/BinaryReader do formato do blob.
#include "modb/storage/binary.hpp"

// Disponibiliza std::lexicographical_compare para ordenar por encoding.
#include <algorithm>
// Disponibiliza std::byte.
#include <cstddef>
// Disponibiliza inteiros de largura fixa.
#include <cstdint>
// Disponibiliza o callback de iteração.
#include <functional>
// Disponibiliza std::optional no get do mapa.
#include <optional>
// Disponibiliza as visões de bytes.
#include <span>
// Disponibiliza std::move.
#include <utility>
// Disponibiliza os buffers das coleções.
#include <vector>

namespace modb::object {

// Placeholder da Fase 5: as coleções aceitam uma Transaction em toda escrita.
class Transaction;

// Cabeçalho comum das coleções: as primeiras 4 bytes do blob guardam a
// contagem de elementos. Helpers não-template ficam em collection.cpp.
[[nodiscard]] Result<std::uint32_t> collection_count(std::span<const std::byte> raw);
// Compara dois elementos já codificados, lexicograficamente por byte sem sinal.
[[nodiscard]] bool encoded_less(std::span<const std::byte> a, std::span<const std::byte> b);

// Codifica um único elemento no encoding canônico (tag + valor, ADR-003).
template <typename T>
[[nodiscard]] Result<std::vector<std::byte>> encode_element(const T& value) {
    storage::BinaryWriter writer;
    if (auto encoded = encode_value(writer, to_attribute_value<T>(value)); !encoded) {
        return std::unexpected(encoded.error());
    }
    return std::move(writer).take();
}

// Decodifica um único elemento a partir da posição corrente do reader.
template <typename T>
[[nodiscard]] Result<T> decode_element(storage::BinaryReader& reader) {
    auto value = decode_value(reader);
    if (!value) {
        return std::unexpected(value.error());
    }
    return from_attribute_value<T>(*value);
}

// Vetor persistente de T. O objeto pai guarda apenas o BlobId; os elementos
// vivem numa cadeia de páginas BLBP no formato `count u32 | elemento...`.
//
// Limitação de MVP: push_back reescreve o blob inteiro (O(n) por inserção).
// O append incremental fica para a Fase 10, com medição (tarefa 10.1).
template <typename T>
class PersistentVector {
public:
    // Abre um vetor existente por seu BlobId.
    PersistentVector(BlobStore& blobs, BlobId id) noexcept : blobs_{&blobs}, id_{id} {}

    // Cria o blob de respaldo vazio (count = 0) e devolve o vetor pronto.
    [[nodiscard]] static Result<PersistentVector> create(BlobStore& blobs) {
        storage::BinaryWriter writer;
        writer.write_u32(0);
        auto id = blobs.create(std::move(writer).take());
        if (!id) {
            return std::unexpected(id.error());
        }
        return PersistentVector{blobs, *id};
    }

    // BlobId estável a ser guardado no objeto pai.
    [[nodiscard]] BlobId id() const noexcept { return id_; }

    [[nodiscard]] Result<std::size_t> size() const {
        auto raw = blobs_->read(id_);
        if (!raw) {
            return std::unexpected(raw.error());
        }
        auto count = collection_count(*raw);
        if (!count) {
            return std::unexpected(count.error());
        }
        return static_cast<std::size_t>(*count);
    }

    [[nodiscard]] Result<T> at(std::size_t index) const {
        auto raw = blobs_->read(id_);
        if (!raw) {
            return std::unexpected(raw.error());
        }
        storage::BinaryReader reader{*raw};
        auto count = reader.read_u32();
        if (!count) {
            return std::unexpected(count.error());
        }
        if (index >= *count) {
            return std::unexpected(
                Error{ErrorCode::invalid_argument, "vector index is out of range"});
        }
        for (std::size_t skipped = 0; skipped < index; ++skipped) {
            if (auto value = decode_value(reader); !value) {
                return std::unexpected(value.error());
            }
        }
        return decode_element<T>(reader);
    }

    [[nodiscard]] Result<void> push_back(Transaction& /*tx*/, const T& value) {
        auto raw = blobs_->read(id_);
        if (!raw) {
            return std::unexpected(raw.error());
        }
        auto count = collection_count(*raw);
        if (!count) {
            return std::unexpected(count.error());
        }
        auto element = encode_element<T>(value);
        if (!element) {
            return std::unexpected(element.error());
        }
        storage::BinaryWriter writer;
        writer.write_u32(*count + 1);
        // Reaproveita os elementos existentes sem decodificá-los (após o u32).
        writer.write_bytes(std::span<const std::byte>{*raw}.subspan(sizeof(std::uint32_t)));
        writer.write_bytes(*element);
        auto rewritten = blobs_->rewrite(id_, std::move(writer).take());
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        id_ = *rewritten;
        return {};
    }

    // Percorre os elementos sem construir um vetor com todos de uma vez.
    [[nodiscard]] Result<void> for_each(
        const std::function<Result<void>(const T&)>& visitor) const {
        auto raw = blobs_->read(id_);
        if (!raw) {
            return std::unexpected(raw.error());
        }
        storage::BinaryReader reader{*raw};
        auto count = reader.read_u32();
        if (!count) {
            return std::unexpected(count.error());
        }
        for (std::uint32_t index = 0; index < *count; ++index) {
            auto element = decode_element<T>(reader);
            if (!element) {
                return std::unexpected(element.error());
            }
            if (auto result = visitor(*element); !result) {
                return std::unexpected(result.error());
            }
        }
        return {};
    }

private:
    BlobStore* blobs_;
    BlobId id_;
};

// Conjunto persistente de T: elementos únicos mantidos ordenados pela
// codificação canônica (busca binária na leitura). Escrita O(n) (MVP).
template <typename T>
class PersistentSet {
public:
    PersistentSet(BlobStore& blobs, BlobId id) noexcept : blobs_{&blobs}, id_{id} {}

    [[nodiscard]] static Result<PersistentSet> create(BlobStore& blobs) {
        storage::BinaryWriter writer;
        writer.write_u32(0);
        auto id = blobs.create(std::move(writer).take());
        if (!id) {
            return std::unexpected(id.error());
        }
        return PersistentSet{blobs, *id};
    }

    [[nodiscard]] BlobId id() const noexcept { return id_; }

    [[nodiscard]] Result<std::size_t> size() const {
        auto raw = blobs_->read(id_);
        if (!raw) {
            return std::unexpected(raw.error());
        }
        auto count = collection_count(*raw);
        if (!count) {
            return std::unexpected(count.error());
        }
        return static_cast<std::size_t>(*count);
    }

    // Insere mantendo a ordem; duplicatas (mesma codificação) são ignoradas.
    [[nodiscard]] Result<void> insert(Transaction& /*tx*/, const T& value) {
        auto raw = blobs_->read(id_);
        if (!raw) {
            return std::unexpected(raw.error());
        }
        auto element = encode_element<T>(value);
        if (!element) {
            return std::unexpected(element.error());
        }
        auto ranges = element_ranges(*raw);
        if (!ranges) {
            return std::unexpected(ranges.error());
        }
        const std::span<const std::byte> raw_view{*raw};
        std::size_t position = ranges->size();
        for (std::size_t index = 0; index < ranges->size(); ++index) {
            const auto span = subspan(raw_view, (*ranges)[index]);
            if (!encoded_less(span, *element)) {
                // Já presente: nada a fazer (dedup).
                if (!encoded_less(*element, span)) {
                    return {};
                }
                position = index;
                break;
            }
        }
        return rebuild_inserting(raw_view, *ranges, position, *element);
    }

    [[nodiscard]] Result<bool> contains(const T& value) const {
        auto raw = blobs_->read(id_);
        if (!raw) {
            return std::unexpected(raw.error());
        }
        auto element = encode_element<T>(value);
        if (!element) {
            return std::unexpected(element.error());
        }
        auto ranges = element_ranges(*raw);
        if (!ranges) {
            return std::unexpected(ranges.error());
        }
        const std::span<const std::byte> raw_view{*raw};
        std::size_t low = 0;
        std::size_t high = ranges->size();
        while (low < high) {
            const std::size_t mid = low + (high - low) / 2;
            const auto span = subspan(raw_view, (*ranges)[mid]);
            if (encoded_less(span, *element)) {
                low = mid + 1;
            } else if (encoded_less(*element, span)) {
                high = mid;
            } else {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] Result<void> for_each(
        const std::function<Result<void>(const T&)>& visitor) const {
        auto raw = blobs_->read(id_);
        if (!raw) {
            return std::unexpected(raw.error());
        }
        storage::BinaryReader reader{*raw};
        auto count = reader.read_u32();
        if (!count) {
            return std::unexpected(count.error());
        }
        for (std::uint32_t index = 0; index < *count; ++index) {
            auto element = decode_element<T>(reader);
            if (!element) {
                return std::unexpected(element.error());
            }
            if (auto result = visitor(*element); !result) {
                return std::unexpected(result.error());
            }
        }
        return {};
    }

private:
    // Faixa [start, end) de um elemento dentro do blob cru.
    struct Range {
        std::size_t start;
        std::size_t end;
    };

    static std::span<const std::byte> subspan(std::span<const std::byte> raw, Range range) {
        return raw.subspan(range.start, range.end - range.start);
    }

    // Localiza a faixa de bytes de cada elemento (após o u32 de contagem).
    static Result<std::vector<Range>> element_ranges(std::span<const std::byte> raw) {
        storage::BinaryReader reader{raw};
        auto count = reader.read_u32();
        if (!count) {
            return std::unexpected(count.error());
        }
        std::vector<Range> ranges;
        ranges.reserve(*count);
        for (std::uint32_t index = 0; index < *count; ++index) {
            const std::size_t start = raw.size() - reader.remaining();
            if (auto value = decode_value(reader); !value) {
                return std::unexpected(value.error());
            }
            const std::size_t end = raw.size() - reader.remaining();
            ranges.push_back(Range{start, end});
        }
        return ranges;
    }

    Result<void> rebuild_inserting(std::span<const std::byte> raw,
                                   const std::vector<Range>& ranges, std::size_t position,
                                   std::span<const std::byte> element) {
        storage::BinaryWriter writer;
        writer.write_u32(static_cast<std::uint32_t>(ranges.size() + 1));
        for (std::size_t index = 0; index < ranges.size(); ++index) {
            if (index == position) {
                writer.write_bytes(element);
            }
            writer.write_bytes(subspan(raw, ranges[index]));
        }
        if (position >= ranges.size()) {
            writer.write_bytes(element);
        }
        auto rewritten = blobs_->rewrite(id_, std::move(writer).take());
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        id_ = *rewritten;
        return {};
    }

    BlobStore* blobs_;
    BlobId id_;
};

// Mapa persistente K→V: entradas ordenadas pela codificação canônica da chave;
// chaves únicas; busca binária na leitura. Escrita O(n) (MVP).
template <typename K, typename V>
class PersistentMap {
public:
    PersistentMap(BlobStore& blobs, BlobId id) noexcept : blobs_{&blobs}, id_{id} {}

    [[nodiscard]] static Result<PersistentMap> create(BlobStore& blobs) {
        storage::BinaryWriter writer;
        writer.write_u32(0);
        auto id = blobs.create(std::move(writer).take());
        if (!id) {
            return std::unexpected(id.error());
        }
        return PersistentMap{blobs, *id};
    }

    [[nodiscard]] BlobId id() const noexcept { return id_; }

    [[nodiscard]] Result<std::size_t> size() const {
        auto raw = blobs_->read(id_);
        if (!raw) {
            return std::unexpected(raw.error());
        }
        auto count = collection_count(*raw);
        if (!count) {
            return std::unexpected(count.error());
        }
        return static_cast<std::size_t>(*count);
    }

    // Insere ou substitui o valor associado à chave.
    [[nodiscard]] Result<void> put(Transaction& /*tx*/, const K& key, const V& value) {
        auto raw = blobs_->read(id_);
        if (!raw) {
            return std::unexpected(raw.error());
        }
        auto key_bytes = encode_element<K>(key);
        if (!key_bytes) {
            return std::unexpected(key_bytes.error());
        }
        auto value_bytes = encode_element<V>(value);
        if (!value_bytes) {
            return std::unexpected(value_bytes.error());
        }
        auto entries = entry_ranges(*raw);
        if (!entries) {
            return std::unexpected(entries.error());
        }
        const std::span<const std::byte> raw_view{*raw};
        std::size_t position = entries->size();
        bool replace = false;
        for (std::size_t index = 0; index < entries->size(); ++index) {
            const auto key_span = subspan(raw_view, (*entries)[index].key);
            if (!encoded_less(key_span, *key_bytes)) {
                position = index;
                replace = !encoded_less(*key_bytes, key_span);
                break;
            }
        }
        return rebuild(raw_view, *entries, position, replace, *key_bytes, *value_bytes);
    }

    [[nodiscard]] Result<std::optional<V>> get(const K& key) const {
        auto raw = blobs_->read(id_);
        if (!raw) {
            return std::unexpected(raw.error());
        }
        auto key_bytes = encode_element<K>(key);
        if (!key_bytes) {
            return std::unexpected(key_bytes.error());
        }
        auto entries = entry_ranges(*raw);
        if (!entries) {
            return std::unexpected(entries.error());
        }
        const std::span<const std::byte> raw_view{*raw};
        std::size_t low = 0;
        std::size_t high = entries->size();
        while (low < high) {
            const std::size_t mid = low + (high - low) / 2;
            const auto key_span = subspan(raw_view, (*entries)[mid].key);
            if (encoded_less(key_span, *key_bytes)) {
                low = mid + 1;
            } else if (encoded_less(*key_bytes, key_span)) {
                high = mid;
            } else {
                storage::BinaryReader reader{subspan(raw_view, (*entries)[mid].value)};
                auto decoded = decode_element<V>(reader);
                if (!decoded) {
                    return std::unexpected(decoded.error());
                }
                return std::optional<V>{std::move(*decoded)};
            }
        }
        return std::optional<V>{};
    }

    [[nodiscard]] Result<bool> remove(Transaction& /*tx*/, const K& key) {
        auto raw = blobs_->read(id_);
        if (!raw) {
            return std::unexpected(raw.error());
        }
        auto key_bytes = encode_element<K>(key);
        if (!key_bytes) {
            return std::unexpected(key_bytes.error());
        }
        auto entries = entry_ranges(*raw);
        if (!entries) {
            return std::unexpected(entries.error());
        }
        const std::span<const std::byte> raw_view{*raw};
        std::size_t position = entries->size();
        for (std::size_t index = 0; index < entries->size(); ++index) {
            const auto key_span = subspan(raw_view, (*entries)[index].key);
            if (!encoded_less(key_span, *key_bytes) && !encoded_less(*key_bytes, key_span)) {
                position = index;
                break;
            }
        }
        if (position >= entries->size()) {
            return false;
        }
        storage::BinaryWriter writer;
        writer.write_u32(static_cast<std::uint32_t>(entries->size() - 1));
        for (std::size_t index = 0; index < entries->size(); ++index) {
            if (index == position) {
                continue;
            }
            writer.write_bytes(subspan(raw_view, (*entries)[index].key));
            writer.write_bytes(subspan(raw_view, (*entries)[index].value));
        }
        auto rewritten = blobs_->rewrite(id_, std::move(writer).take());
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        id_ = *rewritten;
        return true;
    }

private:
    struct Range {
        std::size_t start;
        std::size_t end;
    };
    struct Entry {
        Range key;
        Range value;
    };

    static std::span<const std::byte> subspan(std::span<const std::byte> raw, Range range) {
        return raw.subspan(range.start, range.end - range.start);
    }

    static Result<std::vector<Entry>> entry_ranges(std::span<const std::byte> raw) {
        storage::BinaryReader reader{raw};
        auto count = reader.read_u32();
        if (!count) {
            return std::unexpected(count.error());
        }
        std::vector<Entry> entries;
        entries.reserve(*count);
        for (std::uint32_t index = 0; index < *count; ++index) {
            const std::size_t key_start = raw.size() - reader.remaining();
            if (auto key = decode_value(reader); !key) {
                return std::unexpected(key.error());
            }
            const std::size_t key_end = raw.size() - reader.remaining();
            if (auto value = decode_value(reader); !value) {
                return std::unexpected(value.error());
            }
            const std::size_t value_end = raw.size() - reader.remaining();
            entries.push_back(Entry{Range{key_start, key_end}, Range{key_end, value_end}});
        }
        return entries;
    }

    Result<void> rebuild(std::span<const std::byte> raw, const std::vector<Entry>& entries,
                         std::size_t position, bool replace, std::span<const std::byte> key_bytes,
                         std::span<const std::byte> value_bytes) {
        const std::size_t new_count = replace ? entries.size() : entries.size() + 1;
        storage::BinaryWriter writer;
        writer.write_u32(static_cast<std::uint32_t>(new_count));
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (index == position) {
                writer.write_bytes(key_bytes);
                writer.write_bytes(value_bytes);
                if (replace) {
                    continue; // substitui a entrada existente
                }
            }
            writer.write_bytes(subspan(raw, entries[index].key));
            writer.write_bytes(subspan(raw, entries[index].value));
        }
        if (position >= entries.size()) {
            writer.write_bytes(key_bytes);
            writer.write_bytes(value_bytes);
        }
        auto rewritten = blobs_->rewrite(id_, std::move(writer).take());
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        id_ = *rewritten;
        return {};
    }

    BlobStore* blobs_;
    BlobId id_;
};

} // namespace modb::object
