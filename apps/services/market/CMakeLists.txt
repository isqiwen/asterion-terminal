# Recording keeps writing whatever the rest does: it links nothing of the history side.
add_library(asterion_market_recording STATIC recording/recorder.cpp)
target_link_libraries(asterion_market_recording PUBLIC asterion_ctp asterion_protocol asterion_bar_parquet duckdb::duckdb)
asterion_target_defaults(asterion_market_recording)
add_library(asterion_market_recorded STATIC
  recorded/reader.cpp recorded/compaction.cpp recorded/database.cpp recorded/compactor.cpp)
target_include_directories(asterion_market_recorded PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(asterion_market_recorded PUBLIC asterion_market_recording)
asterion_target_defaults(asterion_market_recorded)
add_library(asterion_market_warehouse STATIC warehouse/store.cpp warehouse/reader.cpp warehouse/research.cpp warehouse/composer.cpp)
target_include_directories(asterion_market_warehouse PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(asterion_market_warehouse PUBLIC asterion_protocol PRIVATE asterion_bar_parquet duckdb::duckdb)
asterion_target_defaults(asterion_market_warehouse)
find_package(ZLIB CONFIG REQUIRED)
find_package(tinyxml2 CONFIG REQUIRED)
add_library(asterion_market_acquisition STATIC
  acquisition/source.cpp acquisition/archive.cpp acquisition/values.cpp acquisition/tushare.cpp acquisition/recorded.cpp acquisition/recorded_publication.cpp acquisition/recorded_intake.cpp acquisition/exchanges.cpp acquisition/guotai.cpp acquisition/manager.cpp)
target_link_libraries(asterion_market_acquisition PUBLIC asterion_market_warehouse asterion_market_recorded PRIVATE httplib::httplib Iconv::Iconv ZLIB::ZLIB tinyxml2::tinyxml2)
asterion_target_defaults(asterion_market_acquisition)
add_library(asterion_market_settings STATIC settings.cpp)
target_link_libraries(asterion_market_settings PUBLIC asterion_ctp)
asterion_target_defaults(asterion_market_settings)
add_library(asterion_market_live STATIC live.cpp recording.cpp)
target_link_libraries(asterion_market_live PUBLIC asterion_market_settings asterion_market_recording)
asterion_target_defaults(asterion_market_live)
add_executable(asterion_market main.cpp market.cpp)
target_link_libraries(asterion_market PRIVATE asterion_market_live asterion_market_acquisition CLI11::CLI11)
asterion_target_defaults(asterion_market)

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
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:asterion_market> ${CMAKE_CURRENT_SOURCE_DIR}/scripts/setup.sh ${layout}/bin/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/ctp-md.so ${CMAKE_BINARY_DIR}/ctp-trader.so ${layout}/lib/
    COMMAND ${CMAKE_COMMAND} -E chdir ${stage} ${CMAKE_COMMAND} -E tar czf payload.tar.gz asterion-market
    COMMAND sh -c "{ cat \"$1\"; echo __ARCHIVE__; cat \"$2\"; } > \"$3\" && chmod 755 \"$3\"" setup
      ${CMAKE_CURRENT_SOURCE_DIR}/scripts/setup.sh ${stage}/payload.tar.gz ${release}
    DEPENDS asterion_crash_reporter ${CMAKE_CURRENT_SOURCE_DIR}/config/logging.json asterion_market ${CMAKE_CURRENT_SOURCE_DIR}/scripts/setup.sh
      ${CMAKE_BINARY_DIR}/ctp-md.so ${CMAKE_BINARY_DIR}/ctp-trader.so
    VERBATIM)
  add_custom_target(asterion_market_setup DEPENDS ${release})
endif()
