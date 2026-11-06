file(STRINGS "${ROOT}/VERSION" ASTERION_VERSION LIMIT_COUNT 1)
execute_process(COMMAND git -C "${ROOT}" rev-parse --short=12 HEAD
  OUTPUT_VARIABLE ASTERION_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
configure_file("${ROOT}/cmake/version.hpp.in" "${OUTPUT}/generated/asterion/version.hpp" @ONLY)
configure_file("${ROOT}/cmake/product.json.in" "${OUTPUT}/product.json" @ONLY)
