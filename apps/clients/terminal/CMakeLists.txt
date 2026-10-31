# Saved data-source and CTP settings with their keychain secrets. The
# installer links it too, to erase those secrets when it removes user data.
add_library(asterion_terminal_credentials STATIC
  native/data_credentials.cpp
  native/keychain_store.cpp
  native/ctp_connections.cpp
  native/trading_connections.cpp
  native/recorder_endpoint.cpp
  native/strategy_repository.cpp
)
target_include_directories(asterion_terminal_credentials PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/native)
target_link_libraries(asterion_terminal_credentials PUBLIC asterion_node_control)
add_dependencies(asterion_terminal_credentials asterion_keychain)
asterion_target_defaults(asterion_terminal_credentials)

add_library(asterion_terminal_application STATIC
  native/market_snapshot.cpp
  native/market_client.cpp
  native/research_client.cpp
  native/data_client.cpp
  native/terminal_application.cpp
  native/commands_live.cpp
  native/commands_node.cpp
  native/commands_backup.cpp
  native/commands_data.cpp
  native/commands_task.cpp
  native/analytics.cpp
  native/commands_backtest.cpp
  native/commands_factor.cpp
  native/commands_node_workspaces.cpp
  native/commands_history_usage.cpp
  native/commands_connections.cpp
  native/commands_market.cpp
  native/commands_lab.cpp
  native/trading_client.cpp
  native/local_services.cpp
  native/local_service_health.cpp
)
target_compile_definitions(asterion_terminal_application PRIVATE ASTERION_PRODUCT_VERSION="${PROJECT_VERSION}")
target_include_directories(asterion_terminal_application PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/native)
target_link_libraries(asterion_terminal_application PUBLIC asterion_terminal_credentials asterion_node_control asterion_chart_indicators asterion_protocol PRIVATE asterion_performance asterion_data_client nlohmann_json::nlohmann_json OpenSSL::Crypto)
add_dependencies(asterion_terminal_application asterion_market)
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
add_executable(asterion_terminal_dev_bridge dev/core_bridge.cpp)
target_link_libraries(asterion_terminal_dev_bridge PRIVATE asterion_terminal_api CLI11::CLI11)
add_dependencies(asterion_terminal_application asterion_data asterion_research)
asterion_target_defaults(asterion_terminal_application)
asterion_target_defaults(asterion_terminal_dev_bridge)

add_library(asterion_domain_backup STATIC native/domain_backup.cpp)
target_include_directories(asterion_domain_backup PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/native)
target_link_libraries(asterion_domain_backup PUBLIC asterion_backup_files PRIVATE asterion_data_backup asterion_research_backup)
asterion_target_defaults(asterion_domain_backup)

target_link_libraries(asterion_terminal_application PRIVATE asterion_domain_backup)
