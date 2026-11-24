# sqvfr_add_mod: builds a Sim-QVFR game mod and the zip a player adds it with (Settings > Mods > Add a mod…, or the
# zip dropped on the launcher window).
#
#   include(path/to/sdk/cmake/SqvfrMod.cmake)
#   sqvfr_add_mod(<name>
#     ID <mod id>                 # a-z, 0-9, _ and -, up to 32: its folder under mods\ and mod.json's "id"
#     VERSION <version>           # mod.json's "version": it names the zip
#     SOURCES <files...>
#     MANIFEST <mod.json>
#     [SETTINGS <settings.json>]  # the Game tab's settings for it (schemas/settings.schema.json)
#     [FILES <more files...>])    # anything else the mod folder holds (images, shaders)
#
# Targets: sqvfr_mod_<name> (the DLL, sim-qvfr-<id>.dll, with mod.json beside it in <build>/mods/<config>/<id>/) and
# sqvfr_mod_zip_<name> (<build>/sqvfr-mod-<id>-<version>.zip: the mod folder's files at the zip's root).
#
# The headers: SQVFR_SDK_INCLUDE when it is set, else the SDK's include\ (a packaged SDK), else the repository's
# src\mod_api (this file inside the Sim-QVFR repository).
include_guard(GLOBAL)

if(NOT SQVFR_SDK_INCLUDE)
  if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../include/mod_api.h")
    set(SQVFR_SDK_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/../include")
  else()
    set(SQVFR_SDK_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/../../src/mod_api")
  endif()
endif()

function(sqvfr_add_mod name)
  cmake_parse_arguments(MOD "" "ID;VERSION;MANIFEST;SETTINGS" "SOURCES;FILES" ${ARGN})
  if(NOT MOD_ID OR NOT MOD_VERSION OR NOT MOD_SOURCES OR NOT MOD_MANIFEST)
    message(FATAL_ERROR "sqvfr_add_mod(${name}): ID, VERSION, SOURCES and MANIFEST are needed")
  endif()
  if(NOT MOD_ID MATCHES "^[a-z0-9_-]+$")
    message(FATAL_ERROR "sqvfr_add_mod(${name}): the ID '${MOD_ID}' isn't a mod id (a-z, 0-9, _ and -)")
  endif()

  set(target sqvfr_mod_${name})
  set(folder "${CMAKE_BINARY_DIR}/mods/$<CONFIG>/${MOD_ID}")
  add_library(${target} SHARED ${MOD_SOURCES})
  target_include_directories(${target} PRIVATE "${SQVFR_SDK_INCLUDE}")
  target_compile_features(${target} PRIVATE cxx_std_17)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
  endif()
  # The C runtime linked in: a mod loads with only its own folder and System32 searched for DLLs.
  set_target_properties(${target} PROPERTIES
    OUTPUT_NAME "sim-qvfr-${MOD_ID}"
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    RUNTIME_OUTPUT_DIRECTORY "${folder}")

  set(files "${MOD_MANIFEST}")
  if(MOD_SETTINGS)
    list(APPEND files "${MOD_SETTINGS}")
  endif()
  list(APPEND files ${MOD_FILES})
  set(names "sim-qvfr-${MOD_ID}.dll")
  foreach(file IN LISTS files)
    get_filename_component(base "${file}" NAME)
    list(APPEND names "${base}")
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${file}" "$<TARGET_FILE_DIR:${target}>/${base}")
  endforeach()

  set(zip "${CMAKE_BINARY_DIR}/sqvfr-mod-${MOD_ID}-${MOD_VERSION}.zip")
  add_custom_target(sqvfr_mod_zip_${name}
    COMMAND ${CMAKE_COMMAND} -E rm -f "${zip}"
    COMMAND ${CMAKE_COMMAND} -E tar cf "${zip}" --format=zip ${names}
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:${target}>"
    DEPENDS ${target}
    COMMENT "Zipping the ${MOD_ID} mod: ${zip}"
    VERBATIM)
endfunction()
