file(READ "${PROJECT_SOURCE_DIR}/core/config/futures-sessions.json" ASTERION_FUTURES_SESSIONS_JSON)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/core/config/futures-sessions.json")
configure_file(futures_sessions_json.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/futures_sessions_json.hpp" @ONLY)

add_library(asterion_futures STATIC
  market.cpp
  history_identity.cpp
  trading_days.cpp
  intraday_bars.cpp
  order.cpp
  order_limits.cpp
  futures.cpp
  account.cpp
  position_target.cpp
  paper_execution.cpp
  replay_schedule.cpp
  portfolio_targets.cpp
  replay.cpp
  performance.cpp
)
add_library(Asterion::Futures ALIAS asterion_futures)
target_include_directories(asterion_futures PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(asterion_futures PUBLIC Asterion::System)
asterion_target_defaults(asterion_futures)
