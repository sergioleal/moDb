# modb_add_server(<alvo> MODULES <lib> [<lib>...])
#
# Gera o executável de um servidor de aplicação (modb::server_host) com os
# módulos listados compilados dentro dele (docs-process/PLANO_SERVIDOR_PROCS.md).
# Cada <lib> é um alvo CMake que define, em C++:
#
#     modb::server::Module modb_module_<lib>();
#
# com os hífens e pontos do nome do alvo trocados por '_'. O `main` gerado só
# junta os módulos e chama modb::server::run.
function(modb_add_server target)
    cmake_parse_arguments(ARG "" "" "MODULES" ${ARGN})
    if(NOT ARG_MODULES)
        message(FATAL_ERROR "modb_add_server(${target}): informe ao menos um módulo em MODULES")
    endif()

    set(MODB_SERVER_DECLS "")
    set(MODB_SERVER_LIST "")
    foreach(module IN LISTS ARG_MODULES)
        string(MAKE_C_IDENTIFIER "${module}" module_id)
        string(APPEND MODB_SERVER_DECLS "modb::server::Module modb_module_${module_id}();\n")
        string(APPEND MODB_SERVER_LIST "        modb_module_${module_id}(),\n")
    endforeach()

    set(main_file "${CMAKE_CURRENT_BINARY_DIR}/${target}_main.cpp")
    file(CONFIGURE OUTPUT "${main_file}" @ONLY CONTENT [=[
// Gerado por modb_add_server (cmake/ModbServer.cmake). Não editar.
#include "modb/server/host.hpp"

@MODB_SERVER_DECLS@
int main(int argc, char** argv) {
    const modb::server::Module modules[] = {
@MODB_SERVER_LIST@    };
    return modb::server::run(argc, argv, modules);
}
]=])

    add_executable(${target} "${main_file}")
    target_link_libraries(${target} PRIVATE modb::server_host ${ARG_MODULES})

    # Com MinGW, o executável importa o runtime da toolchain; copiado para
    # junto dele, o servidor roda fora do shell da toolchain (serviço, supervisor).
    if(MINGW)
        get_filename_component(modb_server_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
        foreach(dll libstdc++-6 libgcc_s_seh-1 libwinpthread-1)
            if(EXISTS "${modb_server_mingw_bin}/${dll}.dll")
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${modb_server_mingw_bin}/${dll}.dll" "$<TARGET_FILE_DIR:${target}>"
                    VERBATIM)
            endif()
        endforeach()
    endif()
endfunction()
