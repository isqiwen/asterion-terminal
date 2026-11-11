add_library(asterion_warehouse_store STATIC records.cpp store.cpp reader.cpp composition.cpp composer.cpp)
target_include_directories(asterion_warehouse_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(asterion_warehouse_store PUBLIC asterion_protocol PRIVATE asterion_parquet duckdb::duckdb)
asterion_target_defaults(asterion_warehouse_store)
find_package(ZLIB CONFIG REQUIRED)
find_package(tinyxml2 CONFIG REQUIRED)
add_library(asterion_warehouse_acquisition STATIC
  acquisition/source.cpp acquisition/archive.cpp acquisition/values.cpp acquisition/tushare.cpp acquisition/sina.cpp acquisition/exchanges.cpp acquisition/guotai.cpp acquisition/manager.cpp)
target_link_libraries(asterion_warehouse_acquisition PUBLIC asterion_warehouse_store PRIVATE httplib::httplib Iconv::Iconv ZLIB::ZLIB tinyxml2::tinyxml2)
asterion_target_defaults(asterion_warehouse_acquisition)
add_library(asterion_warehouse_settings STATIC settings.cpp)
target_link_libraries(asterion_warehouse_settings PUBLIC asterion_system)
asterion_target_defaults(asterion_warehouse_settings)
add_executable(asterion_warehouse main.cpp service.cpp)
target_link_libraries(asterion_warehouse PRIVATE asterion_warehouse_settings asterion_warehouse_acquisition CLI11::CLI11)
asterion_target_defaults(asterion_warehouse)

asterion_setup_script(asterion_warehouse_setup_script)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_dependencies(asterion_warehouse asterion_crash_reporter)
  set(stage ${CMAKE_CURRENT_BINARY_DIR}/setup)
  set(release ${CMAKE_BINARY_DIR}/Asterion-Warehouse-${PROJECT_VERSION}-linux-x86_64.sh)
  set(layout ${stage}/asterion-warehouse)
  add_custom_command(OUTPUT ${release}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${stage}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${layout}/bin ${layout}/config
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:asterion_crash_reporter> ${layout}/bin/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/config/logging.json ${layout}/config/
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:asterion_warehouse> ${CMAKE_CURRENT_BINARY_DIR}/setup.sh ${layout}/bin/
    COMMAND ${CMAKE_COMMAND} -E chdir ${stage} ${CMAKE_COMMAND} -E tar czf payload.tar.gz asterion-warehouse
    COMMAND sh -c "{ cat \"$1\"; echo __ARCHIVE__; cat \"$2\"; } > \"$3\" && chmod 755 \"$3\"" setup
      ${CMAKE_CURRENT_BINARY_DIR}/setup.sh ${stage}/payload.tar.gz ${release}
    DEPENDS asterion_warehouse_setup_script asterion_crash_reporter ${CMAKE_CURRENT_SOURCE_DIR}/config/logging.json asterion_warehouse ${CMAKE_CURRENT_BINARY_DIR}/setup.sh
    VERBATIM)
  add_custom_target(asterion_warehouse_setup DEPENDS ${release})
endif()
