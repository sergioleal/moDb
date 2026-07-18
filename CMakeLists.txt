cmake_minimum_required(VERSION 3.30)

project(
    moDb
    VERSION 0.1.0
    DESCRIPTION "An embedded relational database written in C++26"
    LANGUAGES CXX
)

include(CTest)

option(MODB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(MODB_ENABLE_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)
option(MODB_ALLOW_CXX23_FALLBACK "Allow a local C++23 build when C++26 is unavailable" ON)
# Link estático do runtime MinGW gera binários portáteis (sem DLLs), mas a
# toolchain do CLion migrou de GCC 13 para GCC 15.2, cujo libwinpthread.a
# estático deixa `__intrinsic_setjmpex`/`__ms_vsnprintf` sem definição no
# link `-static`. Por isso o padrão é OFF (link dinâmico, que resolve as DLLs
# pelo PATH). Reative com -DMODB_STATIC_MINGW_RUNTIME=ON numa toolchain onde o
# link totalmente estático funcione (ex.: GCC 13).
option(MODB_STATIC_MINGW_RUNTIME "Link the MinGW runtime statically into executables" OFF)
set(MODB_PAGE_SIZE 4096 CACHE STRING "Database page size in bytes")
set(MODB_CHURN_RECORD_TOTAL 10000 CACHE STRING "Record count used by churn tests")

# Opções de link do runtime MinGW, vazias quando o link estático está desligado.
if(MINGW AND MODB_STATIC_MINGW_RUNTIME)
    set(MODB_MINGW_LINK_OPTS ${MODB_MINGW_LINK_OPTS})
else()
    set(MODB_MINGW_LINK_OPTS)
endif()

add_library(modb
    src/model/data_type.cpp
    src/model/schema.cpp
    src/model/value.cpp
    src/object/attribute_value.cpp
    src/object/baseline.cpp
    src/object/binding.cpp
    src/object/blob_store.cpp
    src/object/catalog_store.cpp
    src/object/collection.cpp
    src/object/database.cpp
    src/object/database_root.cpp
    src/object/identity_map.cpp
    src/object/index_catalog.cpp
    src/object/object_codec.cpp
    src/object/object_store.cpp
    src/object/projection_plan.cpp
    src/object/type_definition.cpp
    src/object/type_registry.cpp
    src/index/btree.cpp
    src/net/protocol.cpp
    src/net/native_socket.cpp
    src/net/server.cpp
    src/net/client.cpp
    src/storage/binary.cpp
    src/storage/codec.cpp
    src/storage/page_file.cpp
    src/storage/database_check.cpp
    src/storage/native_file.cpp
    src/storage/page_cache.cpp
    src/storage/scratch_page_pool.cpp
    src/storage/slotted_page.cpp
    src/storage/table_heap.cpp
    src/text_escape.cpp
    src/tx/recovery.cpp
    src/tx/wal.cpp
    src/version.cpp
)
add_library(modb::modb ALIAS modb)
target_compile_definitions(modb PUBLIC MODB_PAGE_SIZE=${MODB_PAGE_SIZE})
if(WIN32)
    target_link_libraries(modb PUBLIC ws2_32)
endif()

if("cxx_std_26" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
    target_compile_features(modb PUBLIC cxx_std_26)
elseif(MODB_ALLOW_CXX23_FALLBACK)
    message(WARNING
        "The compiler does not support C++26; using the explicitly requested "
        "C++23 local-development fallback"
    )
    target_compile_features(modb PUBLIC cxx_std_23)
else()
    message(FATAL_ERROR
        "moDb requires a compiler with C++26 support. Use a newer compiler or "
        "set MODB_ALLOW_CXX23_FALLBACK=ON only for local bootstrap validation."
    )
endif()
target_include_directories(modb
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

if(MSVC)
    # /sdl amplia as checagens de segurança; /GS (stack canary) já é padrão.
    target_compile_options(modb PRIVATE /W4 /permissive- /Zc:preprocessor /sdl)
    if(MODB_WARNINGS_AS_ERRORS)
        target_compile_options(modb PRIVATE /WX)
    endif()
else()
    target_compile_options(modb PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        # Hardening: o arquivo .db é entrada não confiável e o parsing vive aqui.
        -fstack-protector-strong
    )
    # Bounds checks da libstdc++ nas builds de depuração (casa com os asserts de Page).
    target_compile_definitions(modb PRIVATE $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>)
    if(MODB_WARNINGS_AS_ERRORS)
        target_compile_options(modb PRIVATE -Werror)
    endif()
endif()

if(MODB_ENABLE_SANITIZERS)
    # Cada toolchain ativa o sanitizer que realmente possui. Nenhum ramo aborta a
    # configuração: no pior caso avisa e segue, para o preset ser sempre utilizável.
    if(MSVC)
        # MSVC oferece AddressSanitizer; UBSan não existe no toolset.
        # /Zi gera símbolos e o ASan do MSVC é incompatível com link incremental.
        target_compile_options(modb PUBLIC /fsanitize=address /Zi)
        target_link_options(modb PUBLIC /INCREMENTAL:NO)
        message(STATUS "moDb: AddressSanitizer ativado (MSVC).")
    elseif(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # clang-cl e clang-mingw suportam ASan/UBSan no Windows.
        target_compile_options(modb PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(modb PUBLIC -fsanitize=address,undefined)
        message(STATUS "moDb: Address/UndefinedBehaviorSanitizer ativado (Clang no Windows).")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT WIN32)
        # Clang e GCC fora do Windows têm o suporte completo a ASan/UBSan.
        target_compile_options(modb PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(modb PUBLIC -fsanitize=address,undefined)
        message(STATUS "moDb: Address/UndefinedBehaviorSanitizer ativado (Clang/GCC).")
    elseif(MINGW)
        # O GCC do MinGW-w64 não implementa ASan/UBSan. A rede de segurança viável
        # aqui é o hardening da libstdc++, ativado independentemente do build type.
        target_compile_options(modb PUBLIC -fstack-protector-strong)
        target_compile_definitions(modb PUBLIC _GLIBCXX_ASSERTIONS)
        message(WARNING
            "moDb: MinGW GCC não suporta ASan/UBSan. Ativando a rede viável "
            "(_GLIBCXX_ASSERTIONS + -fstack-protector-strong). Para ASan/UBSan use "
            "Clang no Windows ou GCC/Clang em Linux/macOS.")
    else()
        message(WARNING
            "moDb: MODB_ENABLE_SANITIZERS ignorado — nenhum sanitizer conhecido para "
            "o toolchain '${CMAKE_CXX_COMPILER_ID}'.")
    endif()
endif()

add_executable(modb_cli apps/modb_cli/main.cpp)
target_link_libraries(modb_cli PRIVATE modb::modb)
set_target_properties(modb_cli PROPERTIES OUTPUT_NAME modb)

add_executable(modb_object_bench benchmarks/object_throughput.cpp)
target_link_libraries(modb_object_bench PRIVATE modb::modb)

if(MINGW)
    # O build estático do MinGW pode emitir type_info::operator== tanto nos
    # consumidores de std::function quanto em libstdc++. O motor não depende
    # de RTTI; desativá-lo evita a definição duplicada e mantém a ABI coerente.
    target_compile_options(modb PUBLIC -fno-rtti)
    target_link_options(modb_cli PRIVATE ${MODB_MINGW_LINK_OPTS})
    target_link_options(modb_object_bench PRIVATE ${MODB_MINGW_LINK_OPTS})
endif()

if(BUILD_TESTING)
    add_executable(modb_tests tests/version_test.cpp)
    target_link_libraries(modb_tests PRIVATE modb::modb)

    add_executable(modb_model_tests tests/model_test.cpp)
    target_link_libraries(modb_model_tests PRIVATE modb::modb)

    add_executable(modb_object_model_tests tests/object_model_test.cpp)
    target_link_libraries(modb_object_model_tests PRIVATE modb::modb)

    add_executable(modb_object_codec_tests tests/object_codec_test.cpp)
    target_link_libraries(modb_object_codec_tests PRIVATE modb::modb)

    add_executable(modb_binding_tests tests/binding_test.cpp)
    target_link_libraries(modb_binding_tests PRIVATE modb::modb)

    add_executable(modb_projection_tests tests/projection_test.cpp)
    target_link_libraries(modb_projection_tests PRIVATE modb::modb)

    add_executable(modb_schema_evolution_tests tests/schema_evolution_test.cpp)
    target_link_libraries(modb_schema_evolution_tests PRIVATE modb::modb)

    add_executable(modb_wal_tests tests/wal_test.cpp)
    target_link_libraries(modb_wal_tests PRIVATE modb::modb)

    add_executable(modb_recovery_tests tests/recovery_test.cpp)
    target_link_libraries(modb_recovery_tests PRIVATE modb::modb)

    add_executable(modb_failpoint_tests tests/failpoint_test.cpp)
    target_link_libraries(modb_failpoint_tests PRIVATE modb::modb)

    add_executable(modb_blob_store_tests tests/blob_store_test.cpp)
    target_link_libraries(modb_blob_store_tests PRIVATE modb::modb)

    add_executable(modb_relationship_tests tests/relationship_test.cpp)
    target_link_libraries(modb_relationship_tests PRIVATE modb::modb)

    add_executable(modb_collection_tests tests/collection_test.cpp)
    target_link_libraries(modb_collection_tests PRIVATE modb::modb)

    add_executable(modb_identity_map_tests tests/identity_map_test.cpp)
    target_link_libraries(modb_identity_map_tests PRIVATE modb::modb)

    add_executable(modb_object_store_tests tests/object_store_test.cpp)
    target_link_libraries(modb_object_store_tests PRIVATE modb::modb)

    add_executable(modb_snapshot_tests tests/snapshot_test.cpp)
    target_link_libraries(modb_snapshot_tests PRIVATE modb::modb)

    add_executable(modb_mvcc_recovery_tests tests/mvcc_recovery_test.cpp)
    target_link_libraries(modb_mvcc_recovery_tests PRIVATE modb::modb)

    add_executable(modb_generator_tests tests/generator_test.cpp)
    target_link_libraries(modb_generator_tests PRIVATE modb::modb)

    add_executable(modb_streaming_query_tests tests/streaming_query_test.cpp)
    target_link_libraries(modb_streaming_query_tests PRIVATE modb::modb)

    add_executable(modb_btree_tests tests/btree_test.cpp)
    target_link_libraries(modb_btree_tests PRIVATE modb::modb)

    add_executable(modb_indexed_query_tests tests/indexed_query_test.cpp)
    target_link_libraries(modb_indexed_query_tests PRIVATE modb::modb)

    add_executable(modb_index_catalog_tests tests/index_catalog_test.cpp)
    target_link_libraries(modb_index_catalog_tests PRIVATE modb::modb)

    add_executable(modb_projection_query_tests tests/projection_query_test.cpp)
    target_link_libraries(modb_projection_query_tests PRIVATE modb::modb)

    add_executable(modb_aggregation_query_tests tests/aggregation_query_test.cpp)
    target_link_libraries(modb_aggregation_query_tests PRIVATE modb::modb)

    add_executable(modb_planner_tests tests/planner_test.cpp)
    target_link_libraries(modb_planner_tests PRIVATE modb::modb)

    add_executable(modb_protocol_tests tests/protocol_test.cpp)
    target_link_libraries(modb_protocol_tests PRIVATE modb::modb)

    add_executable(modb_server_streaming_tests tests/server_streaming_test.cpp)
    target_link_libraries(modb_server_streaming_tests PRIVATE modb::modb)

    add_executable(modb_catalog_persistence_tests tests/catalog_persistence_test.cpp)
    target_link_libraries(modb_catalog_persistence_tests PRIVATE modb::modb)

    add_executable(modb_storage_tests tests/storage_test.cpp)
    target_link_libraries(modb_storage_tests PRIVATE modb::modb)

    add_executable(modb_page_cache_tests tests/page_cache_test.cpp)
    target_link_libraries(modb_page_cache_tests PRIVATE modb::modb)

    add_executable(modb_codec_tests tests/codec_test.cpp)
    target_link_libraries(modb_codec_tests PRIVATE modb::modb)

    add_executable(modb_text_escape_tests tests/text_escape_test.cpp)
    target_link_libraries(modb_text_escape_tests PRIVATE modb::modb)

    add_executable(modb_slotted_page_tests tests/slotted_page_test.cpp)
    target_link_libraries(modb_slotted_page_tests PRIVATE modb::modb)

    add_executable(modb_table_heap_tests tests/table_heap_test.cpp)
    target_link_libraries(modb_table_heap_tests PRIVATE modb::modb)

    add_executable(modb_database_check_tests tests/database_check_test.cpp)
    target_link_libraries(modb_database_check_tests PRIVATE modb::modb)

    add_executable(modb_table_heap_churn_tests tests/table_heap_churn_test.cpp)
    target_compile_definitions(modb_table_heap_churn_tests
        PRIVATE MODB_CHURN_RECORD_TOTAL=${MODB_CHURN_RECORD_TOTAL})
    target_link_libraries(modb_table_heap_churn_tests PRIVATE modb::modb)

    add_executable(modb_table_heap_reverse_churn_tests tests/table_heap_churn_test.cpp)
    target_compile_definitions(modb_table_heap_reverse_churn_tests
        PRIVATE
            MODB_CHURN_RECORD_TOTAL=${MODB_CHURN_RECORD_TOTAL}
            MODB_DELETE_REVERSE=1)
    target_link_libraries(modb_table_heap_reverse_churn_tests PRIVATE modb::modb)

    add_executable(modb_table_heap_space_reuse_tests tests/table_heap_space_reuse_test.cpp)
    target_link_libraries(modb_table_heap_space_reuse_tests PRIVATE modb::modb)

    add_executable(modb_table_heap_space_reuse_65_tests tests/table_heap_space_reuse_test.cpp)
    target_compile_definitions(modb_table_heap_space_reuse_65_tests
        PRIVATE MODB_REPLACEMENT_RECORD_SIZE=65)
    target_link_libraries(modb_table_heap_space_reuse_65_tests PRIVATE modb::modb)

    if(MINGW)
        target_link_options(modb_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_model_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_object_model_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_object_codec_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_binding_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_projection_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_schema_evolution_tests
            PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_wal_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_recovery_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_failpoint_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_blob_store_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_relationship_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_collection_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_identity_map_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_object_store_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_snapshot_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_mvcc_recovery_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_generator_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_streaming_query_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_btree_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_indexed_query_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_index_catalog_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_projection_query_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_aggregation_query_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_planner_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_protocol_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_server_streaming_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_catalog_persistence_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_storage_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_page_cache_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_codec_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_text_escape_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_slotted_page_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_table_heap_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_database_check_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_table_heap_churn_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_table_heap_reverse_churn_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_table_heap_space_reuse_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
        target_link_options(modb_table_heap_space_reuse_65_tests PRIVATE ${MODB_MINGW_LINK_OPTS})
    endif()

    add_test(NAME modb.unit COMMAND modb_tests)
    add_test(NAME modb.model COMMAND modb_model_tests)
    add_test(NAME modb.object_model COMMAND modb_object_model_tests)
    add_test(NAME modb.object_codec COMMAND modb_object_codec_tests)
    add_test(NAME modb.binding COMMAND modb_binding_tests)
    add_test(NAME modb.projection COMMAND modb_projection_tests)
    add_test(NAME modb.schema_evolution COMMAND modb_schema_evolution_tests)
    add_test(NAME modb.wal COMMAND modb_wal_tests)
    add_test(NAME modb.recovery COMMAND modb_recovery_tests)
    add_test(NAME modb.failpoint COMMAND modb_failpoint_tests)
    add_test(NAME modb.blob_store COMMAND modb_blob_store_tests)
    add_test(NAME modb.relationship COMMAND modb_relationship_tests)
    add_test(NAME modb.collection COMMAND modb_collection_tests)
    add_test(NAME modb.identity_map COMMAND modb_identity_map_tests)
    add_test(NAME modb.object_store COMMAND modb_object_store_tests)
    add_test(NAME modb.snapshot COMMAND modb_snapshot_tests)
    add_test(NAME modb.mvcc_recovery COMMAND modb_mvcc_recovery_tests)
    add_test(NAME modb.generator COMMAND modb_generator_tests)
    add_test(NAME modb.streaming_query COMMAND modb_streaming_query_tests)
    add_test(NAME modb.btree COMMAND modb_btree_tests)
    add_test(NAME modb.indexed_query COMMAND modb_indexed_query_tests)
    add_test(NAME modb.index_catalog COMMAND modb_index_catalog_tests)
    add_test(NAME modb.projection_query COMMAND modb_projection_query_tests)
    add_test(NAME modb.aggregation_query COMMAND modb_aggregation_query_tests)
    add_test(NAME modb.planner COMMAND modb_planner_tests)
    add_test(NAME modb.protocol COMMAND modb_protocol_tests)
    add_test(NAME modb.server_streaming COMMAND modb_server_streaming_tests)
    add_test(NAME modb.catalog_persistence COMMAND modb_catalog_persistence_tests)
    add_test(NAME modb.storage COMMAND modb_storage_tests)
    add_test(NAME modb.page_cache COMMAND modb_page_cache_tests)
    add_test(NAME modb.codec COMMAND modb_codec_tests)
    add_test(NAME modb.text_escape COMMAND modb_text_escape_tests)
    add_test(NAME modb.slotted_page COMMAND modb_slotted_page_tests)
    add_test(NAME modb.table_heap COMMAND modb_table_heap_tests)
    add_test(NAME modb.database_check COMMAND modb_database_check_tests)
    add_test(NAME modb.table_heap_churn COMMAND modb_table_heap_churn_tests)
    set_tests_properties(modb.table_heap_churn PROPERTIES LABELS performance TIMEOUT 300)
    add_test(NAME modb.table_heap_reverse_churn COMMAND modb_table_heap_reverse_churn_tests)
    set_tests_properties(modb.table_heap_reverse_churn PROPERTIES LABELS performance TIMEOUT 300)
    add_test(NAME modb.table_heap_space_reuse COMMAND modb_table_heap_space_reuse_tests)
    set_tests_properties(modb.table_heap_space_reuse PROPERTIES LABELS performance TIMEOUT 300)
    add_test(NAME modb.table_heap_space_reuse_65 COMMAND modb_table_heap_space_reuse_65_tests)
    set_tests_properties(modb.table_heap_space_reuse_65 PROPERTIES LABELS performance TIMEOUT 300)
    add_test(NAME modb.cli.version COMMAND modb_cli --version)
    add_test(NAME modb.cli.help COMMAND modb_cli --help)
    add_test(NAME modb.cli.demo COMMAND modb_cli demo)
    add_test(NAME modb.cli.demo_help COMMAND modb_cli demo --help)
    add_test(NAME modb.cli.demo_run_help COMMAND modb_cli demo run --help)
    add_test(NAME modb.cli.db_help COMMAND modb_cli db --help)
    add_test(NAME modb.cli.page_help COMMAND modb_cli page --help)
    add_test(NAME modb.cli.record_help COMMAND modb_cli record --help)
    add_test(NAME modb.cli.heap_help COMMAND modb_cli heap --help)
    add_test(NAME modb.cli.codec COMMAND modb_cli codec)
    add_test(NAME modb.cli.codec_help COMMAND modb_cli codec --help)
    add_test(NAME modb.cli.protocol_demo COMMAND modb_cli protocol demo)
    add_test(NAME modb.cli.protocol_help COMMAND modb_cli protocol --help)
    add_test(NAME modb.cli.serve_help COMMAND modb_cli serve --help)
    add_test(NAME modb.cli.serve_demo
        COMMAND modb_cli serve demo
                ${CMAKE_CURRENT_BINARY_DIR}/modb-cli-serve-demo.modb --force)
    add_test(NAME modb.cli.serve_query_demo
        COMMAND modb_cli serve query-demo
                ${CMAKE_CURRENT_BINARY_DIR}/modb-cli-serve-query-demo.modb --force)
    add_test(NAME modb.cli.ping_help COMMAND modb_cli ping --help)
    add_test(NAME modb.cli.types COMMAND modb_cli types)
    add_test(NAME modb.cli.types_help COMMAND modb_cli types --help)
    add_test(NAME modb.cli.type_help COMMAND modb_cli type --help)
    add_test(NAME modb.cli.baseline_help COMMAND modb_cli baseline --help)
    add_test(NAME modb.cli.object_help COMMAND modb_cli object --help)
    add_test(NAME modb.cli.oo_help COMMAND modb_cli oo --help)
    add_test(NAME modb.cli.oo_demo
        COMMAND modb_cli oo employee demo
                ${CMAKE_CURRENT_BINARY_DIR}/modb-cli-phase3-demo.modb --force)
    add_test(NAME modb.cli.query_help COMMAND modb_cli query --help)
    # Consulta em streaming (Fase 7A), comando top-level `query`, sobre o
    # arquivo evoluído da demo (2 Employees, tipo lógico único em duas
    # versões). Ordem fixada via DEPENDS.
    add_test(NAME modb.cli.query
        COMMAND modb_cli query
                ${CMAKE_CURRENT_BINARY_DIR}/modb-cli-phase3-demo.modb --schema 2 --limit 1)
    set_tests_properties(modb.cli.query PROPERTIES DEPENDS modb.cli.oo_demo)
    add_test(NAME modb.cli.query_project
        COMMAND modb_cli query
                ${CMAKE_CURRENT_BINARY_DIR}/modb-cli-phase3-demo.modb --schema 2
                --project id,name,salary --compute annual_salary --limit 2)
    set_tests_properties(modb.cli.query_project PROPERTIES DEPENDS modb.cli.query)
    add_test(NAME modb.cli.query_materialize
        COMMAND modb_cli query
                ${CMAKE_CURRENT_BINARY_DIR}/modb-cli-phase3-demo.modb --schema 2
                --project id,name,salary --compute annual_salary
                --materialize-summary --limit 2)
    set_tests_properties(modb.cli.query_materialize PROPERTIES DEPENDS modb.cli.query_project)
    add_test(NAME modb.cli.query_topk
        COMMAND modb_cli query
                ${CMAKE_CURRENT_BINARY_DIR}/modb-cli-phase3-demo.modb --schema 2
                --order-by salary --top 1)
    set_tests_properties(modb.cli.query_topk PROPERTIES DEPENDS modb.cli.query_materialize)
    # Índice (Fase 7B): cria sobre a demo (ainda em `oo employee index`), depois
    # consulta por igualdade de salário via Index Scan no comando `query`.
    add_test(NAME modb.cli.oo_index
        COMMAND modb_cli oo employee index
                ${CMAKE_CURRENT_BINARY_DIR}/modb-cli-phase3-demo.modb --schema 2)
    add_test(NAME modb.cli.query_indexed
        COMMAND modb_cli query
                ${CMAKE_CURRENT_BINARY_DIR}/modb-cli-phase3-demo.modb --schema 2 --salary 18000)
    set_tests_properties(modb.cli.oo_index PROPERTIES DEPENDS modb.cli.query_topk)
    set_tests_properties(modb.cli.query_indexed PROPERTIES DEPENDS modb.cli.oo_index)
    add_test(NAME modb.cli.query_explain
        COMMAND modb_cli query
                ${CMAKE_CURRENT_BINARY_DIR}/modb-cli-phase3-demo.modb --schema 2
                --salary 18000 --limit 1 --explain)
    set_tests_properties(modb.cli.query_explain PROPERTIES DEPENDS modb.cli.query_indexed)
    add_test(NAME modb.cli.blob_help COMMAND modb_cli blob --help)
    add_test(NAME modb.cli.graph_help COMMAND modb_cli graph --help)
    add_test(NAME modb.cli.coll_help COMMAND modb_cli coll --help)
    add_test(NAME modb.cli.blob_put
        COMMAND modb_cli blob put ${CMAKE_CURRENT_BINARY_DIR}/modb-cli-phase4-blob.modb hello)
    add_test(NAME modb.cli.graph_demo
        COMMAND modb_cli graph demo
                ${CMAKE_CURRENT_BINARY_DIR}/modb-cli-phase4-graph.modb --force)
    add_test(NAME modb.cli.coll_demo
        COMMAND modb_cli coll demo
                ${CMAKE_CURRENT_BINARY_DIR}/modb-cli-phase4-coll.modb --force)
    add_test(NAME modb.cli.tx_help COMMAND modb_cli tx --help)
    add_test(NAME modb.cli.mvcc_help COMMAND modb_cli mvcc --help)
    add_test(NAME modb.cli.mvcc_snapshot_demo
        COMMAND modb_cli mvcc snapshot-demo
                ${CMAKE_CURRENT_BINARY_DIR}/modb-cli-phase6b-snapshot-demo.modb --force)
    # gc roda sobre o arquivo que a demo deixou (já compactado): coleta 0 e sai
    # 0. Ordem fixada via DEPENDS para o arquivo existir e ser determinístico.
    add_test(NAME modb.cli.mvcc_gc
        COMMAND modb_cli mvcc gc
                ${CMAKE_CURRENT_BINARY_DIR}/modb-cli-phase6b-snapshot-demo.modb)
    set_tests_properties(modb.cli.mvcc_gc PROPERTIES
        DEPENDS modb.cli.mvcc_snapshot_demo)
    # status/versions são diagnósticos read-only sobre o arquivo que a demo
    # deixou (Ana = id 18). Ordem fixada via DEPENDS para o arquivo existir.
    add_test(NAME modb.cli.mvcc_status
        COMMAND modb_cli mvcc status
                ${CMAKE_CURRENT_BINARY_DIR}/modb-cli-phase6b-snapshot-demo.modb)
    add_test(NAME modb.cli.mvcc_versions
        COMMAND modb_cli mvcc versions
                ${CMAKE_CURRENT_BINARY_DIR}/modb-cli-phase6b-snapshot-demo.modb 18)
    set_tests_properties(modb.cli.mvcc_status modb.cli.mvcc_versions PROPERTIES
        DEPENDS modb.cli.mvcc_snapshot_demo)
    add_test(NAME modb.cli.tx_demo
        COMMAND modb_cli tx demo
                ${CMAKE_CURRENT_BINARY_DIR}/modb-cli-phase5-tx-demo.modb --force)
    add_test(NAME modb.cli.tx_crash_before_commit
        COMMAND modb_cli tx crash
                ${CMAKE_CURRENT_BINARY_DIR}/modb-cli-phase5-crash-before-commit.modb
                before-commit --force)
    add_test(NAME modb.cli.tx_crash_after_commit
        COMMAND modb_cli tx crash
                ${CMAKE_CURRENT_BINARY_DIR}/modb-cli-phase5-crash-after-commit.modb
                after-commit --force)
    add_test(NAME modb.cli.tx_crash_mid_apply
        COMMAND modb_cli tx crash
                ${CMAKE_CURRENT_BINARY_DIR}/modb-cli-phase5-crash-mid-apply.modb
                mid-apply --force)
    add_test(NAME modb.cli.tx_crash_before_cleanup
        COMMAND modb_cli tx crash
                ${CMAKE_CURRENT_BINARY_DIR}/modb-cli-phase5-crash-before-cleanup.modb
                before-cleanup --force)
    # wal-info roda com o WAL da queda ainda pendente; get roda depois e
    # dispara a recuperação (que remove o WAL) — ordem fixada via DEPENDS para
    # o cenário demonstrado ser sempre o mesmo.
    add_test(NAME modb.cli.tx_wal_info
        COMMAND modb_cli tx wal-info
                ${CMAKE_CURRENT_BINARY_DIR}/modb-cli-phase5-crash-after-commit.modb)
    add_test(NAME modb.cli.tx_get
        COMMAND modb_cli tx get
                ${CMAKE_CURRENT_BINARY_DIR}/modb-cli-phase5-crash-after-commit.modb 18)
    set_tests_properties(modb.cli.tx_wal_info PROPERTIES
        DEPENDS modb.cli.tx_crash_after_commit)
    set_tests_properties(modb.cli.tx_get PROPERTIES DEPENDS modb.cli.tx_wal_info)
endif()
