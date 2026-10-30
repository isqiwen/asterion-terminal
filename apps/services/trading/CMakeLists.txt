add_library(asterion_trading_session STATIC
  live_session.cpp
  account_loop.cpp
  account_journal.cpp
  account_policy.cpp
  account_records.cpp
  settings.cpp
)
target_include_directories(asterion_trading_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(asterion_trading_session PUBLIC asterion_order_limits asterion_sqlite asterion_protocol asterion_ctp_trader)
add_executable(asterion_trading main.cpp)
target_link_libraries(asterion_trading PRIVATE asterion_trading_session asterion_protocol CLI11::CLI11)
asterion_target_defaults(asterion_trading_session)
asterion_target_defaults(asterion_trading)

add_subdirectory(ctp)

# The release for a trading host: one self-extracting script, setup.sh with
# the installation (bin/ lib/) appended as a tar.gz after a line
# "__ARCHIVE__". Built only when asked for: cmake --build … --target asterion_trading_setup
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(stage ${CMAKE_CURRENT_BINARY_DIR}/setup)
  set(release ${CMAKE_BINARY_DIR}/asterion-trading-setup.sh)
  set(layout ${stage}/asterion-trading)
  add_custom_command(OUTPUT ${release}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${stage}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${layout}/bin ${layout}/lib
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:asterion_trading> ${CMAKE_CURRENT_SOURCE_DIR}/scripts/setup.sh ${layout}/bin/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/ctp-trader.so ${layout}/lib/
    COMMAND ${CMAKE_COMMAND} -E chdir ${stage} ${CMAKE_COMMAND} -E tar czf payload.tar.gz asterion-trading
    COMMAND sh -c "{ cat \"$1\"; echo __ARCHIVE__; cat \"$2\"; } > \"$3\" && chmod 755 \"$3\"" setup
      ${CMAKE_CURRENT_SOURCE_DIR}/scripts/setup.sh ${stage}/payload.tar.gz ${release}
    DEPENDS asterion_trading ${CMAKE_CURRENT_SOURCE_DIR}/scripts/setup.sh
      ${CMAKE_BINARY_DIR}/ctp-trader.so
    VERBATIM)
  add_custom_target(asterion_trading_setup DEPENDS ${release})
endif()
