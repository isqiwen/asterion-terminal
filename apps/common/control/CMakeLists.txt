add_library(asterion_node_control STATIC
  service_io.cpp
  plugin_catalog.cpp
  node_snapshot.cpp
  service_programs.cpp
  node_service.cpp
  remote_bundle.cpp
  node_paths.cpp
)
target_include_directories(asterion_node_control PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(asterion_node_control PUBLIC asterion_protocol asterion_kernel PRIVATE OpenSSL::Crypto)
target_compile_definitions(asterion_node_control PRIVATE ASTERION_PRODUCT_VERSION="${PROJECT_VERSION}")
asterion_target_defaults(asterion_node_control)
