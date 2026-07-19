# Fuzzing — Fase 10D

Tag: `0.0.10d`

Alvos em `tests/fuzz/` exercitam decoders sobre entrada não confiável. Cada
entrada é truncada a **1 MiB** antes de qualquer alocação no harness.

| Binário | Decoder |
|---|---|
| `fuzz_object_codec` | `decode_object` / header / payload |
| `fuzz_type_definition` | `decode_type_attributes` + `TypeDefinition::create` |
| `fuzz_blob_chain` | `parse_blob_page` (BLBP) |
| `fuzz_protocol` | `modb::net::decode_message` |
| `fuzz_wal` | `Wal::read_all` (arquivo temporário) |

Corpus de regressão: `tests/fuzz/corpus/<alvo>/`.

## Fallback (MinGW / sem libFuzzer)

No Windows com MinGW GCC não há ASan/UBSan/libFuzzer completos. O caminho
suportado é **replay do corpus** (e as suítes `debug` / `sanitizers`):

```powershell
cmake --preset debug
cmake --build --preset debug
ctest --preset debug -R modb.fuzz --output-on-failure

# ou manualmente:
.\build\debug\fuzz_object_codec.exe .\tests\fuzz\corpus\object_codec
```

O preset `fuzz` com MinGW **desliga** `MODB_ENABLE_LIBFUZZER` com warning e
mantém o mesmo replay.

## Campanha com Clang + libFuzzer

Quando Clang estiver disponível (Linux/macOS ou Clang no Windows):

```bash
cmake --preset fuzz   # ativa MODB_ENABLE_LIBFUZZER + sanitizers
cmake --build --preset fuzz

# 1 h por alvo (critério de aceite documentado):
./build/fuzz/fuzz_object_codec -max_total_time=3600 tests/fuzz/corpus/object_codec
./build/fuzz/fuzz_type_definition -max_total_time=3600 tests/fuzz/corpus/type_definition
./build/fuzz/fuzz_blob_chain -max_total_time=3600 tests/fuzz/corpus/blob_chain
./build/fuzz/fuzz_protocol -max_total_time=3600 tests/fuzz/corpus/protocol
./build/fuzz/fuzz_wal -max_total_time=3600 tests/fuzz/corpus/wal
```

Crash corrigido → adicionar o input ao corpus versionado e cobrir com
`modb.fuzz.*.corpus` no ctest.

## Limites nos decoders (esta entrega)

- `decode_object_payload`: `field_count > max_columns_per_table` → erro antes de alocar.
- `decode_type_attributes`: idem para contagem de atributos; nomes >
  `max_identifier_bytes` rejeitados sem `std::string` gigante.
- `parse_blob_page`: `length > blob_page_capacity` → erro (já existente, agora API pública).

## Aceite nesta tag (MinGW)

1. `ctest --preset debug` e `ctest --preset sanitizers` verdes (inclui corpus).
2. Demo: replay manual de cada alvo sobre o corpus.
3. Campanha 1 h/alvo com libFuzzer fica documentada acima para ambientes Clang;
   no MinGW o equivalente operacional é corpus + sanitizers/hardening.
