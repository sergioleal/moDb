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
set(MODB_PAGE_SIZE 4096 CACHE STRING "Database page size in bytes")
set(MODB_CHURN_RECORD_TOTAL 10000 CACHE STRING "Record count used by churn tests")

add_library(modb
    src/catalog/catalog.cpp
    src/catalog/table.cpp
    src/model/data_type.cpp
    src/model/schema.cpp
    src/model/value.cpp
    src/object/attribute_value.cpp
    src/object/baseline.cpp
    src/object/type_definition.cpp
    src/object/type_registry.cpp
    src/storage/binary.cpp
    src/storage/codec.cpp
    src/storage/page_file.cpp
    src/storage/database_check.cpp
    src/storage/native_file.cpp
    src/storage/scratch_page_pool.cpp
    src/storage/slotted_page.cpp
    src/storage/table_heap.cpp
    src/text_escape.cpp
    src/version.cpp
)
add_library(modb::modb ALIAS modb)
target_compile_definitions(modb PUBLIC MODB_PAGE_SIZE=${MODB_PAGE_SIZE})

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

if(MINGW)
    target_link_options(modb_cli PRIVATE -static -static-libgcc -static-libstdc++)
endif()

if(BUILD_TESTING)
    add_executable(modb_tests tests/version_test.cpp)
    target_link_libraries(modb_tests PRIVATE modb::modb)

    add_executable(modb_model_tests tests/model_test.cpp)
    target_link_libraries(modb_model_tests PRIVATE modb::modb)

    add_executable(modb_object_model_tests tests/object_model_test.cpp)
    target_link_libraries(modb_object_model_tests PRIVATE modb::modb)

    add_executable(modb_catalog_tests tests/catalog_test.cpp)
    target_link_libraries(modb_catalog_tests PRIVATE modb::modb)

    add_executable(modb_storage_tests tests/storage_test.cpp)
    target_link_libraries(modb_storage_tests PRIVATE modb::modb)

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
        target_link_options(modb_tests PRIVATE -static -static-libgcc -static-libstdc++)
        target_link_options(modb_model_tests PRIVATE -static -static-libgcc -static-libstdc++)
        target_link_options(modb_object_model_tests PRIVATE -static -static-libgcc -static-libstdc++)
        target_link_options(modb_catalog_tests PRIVATE -static -static-libgcc -static-libstdc++)
        target_link_options(modb_storage_tests PRIVATE -static -static-libgcc -static-libstdc++)
        target_link_options(modb_codec_tests PRIVATE -static -static-libgcc -static-libstdc++)
        target_link_options(modb_text_escape_tests PRIVATE -static -static-libgcc -static-libstdc++)
        target_link_options(modb_slotted_page_tests PRIVATE -static -static-libgcc -static-libstdc++)
        target_link_options(modb_table_heap_tests PRIVATE -static -static-libgcc -static-libstdc++)
        target_link_options(modb_database_check_tests PRIVATE -static -static-libgcc -static-libstdc++)
        target_link_options(modb_table_heap_churn_tests PRIVATE -static -static-libgcc -static-libstdc++)
        target_link_options(modb_table_heap_reverse_churn_tests PRIVATE -static -static-libgcc -static-libstdc++)
        target_link_options(modb_table_heap_space_reuse_tests PRIVATE -static -static-libgcc -static-libstdc++)
        target_link_options(modb_table_heap_space_reuse_65_tests PRIVATE -static -static-libgcc -static-libstdc++)
    endif()

    add_test(NAME modb.unit COMMAND modb_tests)
    add_test(NAME modb.model COMMAND modb_model_tests)
    add_test(NAME modb.object_model COMMAND modb_object_model_tests)
    add_test(NAME modb.catalog COMMAND modb_catalog_tests)
    add_test(NAME modb.storage COMMAND modb_storage_tests)
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
    add_test(NAME modb.cli.catalog COMMAND modb_cli catalog)
    add_test(NAME modb.cli.catalog_help COMMAND modb_cli catalog --help)
    add_test(NAME modb.cli.types COMMAND modb_cli types)
    add_test(NAME modb.cli.types_help COMMAND modb_cli types --help)
endif()
