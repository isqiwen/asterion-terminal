# Saved Market and Trading connections and the strategy repository setting.
add_library(asterion_terminal_settings STATIC
  native/trading_connections.cpp
  native/market_endpoint.cpp
  native/strategy_repository.cpp
)
target_include_directories(asterion_terminal_settings PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/native)
target_link_libraries(asterion_terminal_settings PUBLIC asterion_node_control)
asterion_target_defaults(asterion_terminal_settings)

add_library(asterion_terminal_application STATIC
  native/python_runtime.cpp
  native/market_snapshot.cpp
  native/market_client.cpp
  native/terminal_application.cpp
  native/commands_node.cpp
  native/commands_connections.cpp
  native/commands_market.cpp
  native/commands_lab.cpp
  native/trading_client.cpp
  native/local_services.cpp
  native/local_service_health.cpp
)
target_compile_definitions(asterion_terminal_application PRIVATE ASTERION_PRODUCT_VERSION="${PROJECT_VERSION}")
target_include_directories(asterion_terminal_application PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/native)
target_link_libraries(asterion_terminal_application PUBLIC asterion_terminal_settings asterion_node_control asterion_protocol PRIVATE nlohmann_json::nlohmann_json OpenSSL::Crypto httplib::httplib)
add_executable(asterion_terminal_dev_bridge dev/core_bridge.cpp)
target_link_libraries(asterion_terminal_dev_bridge PRIVATE asterion_terminal_api CLI11::CLI11)
asterion_target_defaults(asterion_terminal_application)
asterion_target_defaults(asterion_terminal_dev_bridge)
