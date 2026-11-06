# The live market: it links nothing of History or Trading, and has no way to send an order.
add_library(asterion_quotes_settings STATIC settings.cpp)
target_link_libraries(asterion_quotes_settings PUBLIC asterion_ctp)
asterion_target_defaults(asterion_quotes_settings)
add_library(asterion_quotes_live STATIC live.cpp)
target_link_libraries(asterion_quotes_live PUBLIC asterion_quotes_settings asterion_protocol)
asterion_target_defaults(asterion_quotes_live)
add_executable(asterion_quotes main.cpp service.cpp)
target_link_libraries(asterion_quotes PRIVATE asterion_quotes_live CLI11::CLI11)
asterion_target_defaults(asterion_quotes)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_dependencies(asterion_quotes asterion_crash_reporter)
  set(stage ${CMAKE_CURRENT_BINARY_DIR}/setup)
  set(release ${CMAKE_BINARY_DIR}/Asterion-Quotes-${PROJECT_VERSION}-linux-x86_64.sh)
  set(layout ${stage}/asterion-quotes)
  add_custom_command(OUTPUT ${release}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${stage}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${layout}/bin ${layout}/lib ${layout}/config
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:asterion_crash_reporter> ${layout}/bin/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/config/logging.json ${layout}/config/
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:asterion_quotes> ${CMAKE_CURRENT_SOURCE_DIR}/scripts/setup.sh ${layout}/bin/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/ctp-md.so ${CMAKE_BINARY_DIR}/ctp-trader.so ${layout}/lib/
    COMMAND ${CMAKE_COMMAND} -E chdir ${stage} ${CMAKE_COMMAND} -E tar czf payload.tar.gz asterion-quotes
    COMMAND sh -c "{ cat \"$1\"; echo __ARCHIVE__; cat \"$2\"; } > \"$3\" && chmod 755 \"$3\"" setup
      ${CMAKE_CURRENT_SOURCE_DIR}/scripts/setup.sh ${stage}/payload.tar.gz ${release}
    DEPENDS asterion_crash_reporter ${CMAKE_CURRENT_SOURCE_DIR}/config/logging.json asterion_quotes ${CMAKE_CURRENT_SOURCE_DIR}/scripts/setup.sh
      ${CMAKE_BINARY_DIR}/ctp-md.so ${CMAKE_BINARY_DIR}/ctp-trader.so
    VERBATIM)
  add_custom_target(asterion_quotes_setup DEPENDS ${release})
endif()
