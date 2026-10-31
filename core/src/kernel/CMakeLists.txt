add_library(asterion_kernel STATIC
  native_plugin.cpp
  trace.cpp
  log.cpp
  ipc/local_channel.cpp
  ipc/tcp_channel.cpp
  ipc/rpc_client.cpp
  process/child.cpp
  process/artifact.cpp
  process/owner.cpp
  process/resources.cpp
  durable_file.cpp
  rpc_host.cpp
  environment.cpp
  payload_budget.cpp
)
add_library(Asterion::Kernel ALIAS asterion_kernel)
# The native data-source plugin ABI; it goes with the old Data service (M2.6).
target_include_directories(asterion_kernel PUBLIC ${PROJECT_SOURCE_DIR}/bindings/plugin/include)
target_link_libraries(asterion_kernel PRIVATE ${CMAKE_DL_LIBS})
target_link_libraries(asterion_kernel PUBLIC Asterion::Foundation Threads::Threads spdlog::spdlog_header_only PRIVATE asio::asio OpenSSL::SSL)
asterion_target_defaults(asterion_kernel)
