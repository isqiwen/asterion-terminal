add_library(asterion_terminal_application STATIC
  native/market_snapshot.cpp
  native/market_client.cpp
  native/task_client.cpp
  native/data_client.cpp
  native/terminal_application.cpp
  native/commands_live.cpp
  native/commands_node.cpp
  native/commands_data.cpp
  native/commands_task.cpp
  native/analytics.cpp
  native/commands_backtest.cpp
  native/commands_factor.cpp
  native/commands_node_data_tasks.cpp
  native/commands_history_usage.cpp
  native/commands_connections.cpp
  native/data_credentials.cpp
  native/keychain_store.cpp
  native/ctp_connections.cpp
  native/commands_market.cpp
  native/trading_client.cpp
  native/node_client.cpp
  native/local_manager.cpp
)
target_compile_definitions(asterion_terminal_application PRIVATE ASTERION_PRODUCT_VERSION="${PROJECT_VERSION}")
target_include_directories(asterion_terminal_application PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/native)
target_link_libraries(asterion_terminal_application PUBLIC asterion_node_control asterion_chart_indicators asterion_node_firewall asterion_protocol PRIVATE asterion_performance asterion_data_client asterion_task_store asterion_sqlite nlohmann_json::nlohmann_json OpenSSL::Crypto)
add_dependencies(asterion_terminal_application asterion_node_manager asterion_trading asterion_node_agent asterion_market_data)
if(APPLE)
  # Kept free of project libraries so its code signature, which the keychain
  # uses to grant access, only changes when this file does.
  add_executable(asterion_keychain native/keychain/main.cpp)
  target_link_libraries(asterion_keychain PRIVATE "-framework Security" "-framework CoreFoundation")
else()
  # The desktop's libsecret is loaded when the helper runs, not linked.
  add_executable(asterion_keychain native/keychain/secret_service.cpp)
  target_compile_features(asterion_keychain PRIVATE cxx_std_20)
  target_link_libraries(asterion_keychain PRIVATE ${CMAKE_DL_LIBS})
endif()
add_dependencies(asterion_terminal_application asterion_keychain)
add_executable(asterion_terminal_dev_bridge dev/core_bridge.cpp)
target_link_libraries(asterion_terminal_dev_bridge PRIVATE asterion_terminal_api CLI11::CLI11)
add_dependencies(asterion_terminal_application asterion_data_service asterion_task_service)
asterion_target_defaults(asterion_terminal_application)
asterion_target_defaults(asterion_terminal_dev_bridge)
