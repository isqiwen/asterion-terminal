add_library(asterion_node_management STATIC
  agent_client.cpp
  local_node.cpp
  node_program.cpp
  node_enrollment.cpp
  manager.cpp
)
target_include_directories(asterion_node_management PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(asterion_node_management PUBLIC asterion_node_control PRIVATE asterion_sqlite asterion_node_firewall OpenSSL::Crypto)
target_compile_definitions(asterion_node_management PRIVATE ASTERION_PRODUCT_VERSION="${PROJECT_VERSION}")
add_executable(asterion_node_manager main.cpp)
target_link_libraries(asterion_node_manager PRIVATE asterion_node_management CLI11::CLI11)
target_compile_definitions(asterion_node_manager PRIVATE ASTERION_PRODUCT_VERSION="${PROJECT_VERSION}")
asterion_target_defaults(asterion_node_management)
asterion_target_defaults(asterion_node_manager)
