# Explorador Ultra — archivo raíz de construcción.
# Cada módulo de src/ es una biblioteca independiente (ver docs/ARCHITECTURE.md).
cmake_minimum_required(VERSION 3.22)

project(ExploradorUltra
    VERSION 0.0.1
    DESCRIPTION "Administrador de archivos rápido y multiplataforma"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # lo necesita clang-tidy
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

option(EU_BUILD_TESTS "Compilar las pruebas automatizadas" ON)
option(EU_BUILD_BENCH "Compilar las herramientas de medición" ON)
set(EU_SANITIZER "" CACHE STRING "Sanitizer a activar: address, undefined, thread o vacío")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(EuCompilerOptions)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Svg Sql LinguistTools)
qt_standard_project_setup()

# Todos los ejecutables van a build/<preset>/bin en las tres plataformas.
# (Sin esto, Qt los deja en sitios distintos en Windows y en Linux.)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_subdirectory(src/core)
add_subdirectory(src/tasks)
add_subdirectory(src/fs)
add_subdirectory(src/journal)
add_subdirectory(src/ui)
add_subdirectory(src/app)

if(EU_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()

if(EU_BUILD_BENCH)
    add_subdirectory(bench)
endif()
