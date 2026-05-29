cmake_minimum_required(VERSION 3.24)

project(
  MattSQL
  VERSION 0.1.0
  DESCRIPTION "A small custom C++ SQL engine"
  LANGUAGES CXX
)

option(MATTSQL_ENABLE_WARNINGS "Enable strict compiler warnings" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Applies the project's warning policy to a CMake target.
function(mattsql_apply_warnings target)
  if(NOT MATTSQL_ENABLE_WARNINGS)
    return()
  endif()

  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
  endif()
endfunction()

add_library(
  mattsql_core
  src/binder/default_binder.cpp
  src/catalog/hosted_catalog_api.cpp
  src/catalog/in_memory_catalog.cpp
  src/engine.cpp
  src/lexer/lexer.cpp
  src/parser/parser.cpp
)
add_library(MattSQL::core ALIAS mattsql_core)

target_include_directories(
  mattsql_core
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_compile_features(mattsql_core PUBLIC cxx_std_20)
mattsql_apply_warnings(mattsql_core)

add_executable(mattsql src/main.cpp)
target_link_libraries(mattsql PRIVATE MattSQL::core)
mattsql_apply_warnings(mattsql)

include(CTest)

if(BUILD_TESTING)
  add_executable(
    mattsql_tests
    tests/binder_tests.cpp
    tests/catalog_tests.cpp
    tests/engine_tests.cpp
    tests/interface_compile_tests.cpp
    tests/lexer_tests.cpp
    tests/parser_tests.cpp
  )
  target_link_libraries(mattsql_tests PRIVATE MattSQL::core)
  mattsql_apply_warnings(mattsql_tests)

  add_test(NAME mattsql.tests COMMAND mattsql_tests)
endif()
