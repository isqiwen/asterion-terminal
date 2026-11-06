add_library(asterion_trading_session STATIC
  live_session.cpp
  account_loop.cpp
  account_journal.cpp
  account_policy.cpp
  account_records.cpp
  settings.cpp
  strategy_host.cpp
  strategy_bars.cpp
  strategy_orders.cpp
  strategy_execution.cpp
)
target_include_directories(asterion_trading_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(asterion_trading_session PUBLIC Asterion::Futures asterion_sqlite asterion_protocol asterion_ctp_trader asterion_ctp)
add_executable(asterion_trading main.cpp)
target_link_libraries(asterion_trading PRIVATE asterion_trading_session asterion_protocol CLI11::CLI11)
asterion_target_defaults(asterion_trading_session)
asterion_target_defaults(asterion_trading)

add_subdirectory(ctp)

# The release for a trading host: one self-extracting script, setup.sh with
# the installation (bin/ lib/ runtime/ config/) appended as a tar.gz after a line
# "__ARCHIVE__". Built only when asked for: cmake --build … --target asterion_trading_setup
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(runtime ${CMAKE_CURRENT_BINARY_DIR}/runtime/python)
  set(runtime_ready ${CMAKE_CURRENT_BINARY_DIR}/runtime/prepared.stamp)
  add_custom_command(OUTPUT ${runtime_ready}
    BYPRODUCTS ${runtime}/bin/python3
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/prepare_runtime.py ${runtime}
    COMMAND ${CMAKE_COMMAND} -E touch ${runtime_ready}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/scripts/prepare_runtime.py
      ${CMAKE_CURRENT_SOURCE_DIR}/pyproject.toml ${CMAKE_CURRENT_SOURCE_DIR}/uv.lock
    VERBATIM)
  set(stage ${CMAKE_CURRENT_BINARY_DIR}/setup)
  set(release ${CMAKE_BINARY_DIR}/Asterion-Trading-${PROJECT_VERSION}-linux-x86_64.sh)
  set(layout ${stage}/asterion-trading)
  add_custom_command(OUTPUT ${release}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${stage}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${layout}/bin ${layout}/lib ${layout}/config
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/config/logging.json ${layout}/config/
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:asterion_trading> ${CMAKE_CURRENT_SOURCE_DIR}/scripts/setup.sh ${layout}/bin/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/ctp-trader.so ${CMAKE_BINARY_DIR}/ctp-md.so ${layout}/lib/
    COMMAND ${CMAKE_COMMAND} -E make_directory ${layout}/lib/strategy/asterion
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/python/strategy_runner.py ${PROJECT_SOURCE_DIR}/apps/services/common/python/asterion_strategy.py ${layout}/lib/strategy/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/generated-python/asterion/strategy_pb2.py ${CMAKE_BINARY_DIR}/generated-python/asterion/common_pb2.py ${layout}/lib/strategy/asterion/
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${runtime} ${layout}/runtime/python
    COMMAND ${CMAKE_COMMAND} -E chdir ${stage} ${CMAKE_COMMAND} -E tar czf payload.tar.gz asterion-trading
    COMMAND sh -c "{ cat \"$1\"; echo __ARCHIVE__; cat \"$2\"; } > \"$3\" && chmod 755 \"$3\"" setup
      ${CMAKE_CURRENT_SOURCE_DIR}/scripts/setup.sh ${stage}/payload.tar.gz ${release}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/config/logging.json asterion_trading asterion_python_protocol ${CMAKE_CURRENT_SOURCE_DIR}/scripts/setup.sh
      ${runtime_ready} ${runtime}/bin/python3
      ${CMAKE_CURRENT_SOURCE_DIR}/python/strategy_runner.py ${PROJECT_SOURCE_DIR}/apps/services/common/python/asterion_strategy.py
      ${CMAKE_BINARY_DIR}/generated-python/asterion/strategy_pb2.py
      ${CMAKE_BINARY_DIR}/ctp-trader.so
      ${CMAKE_BINARY_DIR}/ctp-md.so
    VERBATIM)
  add_custom_target(asterion_trading_setup DEPENDS ${release})
endif()
