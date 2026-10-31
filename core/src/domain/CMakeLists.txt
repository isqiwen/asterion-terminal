add_library(asterion_domain STATIC
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
)
add_library(Asterion::Domain ALIAS asterion_domain)
target_link_libraries(asterion_domain PUBLIC Asterion::Kernel)
asterion_target_defaults(asterion_domain)
