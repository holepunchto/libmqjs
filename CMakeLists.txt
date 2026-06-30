cmake_minimum_required(VERSION 4.0)

find_package(cmake-fetch REQUIRED PATHS node_modules/cmake-fetch)
find_package(cmake-toolchains REQUIRED PATHS node_modules/cmake-toolchains)

project(js C)

fetch_package(
  "github:bellard/mquickjs"
  SOURCE_DIR mqjs
  PATCHES
    patches/001-is-object.patch
    patches/002-sweep-finalize-swallowed-blocks.patch
    patches/003-is-strict-equal.patch
    patches/004-to-int64.patch
    patches/005-catch-shadow.patch
    patches/006-arraybuffer-typedarray.patch
    patches/007-float16-array.patch
)

include(ExternalProject)

set(mqjs_build_prefix ${CMAKE_CURRENT_BINARY_DIR}/atoms)

if(CMAKE_HOST_WIN32)
  set(mqjs_build ${mqjs_build_prefix}/bin/mqjs-build.exe)
else()
  set(mqjs_build ${mqjs_build_prefix}/bin/mqjs-build)
endif()

ExternalProject_Add(
  mqjs_build_host
  SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/atoms
  PREFIX ${mqjs_build_prefix}
  CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DCMAKE_MESSAGE_LOG_LEVEL=${CMAKE_MESSAGE_LOG_LEVEL}
    -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
    -DCMAKE_TOOLCHAIN_FILE=${CMAKE_HOST_TOOLCHAIN_FILE}
    -DCMAKE_INSTALL_PREFIX=${mqjs_build_prefix}
    -Dmqjs=${mqjs}
  BUILD_ALWAYS ON
  INSTALL_BYPRODUCTS ${mqjs_build}
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(mqjs_build_width 64)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(mqjs_build_width 32)
endif()

add_custom_command(
  OUTPUT ${mqjs}/mquickjs_atom.h
  COMMAND ${mqjs_build} -m${mqjs_build_width} -a > ${mqjs}/mquickjs_atom.h
  DEPENDS ${mqjs_build}
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

fetch_package("github:holepunchto/libjs#freeze-seal" SOURCE_DIR js)
fetch_package("github:holepunchto/libintrusive")

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/src/atoms.h
  COMMAND ${mqjs_build} -m${mqjs_build_width} > ${CMAKE_CURRENT_SOURCE_DIR}/src/atoms.h
  DEPENDS ${mqjs_build}
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
