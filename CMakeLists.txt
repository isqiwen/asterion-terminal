cmake_minimum_required(VERSION 3.25)
project(Asterion VERSION 0.1.0 LANGUAGES CXX)
if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Darwin|Linux)$")
  message(FATAL_ERROR "Asterion supports macOS and Linux x86_64 only")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  message(FATAL_ERROR "Linux currently supports x86_64 only")
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/services/service_fingerprint.py"
  OUTPUT_VARIABLE ASTERION_SERVICE_SOURCE_SHA256 OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/services/service_fingerprint.py" --files
  OUTPUT_VARIABLE ASTERION_SERVICE_INPUTS OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ASTERION_SERVICE_INPUTS})
# Detect added/deleted inputs as well as edits to the existing source files.
file(GLOB_RECURSE ASTERION_SERVICE_TREE CONFIGURE_DEPENDS
  core/* protocol/* plugins/* bindings/* apps/services/* conan/* scripts/node/*)
file(WRITE "${CMAKE_BINARY_DIR}/service-source.sha256" "${ASTERION_SERVICE_SOURCE_SHA256}\n")

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include(CTest)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(OpenSSL CONFIG REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(ctp CONFIG REQUIRED)

add_library(asterion_foundation STATIC core/src/foundation/decimal.cpp)
add_library(Asterion::Foundation ALIAS asterion_foundation)
target_compile_features(asterion_foundation PUBLIC cxx_std_20)
target_include_directories(asterion_foundation PUBLIC core/include)
target_link_libraries(asterion_foundation PUBLIC nlohmann_json::nlohmann_json)
set(ASTERION_SANITIZERS "" CACHE STRING "Optional address,undefined or thread sanitizer")
if(ASTERION_SANITIZERS)
  if(NOT ASTERION_SANITIZERS MATCHES "^(address,undefined|thread)$")
    message(FATAL_ERROR "Sanitizers require Clang/GCC and address,undefined or thread")
  endif()
  # -g1 keeps line tables for sanitizer stacks; full Linux DWARF would push
  # instrumented services past the Agent's 128 MiB artifact limit.
  target_compile_options(asterion_foundation PUBLIC -fsanitize=${ASTERION_SANITIZERS} -fno-omit-frame-pointer -g1)
  target_link_options(asterion_foundation PUBLIC -fsanitize=${ASTERION_SANITIZERS})
  # Instrumented builds run several times slower; timing assertions scale.
  target_compile_definitions(asterion_foundation PUBLIC ASTERION_SANITIZED=1)
endif()

add_library(asterion_kernel STATIC core/src/kernel/native_plugin.cpp core/src/kernel/trace.cpp core/src/kernel/logger.cpp core/src/kernel/ipc/local_channel.cpp core/src/kernel/ipc/tls_channel.cpp core/src/kernel/process/child.cpp core/src/kernel/process/artifact.cpp core/src/kernel/process/owner.cpp core/src/kernel/durable_file.cpp core/src/kernel/rpc_host.cpp core/src/kernel/ipc/rpc_client.cpp core/src/kernel/environment.cpp)
target_sources(asterion_kernel PRIVATE core/src/kernel/process/capacity.cpp core/src/kernel/payload_budget.cpp)
add_library(Asterion::Kernel ALIAS asterion_kernel)
target_include_directories(asterion_kernel PUBLIC bindings/plugin/include)
target_link_libraries(asterion_kernel PRIVATE ${CMAKE_DL_LIBS})
target_link_libraries(asterion_kernel PUBLIC Asterion::Foundation Threads::Threads PRIVATE spdlog::spdlog_header_only asio::asio OpenSSL::SSL)

add_library(asterion_domain STATIC core/src/domain/market.cpp core/src/domain/historical_bars.cpp core/src/domain/history_identity.cpp core/src/domain/intraday_bars.cpp core/src/domain/order.cpp core/src/domain/futures.cpp core/src/domain/account.cpp core/src/domain/position_target.cpp core/src/domain/risk_port.cpp)
add_library(Asterion::Domain ALIAS asterion_domain)
target_link_libraries(asterion_domain PUBLIC Asterion::Kernel)


add_library(asterion_tushare STATIC plugins/data/tushare/tushare.cpp plugins/data/tushare/https.cpp)
target_include_directories(asterion_tushare PUBLIC plugins/data/tushare)
target_link_libraries(asterion_tushare PUBLIC Asterion::Domain PRIVATE httplib::httplib)

add_library(asterion_history_providers STATIC plugins/data/registry/history_providers.cpp)
target_include_directories(asterion_history_providers PUBLIC plugins/data/registry)
target_link_libraries(asterion_history_providers PUBLIC Asterion::Domain)
add_library(asterion_tushare_plugin SHARED plugins/data/tushare/native_plugin.cpp)
target_link_libraries(asterion_tushare_plugin PRIVATE asterion_tushare)
set_target_properties(asterion_tushare_plugin PROPERTIES PREFIX "" OUTPUT_NAME "asterion-tushare" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins" CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
if(APPLE)
  target_link_options(asterion_tushare_plugin PRIVATE "-Wl,-exported_symbol,_asterion_plugin_entry_v1")
endif()
add_dependencies(asterion_history_providers asterion_tushare_plugin)

add_library(asterion_strategies STATIC plugins/strategy/cta/strategy.cpp)
target_include_directories(asterion_strategies PUBLIC plugins/strategy/cta)
target_link_libraries(asterion_strategies PUBLIC asterion_protocol)

add_library(asterion_chart_indicators STATIC plugins/tools/chart_indicators/macd.cpp plugins/tools/chart_indicators/calendar_bars.cpp)
target_include_directories(asterion_chart_indicators PUBLIC plugins/tools/chart_indicators)
target_link_libraries(asterion_chart_indicators PUBLIC Asterion::Domain)

add_library(asterion_performance STATIC plugins/tools/performance/performance.cpp)
target_include_directories(asterion_performance PUBLIC plugins/tools/performance)
target_link_libraries(asterion_performance PUBLIC Asterion::Foundation)

add_library(asterion_factor_analysis STATIC plugins/tools/factor_analysis/momentum.cpp plugins/tools/factor_analysis/walk_forward.cpp)
target_include_directories(asterion_factor_analysis PUBLIC plugins/tools/factor_analysis)
target_link_libraries(asterion_factor_analysis PUBLIC Asterion::Domain)

add_library(asterion_order_limits_config STATIC plugins/risk/order-limits/config.cpp)
target_link_libraries(asterion_order_limits_config PUBLIC Asterion::Domain)
target_include_directories(asterion_order_limits_config PUBLIC plugins/risk/order-limits)
add_library(asterion_order_limits STATIC plugins/risk/order-limits/order_limits.cpp)
target_link_libraries(asterion_order_limits PUBLIC asterion_order_limits_config)
target_include_directories(asterion_order_limits PUBLIC plugins/risk/order-limits)
add_library(asterion_risk_providers STATIC plugins/risk/registry/native_risk.cpp plugins/risk/registry/risk_module.cpp)
target_include_directories(asterion_risk_providers PUBLIC plugins/risk/registry)
target_link_libraries(asterion_risk_providers PUBLIC asterion_order_limits_config)
add_library(asterion_order_limits_plugin SHARED plugins/risk/order-limits/native_plugin.cpp)
target_link_libraries(asterion_order_limits_plugin PRIVATE asterion_order_limits)
set_target_properties(asterion_order_limits_plugin PROPERTIES PREFIX "" OUTPUT_NAME "asterion-order-limits" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins" CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
if(APPLE)
  target_link_options(asterion_order_limits_plugin PRIVATE "-Wl,-exported_symbol,_asterion_plugin_entry_v1")
endif()
add_dependencies(asterion_risk_providers asterion_order_limits_plugin)
add_library(asterion_paper STATIC plugins/execution/paper/paper_execution.cpp plugins/execution/paper/replay_schedule.cpp plugins/execution/paper/portfolio.cpp)
target_link_libraries(asterion_paper PUBLIC Asterion::Domain asterion_protocol)
target_include_directories(asterion_paper PUBLIC plugins/execution/paper)
find_package(SQLite3 REQUIRED)
find_package(duckdb CONFIG REQUIRED)
add_library(asterion_bar_parquet STATIC plugins/storage/parquet/bar_parquet.cpp)
target_link_libraries(asterion_bar_parquet PUBLIC Asterion::Domain PRIVATE Asterion::Kernel duckdb::duckdb)
target_include_directories(asterion_bar_parquet PUBLIC plugins/storage/parquet)
add_library(asterion_sqlite STATIC plugins/storage/sqlite/sqlite_database.cpp plugins/storage/sqlite/sqlite_journal.cpp)
target_link_libraries(asterion_sqlite PUBLIC Asterion::Kernel PRIVATE SQLite::SQLite3)
target_include_directories(asterion_sqlite PUBLIC plugins/storage/sqlite)

set(ASTERION_PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/protocol/proto)
set(ASTERION_PROTO_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${ASTERION_PROTO_GEN})
set(ASTERION_PROTO_SOURCES)
file(GLOB ASTERION_PROTO_FILES CONFIGURE_DEPENDS ${ASTERION_PROTO_ROOT}/asterion/v1/*.proto)
foreach(schema health domain trading node market task backtest factor data data_service)
  add_custom_command(
    OUTPUT ${ASTERION_PROTO_GEN}/asterion/v1/${schema}.pb.cc ${ASTERION_PROTO_GEN}/asterion/v1/${schema}.pb.h
    COMMAND protobuf::protoc --proto_path=${ASTERION_PROTO_ROOT} --cpp_out=${ASTERION_PROTO_GEN} ${ASTERION_PROTO_ROOT}/asterion/v1/${schema}.proto
    DEPENDS ${ASTERION_PROTO_FILES} protobuf::protoc VERBATIM)
  list(APPEND ASTERION_PROTO_SOURCES ${ASTERION_PROTO_GEN}/asterion/v1/${schema}.pb.cc)
endforeach()
add_library(asterion_protocol STATIC ${ASTERION_PROTO_SOURCES} protocol/src/trading.cpp protocol/src/health.cpp protocol/src/market.cpp protocol/src/bar_dataset.cpp protocol/src/task.cpp protocol/src/task_execution.cpp protocol/src/backtest.cpp protocol/src/factor.cpp protocol/src/data.cpp)
target_include_directories(asterion_protocol PUBLIC protocol/include ${ASTERION_PROTO_GEN})
target_link_libraries(asterion_protocol PUBLIC Asterion::Domain protobuf::libprotobuf)

# Storage plugins that persist protocol-typed records.
add_library(asterion_history_files STATIC plugins/storage/filesystem/history_archive.cpp plugins/storage/filesystem/history_minutes.cpp plugins/storage/filesystem/history_daily.cpp)
target_include_directories(asterion_history_files PUBLIC plugins/storage/filesystem)
target_link_libraries(asterion_history_files PUBLIC asterion_protocol PRIVATE asterion_chart_indicators asterion_bar_parquet)
add_library(asterion_task_events STATIC plugins/storage/sqlite/task_events.cpp)
target_link_libraries(asterion_task_events PUBLIC asterion_sqlite asterion_protocol)

add_library(asterion_data_client STATIC protocol/src/data_client.cpp)
target_link_libraries(asterion_data_client PUBLIC asterion_protocol)

add_library(asterion_task_client STATIC protocol/src/task_client.cpp protocol/src/task_input.cpp)
target_link_libraries(asterion_task_client PUBLIC asterion_data_client)

add_library(asterion_ctp STATIC plugins/data/ctp/ctp_feed.cpp plugins/data/ctp/ctp_catalog.cpp)
find_package(Iconv REQUIRED)
target_include_directories(asterion_ctp PUBLIC plugins/data/ctp)
target_link_libraries(asterion_ctp PUBLIC Asterion::Domain PRIVATE ctp::ctp Iconv::Iconv ${CMAKE_DL_LIBS})
add_library(asterion_ctp_trader STATIC plugins/execution/ctp/ctp_trader.cpp plugins/execution/ctp/ctp_reconciliation.cpp)
target_include_directories(asterion_ctp_trader PUBLIC plugins/execution/ctp)
target_link_libraries(asterion_ctp_trader PUBLIC Asterion::Domain PRIVATE asterion_ctp ctp::ctp ${CMAKE_DL_LIBS})
add_executable(asterion-market-data apps/services/market-data/main.cpp apps/services/market-data/session.cpp)
target_link_libraries(asterion-market-data PRIVATE asterion_ctp asterion_protocol CLI11::CLI11)


add_library(asterion_trading_session STATIC apps/services/trading/live_session.cpp apps/services/trading/account_loop.cpp apps/services/trading/account_journal.cpp apps/services/trading/account_policy.cpp apps/services/trading/account_records.cpp apps/services/trading/strategy_host.cpp)
target_include_directories(asterion_trading_session PUBLIC apps/services/trading)
target_link_libraries(asterion_trading_session PUBLIC asterion_risk_providers asterion_sqlite asterion_protocol asterion_ctp_trader asterion_strategies)
add_executable(asterion-trading apps/services/trading/main.cpp)
target_link_libraries(asterion-trading PRIVATE asterion_trading_session asterion_protocol CLI11::CLI11)

add_library(asterion_node_firewall STATIC apps/services/node-agent/firewall.cpp)
target_include_directories(asterion_node_firewall PUBLIC apps/services/node-agent)
target_link_libraries(asterion_node_firewall PUBLIC Asterion::Kernel PRIVATE asio::asio OpenSSL::Crypto)

add_library(asterion_node_storage STATIC apps/services/node-agent/plugin_artifacts.cpp apps/services/node-agent/service_configuration.cpp apps/services/node-agent/development_programs.cpp apps/services/node-agent/upgrade_plan.cpp)
target_include_directories(asterion_node_storage PUBLIC apps/services/node-agent)
target_link_libraries(asterion_node_storage PUBLIC Asterion::Kernel asterion_protocol)

add_library(asterion_node_supervision STATIC apps/services/node-agent/supervision_rpc.cpp)
target_include_directories(asterion_node_supervision PUBLIC apps/services/node-agent)
target_link_libraries(asterion_node_supervision PUBLIC Asterion::Kernel asterion_protocol)

add_library(asterion_node_operations STATIC apps/services/node-agent/artifact_uploads.cpp apps/services/node-agent/firewall_control.cpp)
target_include_directories(asterion_node_operations PUBLIC apps/services/node-agent)
target_link_libraries(asterion_node_operations PUBLIC Asterion::Kernel asterion_protocol PRIVATE asterion_node_firewall)
add_executable(asterion-node-agent apps/services/node-agent/main.cpp)
target_link_libraries(asterion-node-agent PRIVATE asterion_node_operations asterion_node_supervision asterion_node_storage asterion_node_firewall Asterion::Kernel asterion_protocol OpenSSL::Crypto CLI11::CLI11)

add_library(asterion_terminal_api STATIC bindings/c/src/terminal.cpp apps/clients/terminal/native/service_io.cpp apps/clients/terminal/native/market_client.cpp apps/clients/terminal/native/market_snapshot.cpp apps/clients/terminal/native/task_client.cpp apps/clients/terminal/native/data_client.cpp apps/clients/terminal/native/terminal_application.cpp apps/clients/terminal/native/commands_live.cpp apps/clients/terminal/native/commands_node.cpp apps/clients/terminal/native/commands_data.cpp apps/clients/terminal/native/commands_task.cpp apps/clients/terminal/native/commands_backtest.cpp apps/clients/terminal/native/commands_factor.cpp apps/clients/terminal/native/commands_node_data_tasks.cpp apps/clients/terminal/native/commands_history_usage.cpp apps/clients/terminal/native/commands_connections.cpp apps/clients/terminal/native/data_credentials.cpp apps/clients/terminal/native/keychain_store.cpp apps/clients/terminal/native/ctp_connections.cpp apps/clients/terminal/native/commands_market.cpp apps/clients/terminal/native/trading_client.cpp apps/clients/terminal/native/node_client.cpp apps/clients/terminal/native/plugin_catalog.cpp apps/clients/terminal/native/node_snapshot.cpp apps/clients/terminal/native/service_programs.cpp apps/clients/terminal/native/local_node.cpp apps/clients/terminal/native/node_service.cpp apps/clients/terminal/native/node_program.cpp apps/clients/terminal/native/node_identity.cpp apps/clients/terminal/native/node_enrollment.cpp apps/clients/terminal/native/remote_bundle.cpp)
target_compile_definitions(asterion_terminal_api PRIVATE ASTERION_PRODUCT_VERSION="${PROJECT_VERSION}")
set_source_files_properties(apps/clients/terminal/native/remote_bundle.cpp PROPERTIES COMPILE_DEFINITIONS ASTERION_SERVICE_SOURCE_SHA256="${ASTERION_SERVICE_SOURCE_SHA256}")
target_include_directories(asterion_terminal_api PUBLIC bindings/c/include PRIVATE apps/clients/terminal/native)
target_link_libraries(asterion_terminal_api PUBLIC asterion_chart_indicators asterion_node_firewall asterion_protocol PRIVATE asterion_performance asterion_data_client asterion_task_store asterion_sqlite nlohmann_json::nlohmann_json OpenSSL::Crypto)
add_dependencies(asterion_terminal_api asterion-trading asterion-node-agent asterion-market-data)

# Node-API is the desktop SDK boundary. Headers are explicitly prepared from
# the pinned Electron release; Conan still owns all Core dependencies.
set(ASTERION_NODE_HEADERS "" CACHE PATH "Prepared Electron Node-API include directory")
if(ASTERION_NODE_HEADERS)
  if(NOT EXISTS "${ASTERION_NODE_HEADERS}/node_api.h")
    message(FATAL_ERROR "Prepare the pinned Electron SDK before building the desktop")
  endif()
  add_library(asterion_terminal_node MODULE bindings/node/terminal.cpp)
  target_compile_features(asterion_terminal_node PRIVATE cxx_std_20)
  target_compile_definitions(asterion_terminal_node PRIVATE NAPI_VERSION=8)
  target_include_directories(asterion_terminal_node PRIVATE "${ASTERION_NODE_HEADERS}" apps/clients/terminal/native)
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

if(APPLE)
  # Kept free of project libraries so its code signature, which the keychain
  # uses to grant access, only changes when this file does.
  add_executable(asterion-keychain apps/clients/terminal/native/keychain/main.cpp)
  target_link_libraries(asterion-keychain PRIVATE "-framework Security" "-framework CoreFoundation")
else()
  # The desktop's libsecret is loaded when the helper runs, not linked.
  add_executable(asterion-keychain apps/clients/terminal/native/keychain/secret_service.cpp)
  target_compile_features(asterion-keychain PRIVATE cxx_std_20)
  target_link_libraries(asterion-keychain PRIVATE ${CMAKE_DL_LIBS})
endif()
add_dependencies(asterion_terminal_api asterion-keychain)
add_executable(asterion_terminal_dev_bridge apps/clients/terminal/dev/core_bridge.cpp)
target_link_libraries(asterion_terminal_dev_bridge PRIVATE asterion_terminal_api CLI11::CLI11)


# Independently deployable service applications; worker programs run per task.
set(ASTERION_SERVICE_APPS backtest factor data-pipeline data-service task-service)
set(ASTERION_SERVICE_TARGETS)
foreach(application IN LISTS ASTERION_SERVICE_APPS)
  add_subdirectory(apps/services/${application})
  set_target_properties(asterion-${application} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  list(APPEND ASTERION_SERVICE_TARGETS asterion-${application})
endforeach()
add_dependencies(asterion_terminal_api ${ASTERION_SERVICE_TARGETS})

foreach(target asterion_order_limits_config asterion_risk_providers asterion_order_limits_plugin asterion_tushare asterion_order_limits asterion_ctp_trader asterion_history_files asterion_history_providers asterion_task_client asterion_factor_engine asterion_factor_analysis asterion_performance asterion_task_store asterion_task_events asterion_strategies asterion_backtest_engine ${ASTERION_SERVICE_TARGETS} asterion_ctp asterion-market-data asterion_node_firewall asterion_node_storage asterion_node_supervision asterion_node_operations asterion-node-agent asterion-trading asterion_trading_session asterion_terminal_dev_bridge asterion_paper asterion_sqlite asterion_foundation asterion_kernel asterion_domain asterion_terminal_api)
  # Designated initializers omit members on purpose; Clang accepts that and
  # GCC's -Wextra would reject it.
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror
    $<$<CXX_COMPILER_ID:GNU>:-Wno-missing-field-initializers>)
endforeach()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
