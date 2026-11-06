add_library(asterion_system STATIC
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
add_library(Asterion::System ALIAS asterion_system)
target_link_libraries(asterion_system PRIVATE ${CMAKE_DL_LIBS})
target_link_libraries(asterion_system PUBLIC Asterion::Base Threads::Threads spdlog::spdlog_header_only PRIVATE asio::asio OpenSSL::SSL)
asterion_target_defaults(asterion_system)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(libunwind CONFIG REQUIRED)
  target_sources(asterion_system PRIVATE crash.cpp)
  target_link_libraries(asterion_system PRIVATE libunwind::libunwind)
endif()
