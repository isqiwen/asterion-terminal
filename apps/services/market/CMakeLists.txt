add_library(asterion_market_recording STATIC
  recording/recorder.cpp recording/reader.cpp recording/compaction.cpp
  recording/database.cpp recording/compactor.cpp)
target_link_libraries(asterion_market_recording PUBLIC asterion_ctp asterion_protocol asterion_bar_parquet duckdb::duckdb)
asterion_target_defaults(asterion_market_recording)
add_library(asterion_market_warehouse STATIC warehouse/store.cpp warehouse/reader.cpp)
target_include_directories(asterion_market_warehouse PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(asterion_market_warehouse PUBLIC asterion_protocol PRIVATE asterion_bar_parquet duckdb::duckdb)
asterion_target_defaults(asterion_market_warehouse)
find_package(ZLIB CONFIG REQUIRED)
add_library(asterion_market_acquisition STATIC
  acquisition/source.cpp acquisition/archive.cpp acquisition/values.cpp acquisition/tushare.cpp acquisition/recorded.cpp acquisition/exchanges.cpp acquisition/manager.cpp)
target_link_libraries(asterion_market_acquisition PUBLIC asterion_market_warehouse asterion_market_recording PRIVATE httplib::httplib Iconv::Iconv ZLIB::ZLIB)
asterion_target_defaults(asterion_market_acquisition)
add_library(asterion_market_settings STATIC settings.cpp)
target_link_libraries(asterion_market_settings PUBLIC asterion_ctp)
asterion_target_defaults(asterion_market_settings)
add_executable(asterion_market main.cpp session.cpp)
target_link_libraries(asterion_market PRIVATE asterion_market_settings asterion_market_acquisition CLI11::CLI11)
asterion_target_defaults(asterion_market)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(stage ${CMAKE_CURRENT_BINARY_DIR}/setup)
  set(release ${CMAKE_BINARY_DIR}/asterion-market-setup.sh)
  set(layout ${stage}/asterion-market)
  add_custom_command(OUTPUT ${release}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${stage}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${layout}/bin ${layout}/lib
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:asterion_market> ${CMAKE_CURRENT_SOURCE_DIR}/scripts/setup.sh ${layout}/bin/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/ctp-md.so ${CMAKE_BINARY_DIR}/ctp-trader.so ${layout}/lib/
    COMMAND ${CMAKE_COMMAND} -E chdir ${stage} ${CMAKE_COMMAND} -E tar czf payload.tar.gz asterion-market
    COMMAND sh -c "{ cat \"$1\"; echo __ARCHIVE__; cat \"$2\"; } > \"$3\" && chmod 755 \"$3\"" setup
      ${CMAKE_CURRENT_SOURCE_DIR}/scripts/setup.sh ${stage}/payload.tar.gz ${release}
    DEPENDS asterion_market ${CMAKE_CURRENT_SOURCE_DIR}/scripts/setup.sh
      ${CMAKE_BINARY_DIR}/ctp-md.so ${CMAKE_BINARY_DIR}/ctp-trader.so
    VERBATIM)
  add_custom_target(asterion_market_setup DEPENDS ${release})
endif()
