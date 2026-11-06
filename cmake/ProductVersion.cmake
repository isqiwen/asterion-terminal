file(STRINGS "${ROOT}/VERSION" ASTERION_VERSION LIMIT_COUNT 1)
# The Linux bundle container builds from a source archive without git history.
if(DEFINED ENV{ASTERION_COMMIT})
  set(ASTERION_COMMIT "$ENV{ASTERION_COMMIT}")
else()
  execute_process(COMMAND git -C "${ROOT}" rev-parse --short=12 HEAD
    OUTPUT_VARIABLE ASTERION_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
endif()
configure_file("${ROOT}/cmake/version.hpp.in" "${OUTPUT}/generated/asterion/version.hpp" @ONLY)
configure_file("${ROOT}/cmake/product.json.in" "${OUTPUT}/product.json" @ONLY)
