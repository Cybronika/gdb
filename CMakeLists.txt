cmake_minimum_required(VERSION 3.20)
project(gdb CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /utf-8 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_library(gdb STATIC
  src/graph.cpp
  src/storage.cpp
  src/cypher.cpp)
target_include_directories(gdb PUBLIC include PRIVATE src)

add_executable(gdb_cli tools/gdb_cli.cpp)
target_link_libraries(gdb_cli PRIVATE gdb)

add_executable(gdb_tests tests/test_gdb.cpp)
target_link_libraries(gdb_tests PRIVATE gdb)

enable_testing()
add_test(NAME gdb_tests COMMAND gdb_tests)
