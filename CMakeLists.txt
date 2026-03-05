cmake_minimum_required(VERSION 4.0)

find_package(cmake-fetch REQUIRED PATHS node_modules/cmake-fetch)

project(js C)

fetch_package(
  "github:bellard/mquickjs"
  SOURCE_DIR mqjs
  PATCHES
    patches/001-remove-unused-headers.patch
    patches/002-has-exception.patch
    patches/003-exception-uninitialized.patch
)

set(MQJS_BUILD_ATOMS OFF CACHE BOOL "Build the standard library atoms for ROM")

if(MQJS_BUILD_ATOMS)
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
      src/atoms.c
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

  set(mqjs_build $<TARGET_FILE:mqjs_build>)
else()
  find_program(
    mqjs_build
    NAMES mqjs-build
    PATHS build
    REQUIRED
  )
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(mqjs_build_width 64)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(mqjs_build_width 32)
endif()

add_custom_command(
  OUTPUT ${mqjs}/mquickjs_atom.h
  COMMAND ${mqjs_build} -m${mqjs_build_width} -a > ${mqjs}/mquickjs_atom.h
  CODEGEN
)

add_library(mqjs OBJECT)

set_target_properties(
  mqjs
  PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON
)

target_sources(
  mqjs
  PRIVATE
    ${mqjs}/mquickjs.c
    ${mqjs}/mquickjs_atom.h
    ${mqjs}/libm.c
    ${mqjs}/dtoa.c
    ${mqjs}/cutils.c
)

target_include_directories(
  mqjs
  PUBLIC
    ${mqjs}
)

add_library(js OBJECT)

fetch_package("github:holepunchto/libjs" SOURCE_DIR js)
fetch_package("github:holepunchto/libintrusive")

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/src/atoms.h
  COMMAND ${mqjs_build} -m${mqjs_build_width} > ${CMAKE_CURRENT_SOURCE_DIR}/src/atoms.h
  CODEGEN
)

set_target_properties(
  js
  PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON
)

target_sources(
  js
  PRIVATE
    src/js.c
    src/atoms.h
)

target_include_directories(
  js
  INTERFACE
    include
  PUBLIC
    ${js}/include
  PRIVATE
    $<TARGET_PROPERTY:mqjs,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:uv,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:utf,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:intrusive,INTERFACE_INCLUDE_DIRECTORIES>
)

add_library(js_shared SHARED)

set_target_properties(
  js_shared
  PROPERTIES
  OUTPUT_NAME js
  WINDOWS_EXPORT_ALL_SYMBOLS ON
)

target_include_directories(
  js_shared
  INTERFACE
    $<TARGET_PROPERTY:js,INTERFACE_INCLUDE_DIRECTORIES>
)

target_link_libraries(
  js_shared
  PUBLIC
    mqjs
    uv
    utf_shared
  PRIVATE
    js
)

add_library(js_static STATIC)

set_target_properties(
  js_static
  PROPERTIES
  OUTPUT_NAME js
  PREFIX lib
)

target_include_directories(
  js_static
  INTERFACE
    $<TARGET_PROPERTY:js,INTERFACE_INCLUDE_DIRECTORIES>
)

target_link_libraries(
  js_static
  PUBLIC
    mqjs
    uv_a
    utf_static
  PRIVATE
    js
)

install(TARGETS js_shared js_static)

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()

  add_subdirectory(test)
endif()
