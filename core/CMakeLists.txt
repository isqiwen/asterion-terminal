add_library(asterion_foundation STATIC src/foundation/decimal.cpp)
add_library(Asterion::Foundation ALIAS asterion_foundation)
target_compile_features(asterion_foundation PUBLIC cxx_std_20)
target_include_directories(asterion_foundation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(asterion_foundation PUBLIC nlohmann_json::nlohmann_json)
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

add_library(asterion_kernel STATIC
  src/kernel/native_plugin.cpp
  src/kernel/trace.cpp
  src/kernel/log.cpp
  src/kernel/ipc/local_channel.cpp
  src/kernel/ipc/tcp_channel.cpp
  src/kernel/process/child.cpp
  src/kernel/process/artifact.cpp
  src/kernel/process/owner.cpp
  src/kernel/durable_file.cpp
  src/kernel/rpc_host.cpp
  src/kernel/ipc/rpc_client.cpp
  src/kernel/environment.cpp
)
target_sources(asterion_kernel PRIVATE src/kernel/process/resources.cpp src/kernel/payload_budget.cpp)
add_library(Asterion::Kernel ALIAS asterion_kernel)
target_include_directories(asterion_kernel PUBLIC ${PROJECT_SOURCE_DIR}/bindings/plugin/include)
target_link_libraries(asterion_kernel PRIVATE ${CMAKE_DL_LIBS})
target_link_libraries(asterion_kernel PUBLIC Asterion::Foundation Threads::Threads spdlog::spdlog_header_only PRIVATE asio::asio OpenSSL::SSL)

add_library(asterion_domain STATIC
  src/domain/market.cpp
  src/domain/historical_bars.cpp
  src/domain/history_identity.cpp
  src/domain/history_tables.cpp
  src/domain/intraday_bars.cpp
  src/domain/order.cpp
  src/domain/futures.cpp
  src/domain/account.cpp
  src/domain/position_target.cpp
  src/domain/risk_port.cpp
)
add_library(Asterion::Domain ALIAS asterion_domain)
target_link_libraries(asterion_domain PUBLIC Asterion::Kernel)
asterion_target_defaults(asterion_foundation)
asterion_target_defaults(asterion_kernel)
asterion_target_defaults(asterion_domain)

add_subdirectory(storage/sqlite)
add_subdirectory(risk/order_limits)
