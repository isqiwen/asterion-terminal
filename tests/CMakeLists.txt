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
  asterion_account_review_store_tests asterion_ctp_catalog_tests asterion_ctp_tests
  asterion_ctp_trader_tests asterion_data_backup_tests asterion_data_credential_tests
  asterion_data_tests asterion_dominant_series_tests asterion_kernel_tests asterion_live_tests
  asterion_logic_store_tests asterion_market_snapshot_tests asterion_material_lifecycle_tests
  asterion_material_process_tests asterion_node_maintenance_tests asterion_node_supervision_tests
  asterion_paper_tests asterion_portfolio_tests asterion_quantstats_tests
  asterion_research_backup_tests asterion_research_evaluation_tests asterion_research_sample_tests
  asterion_research_statistics_tests asterion_rpc_host_tests asterion_shfe_margin_tests
  asterion_strategy_store_tests asterion_task_execution_tests asterion_terminal_query_tests
  asterion_trading_backup_tests asterion_trading_process_tests
  asterion_tushare_tests)
# Optional arguments: a timeout in seconds, then SERIAL for tests whose
# timing depends on the machine not being shared with other tests.
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
  set(run_serial FALSE)
  if(ARGC GREATER 4 AND "${ARGV4}" STREQUAL "SERIAL")
    set(run_serial TRUE)
  endif()
  # Instrumented binaries can need more than CMake's five-second default to
  # start while other targets run. This changes discovery, not case budgets.
  set(label unit)
  if(target IN_LIST asterion_process_test_targets)
    set(label process)
  endif()
  gtest_discover_tests(${target} DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 30
    PROPERTIES TIMEOUT ${test_timeout} RUN_SERIAL ${run_serial} LABELS ${label}
    ${asterion_test_environment})
endfunction()
enable_language(C)
# In this tree the sample plugin builds against this checkout's SDK headers.
set(ASTERION_PLUGIN_SDK "${CMAKE_SOURCE_DIR}/bindings/plugin/include")
add_subdirectory(support/native-plugin)

# core: foundation, kernel, domain and protocol
asterion_unit_tests(asterion_protocol_tests core/protocol_test.cpp asterion_protocol)
target_link_libraries(asterion_protocol_tests PRIVATE Asterion::Kernel)
asterion_unit_tests(asterion_foundation_tests core/foundation_test.cpp Asterion::Foundation)
target_link_libraries(asterion_foundation_tests PRIVATE Threads::Threads)
asterion_unit_tests(asterion_kernel_tests core/kernel_test.cpp Asterion::Kernel)
asterion_unit_tests(asterion_host_resources_tests core/host_resources_test.cpp Asterion::Kernel)
asterion_unit_tests(asterion_rpc_host_tests core/rpc_host_test.cpp Asterion::Kernel)
target_link_libraries(asterion_rpc_host_tests PRIVATE asio::asio)
target_compile_definitions(asterion_rpc_host_tests PRIVATE ASTERION_CERTIFICATES="$<TARGET_FILE:asterion_test_certificates>")
add_dependencies(asterion_rpc_host_tests asterion_test_certificates)
asterion_unit_tests(asterion_transport_tests core/timed_operation_test.cpp Asterion::Kernel)
target_include_directories(asterion_transport_tests PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(asterion_transport_tests PRIVATE asio::asio)
asterion_unit_tests(asterion_ipc_tests core/ipc_test.cpp Asterion::Kernel)
asterion_unit_tests(asterion_native_plugin_tests core/native_plugin_test.cpp Asterion::Kernel)
target_compile_definitions(asterion_native_plugin_tests PRIVATE
  PLUGIN_GOOD="$<TARGET_FILE:plugin_fixture_good>" PLUGIN_BAD_ABI="$<TARGET_FILE:plugin_fixture_bad_abi>"
  PLUGIN_MISSING_ENTRY="$<TARGET_FILE:plugin_fixture_missing_entry>" PLUGIN_EMPTY_TABLE="$<TARGET_FILE:plugin_fixture_empty_table>"
  PLUGIN_TUSHARE="$<TARGET_FILE:asterion_tushare_plugin>")
add_dependencies(asterion_native_plugin_tests plugin_fixture_good plugin_fixture_bad_abi plugin_fixture_missing_entry plugin_fixture_empty_table asterion_tushare_plugin)
asterion_unit_tests(asterion_domain_tests core/domain_test.cpp Asterion::Domain)
asterion_unit_tests(asterion_logging_tests core/logger_test.cpp Asterion::Kernel)
target_link_libraries(asterion_logging_tests PRIVATE spdlog::spdlog_header_only asio::asio OpenSSL::SSL)

# market: CTP feed and catalog, the market service, the market SDK double
add_library(asterion_test_ctp SHARED market/ctp_fake_sdk.cpp)
target_include_directories(asterion_test_ctp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(asterion_test_ctp PRIVATE ctp::ctp Threads::Threads)
target_compile_features(asterion_test_ctp PRIVATE cxx_std_20)
add_executable(asterion_test_market_remote market/market_remote_client.cpp)
target_include_directories(asterion_test_market_remote PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(asterion_test_market_remote PRIVATE ${CMAKE_SOURCE_DIR}/apps/clients/terminal/native)
target_link_libraries(asterion_test_market_remote PRIVATE asterion_terminal_api)
add_test(NAME remote_market COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/market/remote_market.py ${CMAKE_BINARY_DIR})
set_tests_properties(remote_market PROPERTIES TIMEOUT 30)
asterion_unit_tests(asterion_ctp_tests market/ctp_test.cpp asterion_ctp)
asterion_unit_tests(asterion_recorder_tests recorder/recorder_test.cpp asterion_bar_parquet)
target_sources(asterion_recorder_tests PRIVATE recorder/login_test.cpp recorder/compaction_test.cpp ${CMAKE_SOURCE_DIR}/apps/services/recorder/recorder.cpp ${CMAKE_SOURCE_DIR}/apps/services/recorder/login.cpp ${CMAKE_SOURCE_DIR}/apps/services/recorder/compaction.cpp ${CMAKE_SOURCE_DIR}/apps/services/recorder/database.cpp)
target_include_directories(asterion_recorder_tests PRIVATE ${CMAKE_SOURCE_DIR}/apps/services/recorder)
target_link_libraries(asterion_recorder_tests PRIVATE asterion_ctp duckdb::duckdb)
add_test(NAME recorder_process COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/recorder/recorder_process.py $<TARGET_FILE:asterion_recorder> $<TARGET_FILE:asterion_test_ctp> $<TARGET_FILE:asterion_test_ctp_trader> ${CMAKE_BINARY_DIR}/generated-python)
set_tests_properties(recorder_process PROPERTIES TIMEOUT 90)
asterion_unit_tests(asterion_market_snapshot_tests market/market_snapshot_test.cpp asterion_protocol)
target_sources(asterion_market_snapshot_tests PRIVATE ${CMAKE_SOURCE_DIR}/apps/clients/terminal/native/market_snapshot.cpp)
asterion_unit_tests(asterion_ctp_catalog_tests market/ctp_catalog_test.cpp asterion_ctp)
target_compile_definitions(asterion_ctp_catalog_tests PRIVATE ASTERION_TEST_CTP_TRADER="$<TARGET_FILE:asterion_test_ctp_trader>")
target_compile_definitions(asterion_ctp_catalog_tests PRIVATE ASTERION_TEST_CTP="$<TARGET_FILE:asterion_test_ctp>" ASTERION_MARKET_PATH="$<TARGET_FILE:asterion_market>")
target_link_libraries(asterion_ctp_catalog_tests PRIVATE asterion_protocol)
add_dependencies(asterion_ctp_catalog_tests asterion_test_ctp_trader asterion_test_ctp asterion_market)
target_link_libraries(asterion_ctp_tests PRIVATE asterion_protocol)
target_compile_definitions(asterion_ctp_tests PRIVATE ASTERION_TEST_CTP="$<TARGET_FILE:asterion_test_ctp>" ASTERION_MARKET_PATH="$<TARGET_FILE:asterion_market>")
add_dependencies(asterion_ctp_tests asterion_test_ctp asterion_market)
add_test(NAME market_process COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/support/isolated_node.py ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/market/market_process.py ${CMAKE_BINARY_DIR})
set_tests_properties(market_process PROPERTIES TIMEOUT 60)
add_test(NAME market_observation_archive COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/support/isolated_node.py ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/market/observation_archive.py ${CMAKE_BINARY_DIR})
set_tests_properties(market_observation_archive PROPERTIES TIMEOUT 60)
add_test(NAME ctp_sdk_smoke COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/support/isolated_node.py ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/market/ctp_sdk_smoke.py ${CMAKE_BINARY_DIR})
set_tests_properties(ctp_sdk_smoke PROPERTIES TIMEOUT 30)
asterion_unit_tests(asterion_intraday_tests market/intraday_bars_test.cpp asterion_protocol)

# trading: live sessions, the CTP trader, risk limits, the trading service, the trader SDK double
add_test(NAME acceptance_dry_run COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/trading/acceptance_dry_run.py ${CMAKE_BINARY_DIR} $<TARGET_FILE:asterion_test_ctp>)
set_tests_properties(acceptance_dry_run PROPERTIES TIMEOUT 150)
add_test(NAME trading_tls_admission COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/trading/trading_tls_admission.py $<TARGET_FILE:asterion_trading> $<TARGET_FILE:asterion_test_certificates> $<TARGET_FILE:asterion_test_ctp_trader>)
set_tests_properties(trading_tls_admission PROPERTIES TIMEOUT 30)
add_library(asterion_test_ctp_trader SHARED trading/ctp_fake_trader.cpp)
target_include_directories(asterion_test_ctp_trader PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(asterion_test_ctp_trader PRIVATE ctp::ctp Threads::Threads)
target_compile_features(asterion_test_ctp_trader PRIVATE cxx_std_20)
# The SDK double enforces CTP's one-query-per-second limit in wall-clock time.
asterion_unit_tests(asterion_ctp_trader_tests trading/ctp_trader_test.cpp asterion_ctp_trader 60 SERIAL)
target_include_directories(asterion_ctp_trader_tests PRIVATE ${CMAKE_SOURCE_DIR}/plugins/data/ctp)
target_link_libraries(asterion_ctp_trader_tests PRIVATE ctp::ctp)
target_compile_definitions(asterion_ctp_trader_tests PRIVATE ASTERION_TEST_CTP_TRADER="$<TARGET_FILE:asterion_test_ctp_trader>")
add_dependencies(asterion_ctp_trader_tests asterion_test_ctp_trader)
add_test(NAME live_process COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/support/isolated_node.py ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/trading/live_process.py ${CMAKE_BINARY_DIR})
set_tests_properties(live_process PROPERTIES TIMEOUT 120)
add_test(NAME trading_remote_instances COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/support/isolated_node.py ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/trading/remote_instances.py ${CMAKE_BINARY_DIR})
set_tests_properties(trading_remote_instances PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
asterion_unit_tests(asterion_trading_process_tests trading/trading_process_test.cpp asterion_protocol)
target_link_libraries(asterion_trading_process_tests PRIVATE Asterion::Kernel asterion_trading_session)
add_dependencies(asterion_trading_process_tests asterion_trading asterion_test_ctp_trader)
target_compile_definitions(asterion_trading_process_tests PRIVATE ASTERION_TEST_CTP_TRADER="$<TARGET_FILE:asterion_test_ctp_trader>")
asterion_unit_tests(asterion_order_limits_tests trading/order_limits_test.cpp asterion_order_limits)
target_link_libraries(asterion_order_limits_tests PRIVATE asterion_risk_providers)
target_compile_definitions(asterion_order_limits_tests PRIVATE RISK_PLUGIN="$<TARGET_FILE:asterion_order_limits_plugin>" RISK_FIXTURE="$<TARGET_FILE:plugin_fixture_risk>")
add_dependencies(asterion_order_limits_tests asterion_order_limits_plugin plugin_fixture_risk)
asterion_unit_tests(asterion_material_store_tests data/material_store_test.cpp asterion_data_store)
asterion_unit_tests(asterion_material_protocol_tests data/material_protocol_test.cpp asterion_protocol)
asterion_unit_tests(asterion_account_review_protocol_tests tasks/account_review_protocol_test.cpp asterion_protocol)
asterion_unit_tests(asterion_account_review_store_tests tasks/account_review_store_test.cpp asterion_task_store)
target_link_libraries(asterion_account_review_store_tests PRIVATE asterion_account_review asterion_research_host asterion_domain_backup asterion_research_backup asterion_program_files)
target_compile_definitions(asterion_account_review_store_tests PRIVATE ASTERION_RESEARCH_PATH="$<TARGET_FILE:asterion_research>")
add_dependencies(asterion_account_review_store_tests asterion_research)
asterion_unit_tests(asterion_program_files_tests tasks/program_files_test.cpp asterion_program_files)
asterion_unit_tests(asterion_account_review_tests tasks/account_review_test.cpp asterion_account_review)
asterion_unit_tests(asterion_trading_facts_tests trading/facts_archive_test.cpp asterion_trading_session)
asterion_unit_tests(asterion_trading_backup_tests trading/backup_test.cpp asterion_trading_backup)
target_link_libraries(asterion_trading_backup_tests PRIVATE asterion_trading_session asterion_domain_backup asterion_research_host asterion_program_files)
add_dependencies(asterion_trading_backup_tests asterion_order_limits_plugin)
asterion_unit_tests(asterion_live_tests trading/live_session_test.cpp asterion_trading_session 120)
target_include_directories(asterion_live_tests PRIVATE ${CMAKE_SOURCE_DIR}/plugins/data/ctp)
target_link_libraries(asterion_live_tests PRIVATE asterion_order_limits asterion_sqlite)
target_compile_definitions(asterion_live_tests PRIVATE ASTERION_TEST_CTP_TRADER="$<TARGET_FILE:asterion_test_ctp_trader>" RISK_FIXTURE="$<TARGET_FILE:plugin_fixture_risk>")
add_dependencies(asterion_live_tests asterion_test_ctp_trader plugin_fixture_risk)

# data: history providers, storage, the data service, dataset seeding
# Test-only history fixtures used by the process and e2e tests on every platform.
add_executable(asterion_test_history data/history_dataset_fixture.cpp)
target_include_directories(asterion_test_history PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(asterion_test_history PRIVATE asterion_data_store CLI11::CLI11)
add_executable(asterion_test_minutes data/minute_dataset_fixture.cpp)
target_include_directories(asterion_test_minutes PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(asterion_test_minutes PRIVATE asterion_data_store asterion_tushare asterion_task_store CLI11::CLI11)
asterion_unit_tests(asterion_data_tests data/data_service_test.cpp "asterion_data_store;asterion_data_client;asterion_task_store;asterion_sqlite" 45)
target_compile_definitions(asterion_data_tests PRIVATE ASTERION_DATA_SERVICE_PATH="$<TARGET_FILE:asterion_data>" ASTERION_RESEARCH_PATH="$<TARGET_FILE:asterion_research>")
add_dependencies(asterion_data_tests asterion_data asterion_research)
asterion_unit_tests(asterion_native_history_plugin_tests data/native_history_plugin_test.cpp asterion_history_providers)
target_compile_definitions(asterion_native_history_plugin_tests PRIVATE PLUGIN_FIXTURE_DIRECTORY="$<TARGET_FILE_DIR:plugin_fixture_good>")
add_dependencies(asterion_native_history_plugin_tests plugin_fixture_good)
asterion_unit_tests(asterion_calendar_bar_tests data/calendar_bar_test.cpp asterion_chart_indicators)
# Crash recovery preserves the provider budget quiet minute before an explicit retry.
asterion_unit_tests(asterion_tushare_tests data/tushare_test.cpp asterion_task_store 90)
target_link_libraries(asterion_tushare_tests PRIVATE asterion_data_store asterion_data_client asterion_tushare asterion_sqlite asterion_bar_parquet asterion_factor_engine)
target_sources(asterion_tushare_tests PRIVATE data/history_archive_test.cpp data/bar_parquet_test.cpp data/history_tables_test.cpp data/tushare_tables_test.cpp data/table_update_test.cpp data/table_series_test.cpp)
target_sources(asterion_tushare_tests PRIVATE data/minute_evidence_test.cpp data/historical_sessions_test.cpp data/history_repair_gate_test.cpp)
target_compile_definitions(asterion_tushare_tests PRIVATE ASTERION_DATA_SERVICE_PATH="$<TARGET_FILE:asterion_data>" ASTERION_RESEARCH_PATH="$<TARGET_FILE:asterion_research>" PLUGIN_FIXTURE_DIRECTORY="$<TARGET_FILE_DIR:plugin_fixture_good>")
add_dependencies(asterion_tushare_tests asterion_data asterion_research plugin_fixture_good)
# This process test preserves DownloadBudget's real one-minute restart quiet period.
# Set only its budget after GoogleTest has populated the discovered cases.
set(data_refresh_recovery_timeout 90)
if(ASTERION_SANITIZERS)
  math(EXPR data_refresh_recovery_timeout "${data_refresh_recovery_timeout} * 5")
endif()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/data_refresh_recovery_timeout.cmake"
  CONTENT "if(TEST TableRefreshOwnership.DataAloneOwnsCancellationRecoveryAndRepeatedSubmission)\n  set_tests_properties(TableRefreshOwnership.DataAloneOwnsCancellationRecoveryAndRepeatedSubmission PROPERTIES TIMEOUT ${data_refresh_recovery_timeout})\nendif()\n")
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
  "${CMAKE_CURRENT_BINARY_DIR}/data_refresh_recovery_timeout.cmake")


# tasks: the task service, backtest and factor engines, workers
add_test(NAME task_agent_recovery COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/support/isolated_node.py ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tasks/task_agent.py $<TARGET_FILE:asterion_terminal_dev_bridge>)
set_tests_properties(task_agent_recovery PROPERTIES TIMEOUT 60)
# lab: the Python research service under the local Agent, with the prepared runtime.
add_test(NAME lab_warehouse COMMAND ${PROJECT_SOURCE_DIR}/build/electron-resources/python/bin/python3 -I -B ${CMAKE_CURRENT_SOURCE_DIR}/lab/warehouse_test.py ${PROJECT_SOURCE_DIR}/apps/services/lab ${CMAKE_BINARY_DIR}/generated-python)
set_tests_properties(lab_warehouse PROPERTIES TIMEOUT 60)
add_test(NAME lab_sessions COMMAND ${PROJECT_SOURCE_DIR}/build/electron-resources/python/bin/python3 -I -B ${CMAKE_CURRENT_SOURCE_DIR}/lab/sessions_test.py ${PROJECT_SOURCE_DIR}/apps/services/lab)
add_test(NAME lab_dominant COMMAND ${PROJECT_SOURCE_DIR}/build/electron-resources/python/bin/python3 -I -B ${CMAKE_CURRENT_SOURCE_DIR}/lab/dominant_test.py ${PROJECT_SOURCE_DIR}/apps/services/lab)
add_test(NAME lab_agent COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/support/isolated_node.py ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/lab/lab_agent.py $<TARGET_FILE:asterion_terminal_dev_bridge> $<TARGET_FILE:asterion_node_agent>)
set_tests_properties(lab_agent PROPERTIES TIMEOUT 90
  ENVIRONMENT "ASTERION_PYTHON=${PROJECT_SOURCE_DIR}/build/electron-resources/python/bin/python3")
foreach(mode handshake frame overload)
  add_test(NAME task_slow_${mode} COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tasks/task_concurrency.py $<TARGET_FILE:asterion_research> $<TARGET_FILE:asterion_test_certificates> ${mode})
  set_tests_properties(task_slow_${mode} PROPERTIES TIMEOUT 30)
endforeach()
add_test(NAME task_tls_admission COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tasks/task_tls_admission.py $<TARGET_FILE:asterion_research> $<TARGET_FILE:asterion_test_certificates>)
set_tests_properties(task_tls_admission PROPERTIES TIMEOUT 30)
asterion_unit_tests(asterion_performance_tests tasks/performance_test.cpp asterion_performance)
asterion_unit_tests(asterion_account_exposure_tests tasks/account_exposure_test.cpp asterion_performance)
asterion_unit_tests(asterion_static_stress_tests tasks/static_stress_test.cpp asterion_performance)
asterion_unit_tests(asterion_account_reconciliation_tests tasks/account_reconciliation_test.cpp asterion_performance)
asterion_unit_tests(asterion_portfolio_budget_tests tasks/portfolio_budget_test.cpp asterion_performance)
asterion_unit_tests(asterion_strategy_tests tasks/strategy_test.cpp asterion_strategies)
asterion_unit_tests(asterion_strategy_package_tests tasks/strategy_package_test.cpp asterion_protocol)
asterion_unit_tests(asterion_research_logic_tests tasks/research_logic_test.cpp asterion_research_logic)
asterion_unit_tests(asterion_logic_store_tests tasks/logic_store_test.cpp asterion_research_logic_store)
target_link_libraries(asterion_logic_store_tests PRIVATE asterion_task_store asterion_research_logic asterion_research_host asterion_program_files asterion_risk_providers asterion_research_strategies)
asterion_unit_tests(asterion_strategy_store_tests tasks/strategy_store_test.cpp asterion_research_strategies)
target_link_libraries(asterion_strategy_store_tests PRIVATE asterion_task_store asterion_program_files asterion_backtest_engine)
# Explicit offline profiling; timings are evidence, not flaky CTest assertions.
add_executable(asterion_factor_capacity_probe EXCLUDE_FROM_ALL tasks/factor_capacity.cpp)
target_include_directories(asterion_factor_capacity_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(asterion_factor_capacity_probe PRIVATE asterion_factor_engine asterion_factor_analysis CLI11::CLI11)
asterion_unit_tests(asterion_factor_tests tasks/factor_test.cpp asterion_factor_engine)
target_link_libraries(asterion_factor_tests PRIVATE asterion_factor_analysis asterion_sqlite asterion_task_store)
target_sources(asterion_factor_tests PRIVATE tasks/feature_expression_test.cpp tasks/descriptive_statistics_test.cpp)
asterion_unit_tests(asterion_task_execution_tests tasks/task_store_test.cpp asterion_backtest_engine 180)
target_sources(asterion_task_execution_tests PRIVATE tasks/backtest_execution_test.cpp tasks/task_process_test.cpp)
target_link_libraries(asterion_task_execution_tests PRIVATE asterion_research_backup asterion_performance asterion_data_store asterion_data_client asterion_factor_engine asterion_strategies asterion_research_host asterion_risk_providers asterion_sqlite)
target_compile_definitions(asterion_task_execution_tests PRIVATE ASTERION_DATA_SERVICE_PATH="$<TARGET_FILE:asterion_data>" ASTERION_RESEARCH_PATH="$<TARGET_FILE:asterion_research>")
add_dependencies(asterion_task_execution_tests asterion_data asterion_research)
asterion_unit_tests(asterion_dominant_series_tests tasks/dominant_series_test.cpp asterion_backtest_engine)
target_link_libraries(asterion_dominant_series_tests PRIVATE asterion_data_store asterion_factor_engine asterion_strategies asterion_task_store asterion_risk_providers asterion_sqlite)
asterion_unit_tests(asterion_paper_tests tasks/paper_test.cpp asterion_paper 180)
target_link_libraries(asterion_paper_tests PRIVATE asterion_order_limits asterion_sqlite)

# node: the Agent — supervision, deployment, upgrade, SSH hosts, TLS roles, service contracts
add_test(NAME cli_contract COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/node/cli_contract.py $<TARGET_FILE:asterion_terminal_dev_bridge> $<TARGET_FILE:asterion_trading> $<TARGET_FILE:asterion_node_agent> $<TARGET_FILE:asterion_research> $<TARGET_FILE:asterion_data> $<TARGET_FILE:asterion_node_manager>)
set_tests_properties(cli_contract PROPERTIES TIMEOUT 45)
add_executable(asterion_test_certificates node/tls_certificates.cpp)
target_include_directories(asterion_test_certificates PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(asterion_test_certificates PRIVATE cxx_std_20)
target_link_libraries(asterion_test_certificates PRIVATE OpenSSL::Crypto CLI11::CLI11)
add_test(NAME ssh_keys COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/node/ssh_keys.py $<TARGET_FILE:asterion_terminal_dev_bridge>)
add_test(NAME host_initialization COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/node/host_initialization.py)
add_test(NAME ssh_bootstrap COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/node/ssh_bootstrap.py $<TARGET_FILE:asterion_terminal_dev_bridge> $<TARGET_FILE:asterion_node_agent>)
set_tests_properties(ssh_bootstrap PROPERTIES TIMEOUT 240)
add_test(NAME node_deployment COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/node/node_deployment.py $<TARGET_FILE:asterion_terminal_dev_bridge> $<TARGET_FILE:asterion_node_agent> $<TARGET_FILE:asterion_trading> $<TARGET_FILE:asterion_test_certificates>)
add_test(NAME node_manager_lifecycle COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/node/manager_lifecycle.py $<TARGET_FILE:asterion_terminal_dev_bridge> $<TARGET_FILE:asterion_node_agent>)
set_tests_properties(node_manager_lifecycle PROPERTIES TIMEOUT 90)
set_tests_properties(node_deployment PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77)
foreach(mode handshake frame overload)
  add_test(NAME agent_slow_${mode} COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/node/agent_concurrency.py $<TARGET_FILE:asterion_node_agent> $<TARGET_FILE:asterion_test_certificates> ${mode})
  set_tests_properties(agent_slow_${mode} PROPERTIES TIMEOUT 30)
endforeach()
add_test(NAME trading_graceful_stop COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/node/service_graceful_stop.py trading $<TARGET_FILE:asterion_trading> $<TARGET_FILE:asterion_test_ctp_trader>)
add_test(NAME market_graceful_stop COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/node/service_graceful_stop.py market $<TARGET_FILE:asterion_market>)
add_test(NAME research_graceful_stop COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/node/service_graceful_stop.py research $<TARGET_FILE:asterion_research>)
add_test(NAME tls_roles COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/node/tls_roles.py $<TARGET_FILE:asterion_node_agent> $<TARGET_FILE:asterion_trading> $<TARGET_FILE:asterion_test_certificates> $<TARGET_FILE:asterion_test_ctp_trader>)
set_tests_properties(tls_roles PROPERTIES TIMEOUT 60)
add_executable(asterion_test_node_service_control node/node_service_control.cpp)
target_include_directories(asterion_test_node_service_control PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(asterion_test_node_service_control PRIVATE ${CMAKE_SOURCE_DIR}/apps/clients/terminal/native)
target_link_libraries(asterion_test_node_service_control PRIVATE asterion_terminal_api CLI11::CLI11)
asterion_unit_tests(asterion_node_maintenance_tests node/node_maintenance_test.cpp asterion_terminal_api 60)
target_link_libraries(asterion_node_maintenance_tests PRIVATE asterion_node_storage)
target_include_directories(asterion_node_maintenance_tests PRIVATE ${CMAKE_SOURCE_DIR}/apps/clients/terminal/native)
target_compile_definitions(asterion_node_maintenance_tests PRIVATE ASTERION_AGENT_PATH="$<TARGET_FILE:asterion_node_agent>" ASTERION_TRADE_PATH="$<TARGET_FILE:asterion_trading>")
target_compile_definitions(asterion_node_maintenance_tests PRIVATE ASTERION_MARKET_PATH="$<TARGET_FILE:asterion_market>")
target_compile_definitions(asterion_node_maintenance_tests PRIVATE ASTERION_FAKE_CTP="$<TARGET_FILE:asterion_test_ctp>" ASTERION_FAKE_CTP_TRADER="$<TARGET_FILE:asterion_test_ctp_trader>")
add_dependencies(asterion_node_maintenance_tests asterion_node_agent asterion_trading asterion_market asterion_test_ctp asterion_test_ctp_trader)
if(UNIX)
  asterion_unit_tests(asterion_node_supervision_tests node/node_supervision_test.cpp "asterion_terminal_api;asterion_data_client;asterion_data_store;asterion_task_store;asterion_research_host" 90)
  target_include_directories(asterion_node_supervision_tests PRIVATE ${CMAKE_SOURCE_DIR}/apps/clients/terminal/native)
  target_compile_definitions(asterion_node_supervision_tests PRIVATE ASTERION_AGENT_PATH="$<TARGET_FILE:asterion_node_agent>")
  add_dependencies(asterion_node_supervision_tests asterion_node_agent asterion_data asterion_market asterion_research)
endif()
add_executable(asterion_test_agent_revision ${CMAKE_SOURCE_DIR}/apps/management/node-agent/main.cpp node/strategy_revision_marker.cpp)
target_include_directories(asterion_test_agent_revision PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(asterion_test_agent_revision PRIVATE cxx_std_20)
target_compile_definitions(asterion_test_agent_revision PRIVATE ASTERION_PRODUCT_VERSION="${PROJECT_VERSION}")
target_link_libraries(asterion_test_agent_revision PRIVATE asterion_node_operations asterion_node_supervision asterion_node_storage asterion_node_firewall Asterion::Kernel asterion_protocol OpenSSL::Crypto CLI11::CLI11)
add_test(NAME agent_program_update COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/node/node_program_update.py $<TARGET_FILE:asterion_test_node_service_control> $<TARGET_FILE:asterion_node_agent> $<TARGET_FILE:asterion_test_agent_revision>)
set_tests_properties(agent_program_update PROPERTIES TIMEOUT 60)
asterion_unit_tests(asterion_firewall_tests node/firewall_test.cpp asterion_node_firewall)
target_compile_definitions(asterion_node_maintenance_tests PRIVATE PLUGIN_GOOD="$<TARGET_FILE:plugin_fixture_good>" PLUGIN_NEWER="$<TARGET_FILE:plugin_fixture_newer>" PLUGIN_CUSTOM="$<TARGET_FILE:plugin_fixture_risk>")
add_dependencies(asterion_node_maintenance_tests plugin_fixture_good plugin_fixture_newer plugin_fixture_risk)

# terminal: the Terminal's native layer — commands, credentials, plugin management
asterion_unit_tests(asterion_terminal_query_tests terminal/terminal_minute_query_test.cpp asterion_terminal_api)
asterion_unit_tests(asterion_data_credential_tests terminal/data_credentials_test.cpp asterion_terminal_api)
asterion_unit_tests(asterion_ctp_connection_tests terminal/ctp_connections_test.cpp asterion_terminal_api)
target_include_directories(asterion_data_credential_tests PRIVATE ${CMAKE_SOURCE_DIR}/apps/clients/terminal/native)
add_executable(asterion_test_keychain_helper terminal/keychain_helper_fixture.cpp)
target_include_directories(asterion_test_keychain_helper PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(asterion_test_keychain_helper PRIVATE cxx_std_20)
target_compile_definitions(asterion_data_credential_tests PRIVATE ASTERION_TEST_KEYCHAIN_HELPER="$<TARGET_FILE:asterion_test_keychain_helper>")
add_dependencies(asterion_data_credential_tests asterion_test_keychain_helper)
target_include_directories(asterion_ctp_connection_tests PRIVATE ${CMAKE_SOURCE_DIR}/apps/clients/terminal/native)
target_link_libraries(asterion_terminal_query_tests PRIVATE asterion_node_management asterion_strategies spdlog::spdlog_header_only)
add_executable(asterion_terminal_tests terminal/terminal_api_test.cpp)
target_include_directories(asterion_terminal_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(asterion_terminal_tests PRIVATE asterion_data_store asterion_task_store asterion_terminal_api GTest::gtest)
# One CTest per case, each with its own isolated Agent.
set(asterion_terminal_test_timeout 90)
if(ASTERION_SANITIZERS)
  # Discovered tests are registered at test time, after the directory-level
  # timeout loop below. Apply the same instrumentation multiplier here.
  math(EXPR asterion_terminal_test_timeout "${asterion_terminal_test_timeout} * 5")
endif()
gtest_discover_tests(asterion_terminal_tests EXTRA_ARGS --isolated-node DISCOVERY_MODE PRE_TEST
  DISCOVERY_TIMEOUT 30
  PROPERTIES TIMEOUT ${asterion_terminal_test_timeout} LABELS process ${asterion_test_environment})
asterion_unit_tests(asterion_plugin_management_tests terminal/plugin_management_test.cpp asterion_terminal_api)
target_link_libraries(asterion_plugin_management_tests PRIVATE asterion_node_storage)
target_include_directories(asterion_plugin_management_tests PRIVATE ${CMAKE_SOURCE_DIR}/apps/clients/terminal/native)
target_compile_definitions(asterion_plugin_management_tests PRIVATE NODE_AGENT="$<TARGET_FILE:asterion_node_agent>" PLUGIN_NEWER="$<TARGET_FILE:plugin_fixture_newer>" PLUGIN_GOOD="$<TARGET_FILE:plugin_fixture_good>" PLUGIN_BAD_ABI="$<TARGET_FILE:plugin_fixture_bad_abi>")
add_dependencies(asterion_plugin_management_tests asterion_node_agent plugin_fixture_good plugin_fixture_newer plugin_fixture_bad_abi)

# desktop: the Electron host and what an installer contains
if(ASTERION_NODE_HEADERS)
  find_program(ASTERION_TEST_NODE node REQUIRED)
  add_library(asterion_node_bridge_fixture MODULE ${CMAKE_SOURCE_DIR}/bindings/node/terminal.cpp desktop/node_bridge_fixture.cpp ${CMAKE_SOURCE_DIR}/apps/common/control/service_io.cpp)
  target_include_directories(asterion_node_bridge_fixture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_features(asterion_node_bridge_fixture PRIVATE cxx_std_20)
  target_compile_definitions(asterion_node_bridge_fixture PRIVATE NAPI_VERSION=8)
  target_include_directories(asterion_node_bridge_fixture PRIVATE ${CMAKE_SOURCE_DIR}/apps/common/control "${ASTERION_NODE_HEADERS}" ${CMAKE_SOURCE_DIR}/apps/clients/terminal/native)
  target_link_libraries(asterion_node_bridge_fixture PRIVATE asterion_kernel)
  if(APPLE)
    target_link_options(asterion_node_bridge_fixture PRIVATE "-undefined" "dynamic_lookup")
  endif()
  set_target_properties(asterion_node_bridge_fixture PROPERTIES PREFIX "" SUFFIX ".node")
  add_test(NAME node_bridge_lifecycle COMMAND "${ASTERION_TEST_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/desktop/electron_bridge_lifecycle.cjs" $<TARGET_FILE:asterion_node_bridge_fixture>)
  set_tests_properties(node_bridge_lifecycle PROPERTIES TIMEOUT 20)
endif()
add_test(NAME desktop_resources COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/desktop/desktop_resources.py)
add_test(NAME macos_release_guards COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/desktop/macos_release.py)
add_test(NAME remote_resources COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/desktop/remote_resources.py $<TARGET_FILE:asterion_terminal_dev_bridge>)

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

target_link_libraries(asterion_terminal_tests PRIVATE asterion_performance)

asterion_unit_tests(asterion_shfe_margin_tests data/shfe_margins_test.cpp asterion_data_store)
target_link_libraries(asterion_shfe_margin_tests PRIVATE asterion_shfe)
target_link_libraries(asterion_test_history PRIVATE asterion_shfe)
target_link_libraries(asterion_task_execution_tests PRIVATE asterion_shfe)
add_executable(asterion_shfe_margin_probe EXCLUDE_FROM_ALL acceptance/shfe_margin_probe.cpp)
target_include_directories(asterion_shfe_margin_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(asterion_shfe_margin_probe PRIVATE asterion_data_queries asterion_history_providers asterion_shfe CLI11::CLI11)

asterion_unit_tests(asterion_quantstats_tests terminal/quantstats_test.cpp asterion_terminal_api 90)
target_compile_definitions(asterion_quantstats_tests PRIVATE ASTERION_PYTHON_PATH="${PROJECT_SOURCE_DIR}/build/electron-resources/python/bin/python3")

asterion_unit_tests(asterion_portfolio_tests terminal/portfolio_test.cpp asterion_terminal_api 90)
target_compile_definitions(asterion_portfolio_tests PRIVATE ASTERION_PYTHON_PATH="${PROJECT_SOURCE_DIR}/build/electron-resources/python/bin/python3")

foreach(target asterion_test_node_service_control asterion_node_maintenance_tests asterion_node_supervision_tests asterion_data_credential_tests asterion_plugin_management_tests)
 target_link_libraries(${target} PRIVATE asterion_node_management)
endforeach()

asterion_unit_tests(asterion_installation_tests desktop/installation_test.cpp asterion_installation)
target_sources(asterion_installation_tests PRIVATE desktop/startup_test.cpp)

add_executable(asterion_test_manager_rpc node/manager_rpc.cpp)
target_include_directories(asterion_test_manager_rpc PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(asterion_test_manager_rpc PRIVATE asterion_protocol asterion_kernel)
add_test(NAME remote_maintenance COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/node/remote_maintenance.py $<TARGET_FILE:asterion_test_manager_rpc> $<TARGET_FILE:asterion_node_manager> $<TARGET_FILE:asterion_node_agent>)
set_tests_properties(remote_maintenance PROPERTIES TIMEOUT 240)
add_test(NAME remote_host_control COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/node/remote_host_control.py)
if(ASTERION_NODE_HEADERS)
  add_test(NAME installer_orchestration COMMAND "${ASTERION_TEST_NODE}" ${CMAKE_CURRENT_SOURCE_DIR}/desktop/installer_orchestration.cjs)
endif()

# The shared file mechanism; each domain adapter adds its own semantic validation.
asterion_unit_tests(asterion_backup_files_tests data/backup_files_test.cpp asterion_backup_files)

asterion_unit_tests(asterion_research_statistics_tests tasks/research_statistics_test.cpp asterion_research_statistics)
target_link_libraries(asterion_research_statistics_tests PRIVATE asterion_factor_engine asterion_program_files asterion_task_store)
asterion_unit_tests(asterion_research_evaluation_tests tasks/research_evaluation_test.cpp asterion_research_evaluation 90)
target_link_libraries(asterion_research_evaluation_tests PRIVATE asterion_research_host asterion_backtest_engine asterion_program_files asterion_task_store)
target_compile_definitions(asterion_research_evaluation_tests PRIVATE ASTERION_RESEARCH_PATH=\"$<TARGET_FILE:asterion_research>\")
add_dependencies(asterion_research_evaluation_tests asterion_research)
asterion_unit_tests(asterion_research_backup_tests tasks/research_backup_test.cpp asterion_research_backup)
target_link_libraries(asterion_research_backup_tests PRIVATE asterion_research_strategies asterion_risk_providers asterion_sqlite asterion_research_host asterion_domain_backup asterion_data_store asterion_data_backup asterion_backtest_engine asterion_factor_engine)
asterion_unit_tests(asterion_data_backup_tests data/data_backup_test.cpp asterion_data_backup)
target_sources(asterion_data_backup_tests PRIVATE data/resource_catalogue_test.cpp data/observation_archive_test.cpp)
target_link_libraries(asterion_data_backup_tests PRIVATE asterion_data_store asterion_task_store asterion_domain_backup)
target_compile_definitions(asterion_data_backup_tests PRIVATE PLUGIN_FIXTURE_DIRECTORY="$<TARGET_FILE_DIR:plugin_fixture_good>")
add_dependencies(asterion_data_backup_tests plugin_fixture_good)

# Synthetic provider loaded only from its own explicit test directory.
add_library(asterion_test_material_provider SHARED data/material_provider_fixture.cpp)
target_link_libraries(asterion_test_material_provider PRIVATE Asterion::Kernel)
set_target_properties(asterion_test_material_provider PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/material-fixtures" CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
asterion_unit_tests(asterion_material_provider_tests data/material_provider_test.cpp "asterion_material_providers;asterion_data_store")
target_compile_definitions(asterion_material_provider_tests PRIVATE MATERIAL_FIXTURE_DIRECTORY="$<TARGET_FILE_DIR:asterion_test_material_provider>")
add_dependencies(asterion_material_provider_tests asterion_test_material_provider)

asterion_unit_tests(asterion_material_task_tests data/material_task_test.cpp asterion_task_store)

asterion_unit_tests(asterion_material_process_tests data/material_process_test.cpp "asterion_data_store;asterion_data_client" 45)
target_compile_definitions(asterion_material_process_tests PRIVATE ASTERION_DATA_SERVICE_PATH="$<TARGET_FILE:asterion_data>" MATERIAL_FIXTURE_DIRECTORY="$<TARGET_FILE_DIR:asterion_test_material_provider>")
add_dependencies(asterion_material_process_tests asterion_data asterion_test_material_provider)

asterion_unit_tests(asterion_material_lifecycle_tests data/material_lifecycle_test.cpp "asterion_data_store;asterion_material_providers;asterion_data_backup;asterion_domain_backup")
target_compile_definitions(asterion_material_lifecycle_tests PRIVATE MATERIAL_FIXTURE_DIRECTORY="$<TARGET_FILE_DIR:asterion_test_material_provider>")
add_dependencies(asterion_material_lifecycle_tests asterion_test_material_provider)

asterion_unit_tests(asterion_research_variable_tests tasks/research_variable_test.cpp "asterion_research_variables;asterion_research_tasks")

asterion_unit_tests(asterion_research_sample_tests tasks/research_sample_test.cpp asterion_research_sample)
target_link_libraries(asterion_research_sample_tests PRIVATE asterion_factor_analysis asterion_task_store asterion_program_files asterion_research_statistics)



# Typed task submissions are owned by the Research and Data services.
target_link_libraries(asterion_data_tests PRIVATE asterion_research_tasks asterion_data_tasks)
target_link_libraries(asterion_dominant_series_tests PRIVATE asterion_research_tasks asterion_data_tasks)
target_link_libraries(asterion_factor_tests PRIVATE asterion_research_tasks asterion_data_tasks)
target_link_libraries(asterion_material_task_tests PRIVATE asterion_research_tasks asterion_data_tasks)
target_link_libraries(asterion_test_minutes PRIVATE asterion_research_tasks asterion_data_tasks)
target_link_libraries(asterion_trading_backup_tests PRIVATE asterion_research_tasks asterion_data_tasks)
target_link_libraries(asterion_data_backup_tests PRIVATE asterion_research_tasks asterion_data_tasks)
target_link_libraries(asterion_node_supervision_tests PRIVATE asterion_research_tasks asterion_data_tasks)
target_link_libraries(asterion_account_review_store_tests PRIVATE asterion_research_tasks asterion_data_tasks)
target_link_libraries(asterion_logic_store_tests PRIVATE asterion_research_tasks asterion_data_tasks)
target_link_libraries(asterion_research_backup_tests PRIVATE asterion_research_tasks asterion_data_tasks)
target_link_libraries(asterion_research_evaluation_tests PRIVATE asterion_research_tasks asterion_data_tasks)
target_link_libraries(asterion_research_sample_tests PRIVATE asterion_research_tasks asterion_data_tasks)
target_link_libraries(asterion_research_statistics_tests PRIVATE asterion_research_tasks asterion_data_tasks)
target_link_libraries(asterion_strategy_store_tests PRIVATE asterion_research_tasks asterion_data_tasks)
target_link_libraries(asterion_task_execution_tests PRIVATE asterion_research_tasks asterion_data_tasks)

# Applied last so every directly registered test receives them.
get_property(asterion_tests DIRECTORY PROPERTY TESTS)
set_property(TEST ${asterion_tests} PROPERTY LABELS process)
if(APPLE)
  set_property(TEST ${asterion_tests} APPEND PROPERTY ENVIRONMENT "TMPDIR=/private/tmp")
endif()
