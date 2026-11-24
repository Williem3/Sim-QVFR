# Builds the example mod on its own, from the SDK folder:
#   cmake -S examples/hello -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release --target sqvfr_mod_zip_hello
# The zip (build/sqvfr-mod-hello-0.1.0.zip) is what a player drops on the launcher.
cmake_minimum_required(VERSION 3.21)
project(sqvfr_hello_mod CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/SqvfrMod.cmake)

sqvfr_add_mod(hello
  ID hello
  VERSION 0.1.0
  SOURCES hello.cpp
  MANIFEST ${CMAKE_CURRENT_LIST_DIR}/mod.json
  SETTINGS ${CMAKE_CURRENT_LIST_DIR}/settings.json)
