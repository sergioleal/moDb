# Executa instalação em prefixo temporário e compila o consumidor externo.
# Uso (via ctest): cmake -DMODB_SOURCE_DIR=... -DMODB_BINARY_DIR=... -P run_consumer_check.cmake

if(NOT MODB_SOURCE_DIR OR NOT MODB_BINARY_DIR)
    message(FATAL_ERROR "MODB_SOURCE_DIR and MODB_BINARY_DIR are required")
endif()

set(PREFIX "${MODB_BINARY_DIR}/consumer-prefix")
set(BUILD_DIR "${MODB_BINARY_DIR}/consumer-build")
file(REMOVE_RECURSE "${PREFIX}" "${BUILD_DIR}")
file(MAKE_DIRECTORY "${PREFIX}" "${BUILD_DIR}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${MODB_BINARY_DIR}" --prefix "${PREFIX}"
    RESULT_VARIABLE install_rc
)
if(NOT install_rc EQUAL 0)
    message(FATAL_ERROR "cmake --install failed: ${install_rc}")
endif()

# Reutiliza o gerador/compilador do build pai (evita cair em NMake no Windows).
set(_cfg_args
    -S "${MODB_SOURCE_DIR}/tests/consumer"
    -B "${BUILD_DIR}"
    -DmoDb_DIR=${PREFIX}/lib/cmake/moDb
    -DCMAKE_PREFIX_PATH=${PREFIX}
)
if(DEFINED MODB_GENERATOR AND NOT MODB_GENERATOR STREQUAL "")
    list(APPEND _cfg_args -G "${MODB_GENERATOR}")
endif()
if(DEFINED MODB_CXX_COMPILER AND NOT MODB_CXX_COMPILER STREQUAL "")
    list(APPEND _cfg_args -DCMAKE_CXX_COMPILER=${MODB_CXX_COMPILER})
endif()
if(DEFINED MODB_C_COMPILER AND NOT MODB_C_COMPILER STREQUAL "")
    list(APPEND _cfg_args -DCMAKE_C_COMPILER=${MODB_C_COMPILER})
endif()
if(DEFINED MODB_BUILD_TYPE AND NOT MODB_BUILD_TYPE STREQUAL "")
    list(APPEND _cfg_args -DCMAKE_BUILD_TYPE=${MODB_BUILD_TYPE})
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${_cfg_args}
    RESULT_VARIABLE cfg_rc
)
if(NOT cfg_rc EQUAL 0)
    message(FATAL_ERROR "consumer configure failed: ${cfg_rc}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}"
    RESULT_VARIABLE build_rc
)
if(NOT build_rc EQUAL 0)
    message(FATAL_ERROR "consumer build failed: ${build_rc}")
endif()

if(EXISTS "${BUILD_DIR}/modb_consumer.exe")
    set(CONSUMER_BIN "${BUILD_DIR}/modb_consumer.exe")
elseif(EXISTS "${BUILD_DIR}/modb_consumer")
    set(CONSUMER_BIN "${BUILD_DIR}/modb_consumer")
elseif(EXISTS "${BUILD_DIR}/Debug/modb_consumer.exe")
    set(CONSUMER_BIN "${BUILD_DIR}/Debug/modb_consumer.exe")
else()
    message(FATAL_ERROR "modb_consumer binary not found under ${BUILD_DIR}")
endif()

execute_process(COMMAND "${CONSUMER_BIN}" RESULT_VARIABLE run_rc)
if(NOT run_rc EQUAL 0)
    message(FATAL_ERROR "consumer run failed: ${run_rc}")
endif()

message(STATUS "moDb consumer check OK")
