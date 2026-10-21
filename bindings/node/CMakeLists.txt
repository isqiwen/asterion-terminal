# Node-API is the desktop SDK boundary. Headers are explicitly prepared from
# the pinned Electron release; Conan still owns all Core dependencies.
if(ASTERION_NODE_HEADERS)
  if(NOT EXISTS "${ASTERION_NODE_HEADERS}/node_api.h")
    message(FATAL_ERROR "Prepare the pinned Electron SDK before building the desktop")
  endif()
  add_library(asterion_terminal_node MODULE terminal.cpp)
  target_compile_features(asterion_terminal_node PRIVATE cxx_std_20)
  target_compile_definitions(asterion_terminal_node PRIVATE NAPI_VERSION=8)
  target_include_directories(asterion_terminal_node PRIVATE "${ASTERION_NODE_HEADERS}" ${PROJECT_SOURCE_DIR}/apps/clients/terminal/native)
  target_link_libraries(asterion_terminal_node PRIVATE asterion_terminal_api)
  set_target_properties(asterion_terminal_node PROPERTIES PREFIX "" SUFFIX ".node" OUTPUT_NAME "asterion_terminal")
  # Node-API symbols come from the host process. An ELF module leaves them
  # undefined by default, but would also take a same-named symbol of any
  # library the host already loaded (its zlib) over the locked dependency
  # linked into the module.
  if(APPLE)
    target_link_options(asterion_terminal_node PRIVATE "-undefined" "dynamic_lookup")
  else()
    target_link_options(asterion_terminal_node PRIVATE "-Wl,-Bsymbolic")
  endif()
endif()
