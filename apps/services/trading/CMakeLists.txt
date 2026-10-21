add_library(asterion_trading_session STATIC
  live_session.cpp
  account_loop.cpp
  account_journal.cpp
  account_policy.cpp
  account_records.cpp
  strategy_host.cpp
)
target_include_directories(asterion_trading_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(asterion_trading_session PUBLIC asterion_risk_providers asterion_sqlite asterion_protocol asterion_ctp_trader asterion_strategies)
add_executable(asterion_trading main.cpp)
target_link_libraries(asterion_trading PRIVATE asterion_trading_session asterion_protocol CLI11::CLI11)
asterion_target_defaults(asterion_trading_session)
asterion_target_defaults(asterion_trading)
