add_library(asterion_futures STATIC
  market.cpp
  historical_bars.cpp
  history_identity.cpp
  history_tables.cpp
  intraday_bars.cpp
  order.cpp
  order_limits.cpp
  futures.cpp
  account.cpp
  position_target.cpp
  risk_port.cpp
  paper_execution.cpp
  replay_schedule.cpp
  portfolio_targets.cpp
  replay.cpp
)
add_library(Asterion::Futures ALIAS asterion_futures)
target_link_libraries(asterion_futures PUBLIC Asterion::System)
asterion_target_defaults(asterion_futures)
