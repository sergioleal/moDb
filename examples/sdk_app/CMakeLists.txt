# Aplicação de exemplo compilada contra o SDK instalado do Ring0 (release
# v0.1.0 ou `cmake --install`), sem o código-fonte do motor: só o módulo de
# procs é compilado aqui. É o modelo para o servidor de uma aplicação nova.
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<sdk>
#   cmake --build build
#
# Troque as fontes abaixo pelas do seu módulo; o nome do alvo da biblioteca
# define a função de entrada `modb::server::Module modb_module_<alvo>()`.
cmake_minimum_required(VERSION 3.30)
project(notas_sdk_app LANGUAGES CXX)

find_package(moDb 0.1 CONFIG REQUIRED)

add_library(notas_procs STATIC ../server_procs/notas_procs.cpp)
target_include_directories(notas_procs PUBLIC ../server_procs)
target_link_libraries(notas_procs PUBLIC modb::server_host)

modb_add_server(notas-server MODULES notas_procs)
