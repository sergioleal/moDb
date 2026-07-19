cmake_minimum_required(VERSION 3.30)
project(modb_consumer LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)

find_package(moDb CONFIG REQUIRED)
add_executable(modb_consumer main.cpp)
target_link_libraries(modb_consumer PRIVATE modb::modb)
