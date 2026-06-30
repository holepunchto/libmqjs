cmake_minimum_required(VERSION 4.0)

project(mqjs_atoms C)

add_library(mqjs_stdlib OBJECT)

set_target_properties(
  mqjs_stdlib
  PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON
)

target_sources(
  mqjs_stdlib
  PRIVATE
    atoms.c
)

target_include_directories(
  mqjs_stdlib
  PUBLIC
    ${mqjs}
)

add_executable(mqjs_build)

set_target_properties(
  mqjs_build
  PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON
  OUTPUT_NAME mqjs-build
)

target_sources(
  mqjs_build
  PRIVATE
    ${mqjs}/mquickjs_build.c
)

target_link_libraries(
  mqjs_build
  PRIVATE
    mqjs_stdlib
)

install(TARGETS mqjs_build)
