# Tests, grouped by the module they protect. A fixture lives with the module
# that owns what it stands in for; other modules include it by that path.
# The service bundle is built without tests, so nothing here is one of its
# inputs.

# Test programs sit beside the product's programs: process tests and the
# end-to-end suite look for both in the build directory.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

find_package(GTest CONFIG REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
include(GoogleTest)
# macOS limits Unix socket paths to 104 bytes and its default TMPDIR is long;
# fixtures place sockets under the temporary directory, so use the short one
# (its real path: durable files refuse the /tmp symbolic link).
set(asterion_test_environment)
if(APPLE)
  set(asterion_test_environment ENVIRONMENT "TMPDIR=/private/tmp")
endif()
# Labels: `unit` tests run in memory and finish within a second each
# (`ctest -L unit` is the quick local check); `process` tests start service
# processes, sockets or slow stores. A test program that has any such test is
# `process`; directly registered script tests below are all `process`.
set(asterion_process_test_targets
  asterion_python_runtime_tests asterion_warehouse_tests
  asterion_acquisition_tests
  asterion_ctp_catalog_tests asterion_ctp_tests asterion_ctp_trader_tests
  asterion_kernel_tests asterion_live_tests
  asterion_rpc_host_tests asterion_terminal_application_tests)
# Optional argument: a timeout in seconds.
function(asterion_unit_tests target source library)
  add_executable(${target} ${source})
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(${target} PRIVATE ${library} GTest::gtest_main)
  # Process tests take seconds alone; 60 s keeps a parallel run under load from
  # failing healthy tests while still ending a real hang.
  set(test_timeout 60)
  if(ARGC GREATER 3)
    set(test_timeout ${ARGV3})
  endif()
  if(ASTERION_SANITIZERS)
    math(EXPR test_timeout "${test_timeout} * 5")
  endif()
  # Instrumented binaries can need more than CMake's five-second default to
  # start while other targets run. This changes discovery, not case budgets.
  set(label unit)
  if(target IN_LIST asterion_process_test_targets)
    set(label process)
  endif()
  gtest_discover_tests(${target} DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 30
    PROPERTIES TIMEOUT ${test_timeout} LABELS ${label}
    ${asterion_test_environment})
endfunction()
# core: foundation, kernel, domain and protocol
asterion_unit_tests(asterion_protocol_tests core/protocol_test.cpp asterion_protocol)
target_link_libraries(asterion_protocol_tests PRIVATE Asterion::System)
asterion_unit_tests(asterion_foundation_tests core/foundation_test.cpp Asterion::Base)
target_link_libraries(asterion_foundation_tests PRIVATE Threads::Threads)
asterion_unit_tests(asterion_kernel_tests core/kernel_test.cpp Asterion::System)
asterion_unit_tests(asterion_host_resources_tests core/host_resources_test.cpp Asterion::System)
asterion_unit_tests(asterion_rpc_host_tests core/rpc_host_test.cpp Asterion::System)
target_link_libraries(asterion_rpc_host_tests PRIVATE asio::asio)
asterion_unit_tests(asterion_transport_tests core/timed_operation_test.cpp Asterion::System)
target_include_directories(asterion_transport_tests PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(asterion_transport_tests PRIVATE asio::asio)
asterion_unit_tests(asterion_ipc_tests core/ipc_test.cpp Asterion::System)
asterion_unit_tests(asterion_domain_tests core/domain_test.cpp Asterion::Futures)
asterion_unit_tests(asterion_portfolio_targets_tests core/portfolio_targets_test.cpp Asterion::Futures)
asterion_unit_tests(asterion_replay_tests core/replay_test.cpp Asterion::Futures)
asterion_unit_tests(asterion_logging_tests core/logger_test.cpp Asterion::System)
target_link_libraries(asterion_logging_tests PRIVATE spdlog::spdlog_header_only asio::asio OpenSSL::SSL)

# market: CTP feed and catalog, the market service, the market SDK double
asterion_unit_tests(asterion_strategy_bars_tests trading/strategy_bars_test.cpp asterion_trading_session)
add_library(asterion_test_ctp SHARED market/ctp_fake_sdk.cpp)
target_include_directories(asterion_test_ctp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(asterion_test_ctp PRIVATE ctp::ctp Threads::Threads)
target_compile_features(asterion_test_ctp PRIVATE cxx_std_20)
add_executable(asterion_test_market_remote market/market_remote_client.cpp)
target_include_directories(asterion_test_market_remote PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(asterion_test_market_remote PRIVATE ${CMAKE_SOURCE_DIR}/apps/clients/terminal/native)
target_link_libraries(asterion_test_market_remote PRIVATE asterion_terminal_application)
add_test(NAME market_cli_contract COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/market/cli_contract.py $<TARGET_FILE:asterion_market>)
set_tests_properties(market_cli_contract PROPERTIES TIMEOUT 45)

add_test(NAME remote_market COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/market/remote_market.py ${CMAKE_BINARY_DIR})
set_tests_properties(remote_market PROPERTIES TIMEOUT 30)
add_test(NAME market_recording_process COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/market/recording_process.py $<TARGET_FILE:asterion_market> $<TARGET_FILE:asterion_test_ctp> $<TARGET_FILE:asterion_test_ctp_trader>)
set_tests_properties(market_recording_process PROPERTIES TIMEOUT 90)
asterion_unit_tests(asterion_ctp_tests market/ctp_test.cpp asterion_ctp)
asterion_unit_tests(asterion_warehouse_tests market/warehouse_test.cpp asterion_market_warehouse)
target_link_libraries(asterion_warehouse_tests PRIVATE asterion_bar_parquet duckdb::duckdb)
target_compile_definitions(asterion_warehouse_tests PRIVATE
  ASTERION_TEST_CTP="$<TARGET_FILE:asterion_test_ctp>"
  ASTERION_TEST_CTP_TRADER="$<TARGET_FILE:asterion_test_ctp_trader>"
  ASTERION_MARKET_PATH="$<TARGET_FILE:asterion_market>")
add_dependencies(asterion_warehouse_tests asterion_test_ctp asterion_test_ctp_trader asterion_market)
asterion_unit_tests(asterion_acquisition_tests market/acquisition_test.cpp asterion_market_acquisition)
target_sources(asterion_acquisition_tests PRIVATE market/exchange_source_test.cpp market/recorded_source_test.cpp)
target_compile_definitions(asterion_acquisition_tests PRIVATE ASTERION_EXCHANGE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/market/fixtures")
target_compile_definitions(asterion_acquisition_tests PRIVATE
  ASTERION_TEST_CTP="$<TARGET_FILE:asterion_test_ctp>"
  ASTERION_TEST_CTP_TRADER="$<TARGET_FILE:asterion_test_ctp_trader>"
  ASTERION_MARKET_PATH="$<TARGET_FILE:asterion_market>")
add_dependencies(asterion_acquisition_tests asterion_test_ctp asterion_test_ctp_trader asterion_market)
asterion_unit_tests(asterion_market_recording_tests market/recording_test.cpp asterion_market_recording)
target_sources(asterion_market_recording_tests PRIVATE market/settings_test.cpp market/compaction_test.cpp)
target_include_directories(asterion_market_recording_tests PRIVATE ${CMAKE_SOURCE_DIR}/apps/services/market/recording ${CMAKE_SOURCE_DIR}/apps/services/market)
target_link_libraries(asterion_market_recording_tests PRIVATE asterion_market_settings)
# Market installs and runs on Linux hosts only; so do its setup tests.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_test(NAME market_setup COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/market/setup_test.py ${CMAKE_SOURCE_DIR}/apps/services/market/scripts/setup.sh)
  set_tests_properties(market_setup PROPERTIES SKIP_RETURN_CODE 77)
endif()
asterion_unit_tests(asterion_ctp_catalog_tests market/ctp_catalog_test.cpp asterion_ctp)
target_compile_definitions(asterion_ctp_catalog_tests PRIVATE ASTERION_TEST_CTP_TRADER="$<TARGET_FILE:asterion_test_ctp_trader>")
target_compile_definitions(asterion_ctp_catalog_tests PRIVATE ASTERION_TEST_CTP="$<TARGET_FILE:asterion_test_ctp>" ASTERION_MARKET_PATH="$<TARGET_FILE:asterion_market>")
target_link_libraries(asterion_ctp_catalog_tests PRIVATE asterion_protocol)
add_dependencies(asterion_ctp_catalog_tests asterion_test_ctp_trader asterion_test_ctp asterion_market)
target_link_libraries(asterion_ctp_tests PRIVATE asterion_protocol)
target_compile_definitions(asterion_ctp_tests PRIVATE ASTERION_TEST_CTP_TRADER="$<TARGET_FILE:asterion_test_ctp_trader>" ASTERION_TEST_CTP="$<TARGET_FILE:asterion_test_ctp>" ASTERION_MARKET_PATH="$<TARGET_FILE:asterion_market>")
add_dependencies(asterion_ctp_tests asterion_test_ctp asterion_test_ctp_trader asterion_market)
add_test(NAME market_process COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/support/isolated_node.py ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/market/recording_process.py $<TARGET_FILE:asterion_market> $<TARGET_FILE:asterion_test_ctp> $<TARGET_FILE:asterion_test_ctp_trader> --terminal ${CMAKE_BINARY_DIR})
set_tests_properties(market_process PROPERTIES TIMEOUT 60)
add_test(NAME ctp_sdk_smoke COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/support/isolated_node.py ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/market/ctp_sdk_smoke.py ${CMAKE_BINARY_DIR})
set_tests_properties(ctp_sdk_smoke PROPERTIES TIMEOUT 30)
asterion_unit_tests(asterion_intraday_tests market/intraday_bars_test.cpp asterion_protocol)

# trading: live sessions, the CTP trader, risk limits, the trading service, the trader SDK double
add_test(NAME acceptance_dry_run COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/trading/acceptance_dry_run.py ${CMAKE_BINARY_DIR} $<TARGET_FILE:asterion_test_ctp>)
set_tests_properties(acceptance_dry_run PROPERTIES TIMEOUT 150)
add_test(NAME trading_cli_contract COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/trading/cli_contract.py $<TARGET_FILE:asterion_trading>)
set_tests_properties(trading_cli_contract PROPERTIES TIMEOUT 45)

# Trading installs on Linux hosts only; its setup script uses their tools.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_test(NAME trading_installation COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/trading/trading_installation.py $<TARGET_FILE:asterion_trading> $<TARGET_FILE:asterion_test_certificates> $<TARGET_FILE:asterion_test_ctp_trader>)
  set_tests_properties(trading_installation PROPERTIES TIMEOUT 30)
endif()
add_test(NAME trading_setup COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/trading/setup_test.py ${CMAKE_SOURCE_DIR}/apps/services/trading/scripts/setup.sh)
set_tests_properties(trading_setup PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
add_library(asterion_test_ctp_trader SHARED trading/ctp_fake_trader.cpp)
target_include_directories(asterion_test_ctp_trader PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(asterion_test_ctp_trader PRIVATE ctp::ctp Threads::Threads)
target_compile_features(asterion_test_ctp_trader PRIVATE cxx_std_20)
# Each test process owns its SDK double and query throttle.
asterion_unit_tests(asterion_ctp_trader_tests trading/ctp_trader_test.cpp asterion_ctp_trader)
target_include_directories(asterion_ctp_trader_tests PRIVATE ${CMAKE_SOURCE_DIR}/apps/services/common/market/ctp)
target_link_libraries(asterion_ctp_trader_tests PRIVATE ctp::ctp)
target_compile_definitions(asterion_ctp_trader_tests PRIVATE ASTERION_TEST_CTP_TRADER="$<TARGET_FILE:asterion_test_ctp_trader>")
add_dependencies(asterion_ctp_trader_tests asterion_test_ctp_trader)
add_test(NAME live_process COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/support/isolated_node.py ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/trading/live_process.py ${CMAKE_BINARY_DIR})
set_tests_properties(live_process PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
add_test(NAME trading_remote_instances COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/support/isolated_node.py ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/trading/remote_instances.py ${CMAKE_BINARY_DIR})
set_tests_properties(trading_remote_instances PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
asterion_unit_tests(asterion_order_limits_tests trading/order_limits_test.cpp Asterion::Futures)
asterion_unit_tests(asterion_strategy_orders_tests trading/strategy_orders_test.cpp asterion_trading_session)
asterion_unit_tests(asterion_live_tests trading/live_session_test.cpp asterion_trading_session 120)
target_include_directories(asterion_live_tests PRIVATE ${CMAKE_SOURCE_DIR}/apps/services/common/market/ctp)
target_link_libraries(asterion_live_tests PRIVATE Asterion::Futures asterion_sqlite)
target_compile_definitions(asterion_live_tests PRIVATE ASTERION_TEST_CTP_TRADER="$<TARGET_FILE:asterion_test_ctp_trader>")
add_dependencies(asterion_live_tests asterion_test_ctp_trader)

# lab: the Python research service under the Terminal, with the prepared runtime.
add_test(NAME lab_strategy COMMAND ${PROJECT_SOURCE_DIR}/build/electron-resources/python/bin/python3 -I -B ${CMAKE_CURRENT_SOURCE_DIR}/lab/strategy_test.py ${PROJECT_SOURCE_DIR}/apps/services/lab ${CMAKE_BINARY_DIR}/generated-python)
set_tests_properties(lab_strategy PROPERTIES TIMEOUT 60)
if(ASTERION_PYTHON_HOME)
  add_test(NAME lab_engine COMMAND ${PROJECT_SOURCE_DIR}/build/electron-resources/python/bin/python3 -I -B ${CMAKE_CURRENT_SOURCE_DIR}/lab/engine_test.py ${CMAKE_BINARY_DIR}/engine-python)
  set_tests_properties(lab_engine PROPERTIES TIMEOUT 60)
  add_test(NAME lab_backtest COMMAND ${PROJECT_SOURCE_DIR}/build/electron-resources/python/bin/python3 -I -B ${CMAKE_CURRENT_SOURCE_DIR}/lab/backtest_test.py ${PROJECT_SOURCE_DIR}/apps/services/lab ${CMAKE_BINARY_DIR}/generated-python ${CMAKE_BINARY_DIR}/engine-python)
  set_tests_properties(lab_backtest PROPERTIES TIMEOUT 120)
  add_test(NAME lab_factor COMMAND ${PROJECT_SOURCE_DIR}/build/electron-resources/python/bin/python3 -I -B ${CMAKE_CURRENT_SOURCE_DIR}/lab/factor_test.py ${PROJECT_SOURCE_DIR}/apps/services/lab ${CMAKE_BINARY_DIR}/generated-python ${CMAKE_BINARY_DIR}/engine-python)
  set_tests_properties(lab_factor PROPERTIES TIMEOUT 120)
  add_test(NAME lab_market COMMAND ${PROJECT_SOURCE_DIR}/build/electron-resources/python/bin/python3 -I -B ${CMAKE_CURRENT_SOURCE_DIR}/lab/market_test.py ${PROJECT_SOURCE_DIR} ${CMAKE_BINARY_DIR})
  set_tests_properties(lab_market PROPERTIES TIMEOUT 120)
  add_test(NAME market_concurrency COMMAND ${PROJECT_SOURCE_DIR}/build/electron-resources/python/bin/python3 -I -B ${CMAKE_CURRENT_SOURCE_DIR}/market/concurrency_process.py ${PROJECT_SOURCE_DIR} ${CMAKE_BINARY_DIR})
  set_tests_properties(market_concurrency PROPERTIES TIMEOUT 90)
  add_test(NAME strategy_process COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/support/isolated_node.py ${PROJECT_SOURCE_DIR}/build/electron-resources/python/bin/python3 -I -B ${CMAKE_CURRENT_SOURCE_DIR}/trading/strategy_process.py ${PROJECT_SOURCE_DIR} ${CMAKE_BINARY_DIR})
  set_tests_properties(strategy_process PROPERTIES TIMEOUT 90)
  add_test(NAME strategy_stream_process COMMAND ${PROJECT_SOURCE_DIR}/build/electron-resources/python/bin/python3 -I -B ${CMAKE_CURRENT_SOURCE_DIR}/trading/strategy_stream_process.py ${PROJECT_SOURCE_DIR} ${CMAKE_BINARY_DIR})
  # Start the real-clock minute stream first so other tests fill its waits.
  set_tests_properties(strategy_stream_process PROPERTIES TIMEOUT 360 COST 240)
endif()
add_test(NAME lab_log COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/lab/log_test.py ${PROJECT_SOURCE_DIR}/apps/services/lab)
add_test(NAME lab_sessions COMMAND ${PROJECT_SOURCE_DIR}/build/electron-resources/python/bin/python3 -I -B ${CMAKE_CURRENT_SOURCE_DIR}/lab/sessions_test.py ${PROJECT_SOURCE_DIR}/apps/services/lab)
add_test(NAME lab_dominant COMMAND ${PROJECT_SOURCE_DIR}/build/electron-resources/python/bin/python3 -I -B ${CMAKE_CURRENT_SOURCE_DIR}/lab/dominant_test.py ${PROJECT_SOURCE_DIR}/apps/services/lab)
add_test(NAME lab_service COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/support/isolated_node.py ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/lab/lab_service.py $<TARGET_FILE:asterion_terminal_dev_bridge>)
set_tests_properties(lab_service PROPERTIES TIMEOUT 90
  ENVIRONMENT "ASTERION_PYTHON=${PROJECT_SOURCE_DIR}/build/electron-resources/python/bin/python3")
asterion_unit_tests(asterion_performance_tests core/performance_test.cpp Asterion::Futures)

add_executable(asterion_test_certificates support/tls_certificates.cpp)
target_include_directories(asterion_test_certificates PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(asterion_test_certificates PRIVATE cxx_std_20)
target_link_libraries(asterion_test_certificates PRIVATE asterion_system)

# terminal: the Terminal's native layer — commands and credentials
add_test(NAME terminal_cli_contract COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/terminal/cli_contract.py $<TARGET_FILE:asterion_terminal_dev_bridge>)
set_tests_properties(terminal_cli_contract PROPERTIES TIMEOUT 45)

asterion_unit_tests(asterion_python_runtime_tests terminal/python_runtime_test.cpp asterion_terminal_application 900)
asterion_unit_tests(asterion_terminal_application_tests terminal/terminal_application_test.cpp asterion_terminal_application)
target_compile_definitions(asterion_terminal_application_tests PRIVATE
  ASTERION_TEST_CTP="$<TARGET_FILE:asterion_test_ctp>"
  ASTERION_TEST_CTP_TRADER="$<TARGET_FILE:asterion_test_ctp_trader>"
  ASTERION_MARKET_PATH="$<TARGET_FILE:asterion_market>")
add_dependencies(asterion_terminal_application_tests asterion_market asterion_test_ctp asterion_test_ctp_trader)
target_link_libraries(asterion_terminal_application_tests PRIVATE asio::asio spdlog::spdlog_header_only)
# desktop: the Electron host
add_test(NAME desktop_python_startup COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/desktop/python_runtime.py)
find_program(ASTERION_TEST_NODE node REQUIRED)
add_test(NAME lab_channel COMMAND "${ASTERION_TEST_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/desktop/lab_channel.cjs")
if(ASTERION_NODE_HEADERS)
  add_library(asterion_node_bridge_fixture MODULE ${CMAKE_SOURCE_DIR}/apps/clients/terminal/bindings/node/terminal.cpp desktop/node_bridge_fixture.cpp ${CMAKE_SOURCE_DIR}/apps/clients/terminal/native/service_io.cpp)
  target_include_directories(asterion_node_bridge_fixture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_features(asterion_node_bridge_fixture PRIVATE cxx_std_20)
  target_compile_definitions(asterion_node_bridge_fixture PRIVATE NAPI_VERSION=8)
  target_include_directories(asterion_node_bridge_fixture PRIVATE "${ASTERION_NODE_HEADERS}" ${CMAKE_SOURCE_DIR}/apps/clients/terminal/native)
  target_link_libraries(asterion_node_bridge_fixture PRIVATE asterion_system)
  if(APPLE)
    target_link_options(asterion_node_bridge_fixture PRIVATE "-undefined" "dynamic_lookup")
  endif()
  set_target_properties(asterion_node_bridge_fixture PROPERTIES PREFIX "" SUFFIX ".node")
  add_test(NAME node_bridge_lifecycle COMMAND "${ASTERION_TEST_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/desktop/electron_bridge_lifecycle.cjs" $<TARGET_FILE:asterion_node_bridge_fixture>)
  set_tests_properties(node_bridge_lifecycle PROPERTIES TIMEOUT 20)
endif()
add_test(NAME macos_release_guards COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/desktop/macos_release.py)

get_property(asterion_tests DIRECTORY PROPERTY TESTS)
# Instrumented builds run several times slower: scale every explicit limit.
if(ASTERION_SANITIZERS)
  foreach(test IN LISTS asterion_tests)
    get_test_property(${test} TIMEOUT limit)
    if(limit)
      math(EXPR limit "${limit} * 5")
      set_tests_properties(${test} PROPERTIES TIMEOUT ${limit})
    endif()
  endforeach()
endif()



# Applied last so every directly registered test receives them.
get_property(asterion_tests DIRECTORY PROPERTY TESTS)
set_property(TEST ${asterion_tests} PROPERTY LABELS process)
if(APPLE)
  set_property(TEST ${asterion_tests} APPEND PROPERTY ENVIRONMENT "TMPDIR=/private/tmp")
endif()
