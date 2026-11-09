# The live market: it links nothing of Warehouse or Trading, and has no way to send an order.
add_library(asterion_market_settings STATIC settings.cpp)
target_link_libraries(asterion_market_settings PUBLIC asterion_ctp)
asterion_target_defaults(asterion_market_settings)
add_library(asterion_market_live STATIC live.cpp)
target_link_libraries(asterion_market_live PUBLIC asterion_market_settings asterion_protocol)
asterion_target_defaults(asterion_market_live)
add_executable(asterion_market main.cpp service.cpp)
target_link_libraries(asterion_market PRIVATE asterion_market_live CLI11::CLI11)
asterion_target_defaults(asterion_market)

asterion_setup_script(asterion_market_setup_script)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_dependencies(asterion_market asterion_crash_reporter)
  set(stage ${CMAKE_CURRENT_BINARY_DIR}/setup)
  set(release ${CMAKE_BINARY_DIR}/Asterion-Market-${PROJECT_VERSION}-linux-x86_64.sh)
  set(layout ${stage}/asterion-market)
  add_custom_command(OUTPUT ${release}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${stage}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${layout}/bin ${layout}/lib ${layout}/config
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:asterion_crash_reporter> ${layout}/bin/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/config/logging.json ${layout}/config/
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:asterion_market> ${CMAKE_CURRENT_BINARY_DIR}/setup.sh ${layout}/bin/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/ctp-md.so ${CMAKE_BINARY_DIR}/ctp-trader.so ${layout}/lib/
    COMMAND ${CMAKE_COMMAND} -E chdir ${stage} ${CMAKE_COMMAND} -E tar czf payload.tar.gz asterion-market
    COMMAND sh -c "{ cat \"$1\"; echo __ARCHIVE__; cat \"$2\"; } > \"$3\" && chmod 755 \"$3\"" setup
      ${CMAKE_CURRENT_BINARY_DIR}/setup.sh ${stage}/payload.tar.gz ${release}
    DEPENDS asterion_market_setup_script asterion_crash_reporter ${CMAKE_CURRENT_SOURCE_DIR}/config/logging.json asterion_market ${CMAKE_CURRENT_BINARY_DIR}/setup.sh
      ${CMAKE_BINARY_DIR}/ctp-md.so ${CMAKE_BINARY_DIR}/ctp-trader.so
    VERBATIM)
  add_custom_target(asterion_market_setup DEPENDS ${release})
endif()
