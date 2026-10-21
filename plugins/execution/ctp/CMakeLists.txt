add_library(asterion_ctp_trader STATIC ctp_trader.cpp ctp_reconciliation.cpp)
target_include_directories(asterion_ctp_trader PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(asterion_ctp_trader PUBLIC Asterion::Domain PRIVATE asterion_ctp ctp::ctp ${CMAKE_DL_LIBS})
asterion_target_defaults(asterion_ctp_trader)
