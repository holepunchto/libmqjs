cmake_minimum_required(VERSION 4.0)

find_package(cmake-fetch REQUIRED PATHS node_modules/cmake-fetch)
find_package(cmake-prelude REQUIRED PATHS node_modules/cmake-prelude)
find_package(cmake-test REQUIRED PATHS node_modules/cmake-test)
find_package(cmake-toolchains REQUIRED PATHS node_modules/cmake-toolchains)

project(js LANGUAGES C VERSION 0.0.0)

fetch_package("github:holepunchto/libintrusive")

fetch_package(
  "github:bellard/mquickjs"
  SOURCE_DIR mqjs
  PATCHES
    patches/001-is-object.patch
    patches/003-is-strict-equal.patch
    patches/004-to-int64.patch
    patches/005-catch-shadow.patch
    patches/006-arraybuffer-typedarray.patch
    patches/007-float16-array.patch
    patches/008-define-property-str.patch
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

get_property(shared GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS)

add_library(mqjs OBJECT)

set_target_properties(
  mqjs
  PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ${shared}
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

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/src/atoms.h
  COMMAND ${mqjs_build} -m${mqjs_build_width} > ${CMAKE_CURRENT_SOURCE_DIR}/src/atoms.h
  DEPENDS ${mqjs_build}
  CODEGEN
)

add_library_targets(
  js
  FEATURES
    c_std_11
  SOURCES
    src/js.c
    src/atoms.h
  LINK
    utf::<TYPE>
    PRIVATE
      intrusive::headers
      mqjs
    STATIC
      uv_a
    SHARED
      uv
  EXPORT OFF
)

target_include_directories(
  js_headers
  INTERFACE
    $<TARGET_PROPERTY:uv,INTERFACE_INCLUDE_DIRECTORIES>
)

fetch_package("github:holepunchto/libjs" SOURCE_DIR js)

target_sources(
  js_headers
  INTERFACE
    FILE_SET HEADERS
    BASE_DIRS ${js}/include
    FILES ${js}/include/js.h
)

add_test_subdirectory(test)
