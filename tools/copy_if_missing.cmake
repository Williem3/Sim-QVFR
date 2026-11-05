# cmake -DSOURCE=<file> -DDESTINATION=<file> -P copy_if_missing.cmake
# Copies SOURCE to DESTINATION only when DESTINATION does not exist yet (a build never overwrites edited settings).
if(NOT EXISTS "${DESTINATION}")
  file(COPY_FILE "${SOURCE}" "${DESTINATION}")
endif()
