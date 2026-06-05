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
  src/binder/expression_utils.cpp
  src/catalog/hosted_catalog_api.cpp
  src/catalog/in_memory_catalog.cpp
  src/execution/default_executor.cpp
  src/lexer/lexer.cpp
  src/optimizer/default_optimizer.cpp
  src/parser/parser.cpp
  src/planner/default_logical_planner.cpp
  src/planner/default_physical_planner.cpp
  src/runtime/c_abi_platform_runtime.cpp
  src/runtime/hosted_platform_runtime.cpp
  src/runtime/platform.cpp
  src/sql_engine.cpp
  src/storage/in_memory_table_storage.cpp
  src/storage/memory_block_device.cpp
  src/storage/heap/page_heap_table.cpp
  src/storage/page/slotted_page.cpp
  src/storage/tuple_codec.cpp
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
    tests/abi_layout_tests.cpp
    tests/binder_tests.cpp
    tests/c_abi_platform_runtime_tests.cpp
    tests/catalog_tests.cpp
    tests/executor_tests.cpp
    tests/hosted_platform_runtime_tests.cpp
    tests/in_memory_table_storage_tests.cpp
    tests/interface_compile_tests.cpp
    tests/lexer_tests.cpp
    tests/memory_block_device_tests.cpp
    tests/optimizer_tests.cpp
    tests/physical_planner_tests.cpp
    tests/planner_tests.cpp
    tests/parser_tests.cpp
    tests/page_heap_table_tests.cpp
    tests/slotted_page_tests.cpp
    tests/sql_engine_tests.cpp
    tests/sql_logic_support.cpp
    tests/sql_logic_tests.cpp
    tests/test_main.cpp
  )
  target_link_libraries(mattsql_tests PRIVATE MattSQL::core)
  target_compile_definitions(
    mattsql_tests
    PRIVATE
      MATTSQL_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  )
  mattsql_apply_warnings(mattsql_tests)

  add_test(NAME mattsql.tests COMMAND mattsql_tests)

  find_program(MATTSQL_RUSTC rustc)
  if(MATTSQL_RUSTC)
    add_test(
      NAME mattsql.rust_abi_layout
      COMMAND
        ${MATTSQL_RUSTC}
        --edition=2021
        --crate-type
        lib
        --emit
        metadata
        -o
        ${CMAKE_CURRENT_BINARY_DIR}/mattsql_abi_runtime_layout.rmeta
        ${PROJECT_SOURCE_DIR}/tests/rust/abi_runtime_layout.rs
    )
  endif()
endif()
