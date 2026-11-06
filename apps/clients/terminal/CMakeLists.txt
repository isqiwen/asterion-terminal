configure_file(config/logging.json ${CMAKE_BINARY_DIR}/config/logging.json COPYONLY)

# Saved Market and Trading connections and the strategy repository setting.
add_library(asterion_terminal_settings STATIC
  native/trading_connections.cpp
  native/saved_endpoint.cpp
)
target_include_directories(asterion_terminal_settings PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/native)
target_link_libraries(asterion_terminal_settings PUBLIC asterion_system)
asterion_target_defaults(asterion_terminal_settings)

add_library(asterion_terminal_application STATIC
  native/python_runtime.cpp
  native/quotes_client.cpp
  native/history_client.cpp
  native/terminal_application.cpp
  native/commands_connections.cpp
  native/commands_data.cpp
  native/commands_lab.cpp
  native/trading_client.cpp
  native/lab_supervisor.cpp
  native/program_paths.cpp
  native/service_io.cpp
)
target_include_directories(asterion_terminal_application PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/native)
target_link_libraries(asterion_terminal_application PUBLIC asterion_terminal_settings asterion_protocol PRIVATE nlohmann_json::nlohmann_json OpenSSL::Crypto httplib::httplib)
add_executable(asterion_terminal_dev_bridge dev/core_bridge.cpp)
target_link_libraries(asterion_terminal_dev_bridge PRIVATE asterion_terminal_application CLI11::CLI11)
asterion_target_defaults(asterion_terminal_application)
asterion_target_defaults(asterion_terminal_dev_bridge)
add_subdirectory(bindings/node)
