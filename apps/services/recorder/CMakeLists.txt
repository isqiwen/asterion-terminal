add_executable(asterion_recorder main.cpp recording.cpp recorder.cpp reader.cpp compaction.cpp database.cpp settings.cpp)
# The catalog job is shared with the market service.
target_include_directories(asterion_recorder PRIVATE ${CMAKE_SOURCE_DIR}/apps/services/market)
target_link_libraries(asterion_recorder PRIVATE asterion_ctp asterion_protocol asterion_bar_parquet duckdb::duckdb CLI11::CLI11)
asterion_target_defaults(asterion_recorder)

# The release for a recording host: one self-extracting script, setup.sh with
# the installation (bin/ lib/ config/) appended as a tar.gz after a line
# "__ARCHIVE__". Built only when asked for: cmake --build … --target asterion_recorder_setup
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(stage ${CMAKE_CURRENT_BINARY_DIR}/setup)
  set(release ${CMAKE_BINARY_DIR}/asterion-recorder-setup.sh)
  set(layout ${stage}/asterion-recorder)
  add_custom_command(OUTPUT ${release}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${stage}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${layout}/bin ${layout}/lib ${layout}/config
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:asterion_recorder> ${CMAKE_CURRENT_SOURCE_DIR}/scripts/setup.sh ${layout}/bin/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/ctp-md.so ${CMAKE_BINARY_DIR}/ctp-trader.so ${layout}/lib/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/config/recorder.json ${CMAKE_CURRENT_SOURCE_DIR}/config/logging.json ${layout}/config/
    COMMAND ${CMAKE_COMMAND} -E chdir ${stage} ${CMAKE_COMMAND} -E tar czf payload.tar.gz asterion-recorder
    COMMAND sh -c "{ cat \"$1\"; echo __ARCHIVE__; cat \"$2\"; } > \"$3\" && chmod 755 \"$3\"" setup
      ${CMAKE_CURRENT_SOURCE_DIR}/scripts/setup.sh ${stage}/payload.tar.gz ${release}
    DEPENDS asterion_recorder ${CMAKE_CURRENT_SOURCE_DIR}/scripts/setup.sh ${CMAKE_CURRENT_SOURCE_DIR}/config/recorder.json
      ${CMAKE_CURRENT_SOURCE_DIR}/config/logging.json
      ${CMAKE_BINARY_DIR}/ctp-md.so ${CMAKE_BINARY_DIR}/ctp-trader.so
    VERBATIM)
  add_custom_target(asterion_recorder_setup DEPENDS ${release})
endif()
