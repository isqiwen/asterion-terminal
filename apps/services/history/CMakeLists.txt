add_library(asterion_history_warehouse STATIC warehouse/store.cpp warehouse/reader.cpp warehouse/research.cpp warehouse/composer.cpp)
target_include_directories(asterion_history_warehouse PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(asterion_history_warehouse PUBLIC asterion_protocol PRIVATE asterion_bar_parquet duckdb::duckdb)
asterion_target_defaults(asterion_history_warehouse)
find_package(ZLIB CONFIG REQUIRED)
find_package(tinyxml2 CONFIG REQUIRED)
add_library(asterion_history_acquisition STATIC
  acquisition/source.cpp acquisition/archive.cpp acquisition/values.cpp acquisition/tushare.cpp acquisition/catalog_source.cpp acquisition/sina.cpp acquisition/snapshot_intake.cpp acquisition/exchanges.cpp acquisition/guotai.cpp acquisition/manager.cpp)
target_link_libraries(asterion_history_acquisition PUBLIC asterion_history_warehouse PRIVATE httplib::httplib Iconv::Iconv ZLIB::ZLIB tinyxml2::tinyxml2)
asterion_target_defaults(asterion_history_acquisition)
add_library(asterion_history_settings STATIC settings.cpp)
target_link_libraries(asterion_history_settings PUBLIC asterion_ctp)
asterion_target_defaults(asterion_history_settings)
# The live market links nothing of the history side.
add_library(asterion_history_live STATIC live.cpp)
target_link_libraries(asterion_history_live PUBLIC asterion_history_settings asterion_protocol)
asterion_target_defaults(asterion_history_live)
add_executable(asterion_history main.cpp service.cpp)
target_link_libraries(asterion_history PRIVATE asterion_history_live asterion_history_acquisition CLI11::CLI11)
asterion_target_defaults(asterion_history)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_dependencies(asterion_history asterion_crash_reporter)
  set(stage ${CMAKE_CURRENT_BINARY_DIR}/setup)
  set(release ${CMAKE_BINARY_DIR}/Asterion-History-${PROJECT_VERSION}-linux-x86_64.sh)
  set(layout ${stage}/asterion-history)
  add_custom_command(OUTPUT ${release}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${stage}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${layout}/bin ${layout}/lib ${layout}/config
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:asterion_crash_reporter> ${layout}/bin/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/config/logging.json ${layout}/config/
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:asterion_history> ${CMAKE_CURRENT_SOURCE_DIR}/scripts/setup.sh ${layout}/bin/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/ctp-md.so ${CMAKE_BINARY_DIR}/ctp-trader.so ${layout}/lib/
    COMMAND ${CMAKE_COMMAND} -E chdir ${stage} ${CMAKE_COMMAND} -E tar czf payload.tar.gz asterion-history
    COMMAND sh -c "{ cat \"$1\"; echo __ARCHIVE__; cat \"$2\"; } > \"$3\" && chmod 755 \"$3\"" setup
      ${CMAKE_CURRENT_SOURCE_DIR}/scripts/setup.sh ${stage}/payload.tar.gz ${release}
    DEPENDS asterion_crash_reporter ${CMAKE_CURRENT_SOURCE_DIR}/config/logging.json asterion_history ${CMAKE_CURRENT_SOURCE_DIR}/scripts/setup.sh
      ${CMAKE_BINARY_DIR}/ctp-md.so ${CMAKE_BINARY_DIR}/ctp-trader.so
    VERBATIM)
  add_custom_target(asterion_history_setup DEPENDS ${release})
endif()
