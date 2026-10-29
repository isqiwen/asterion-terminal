add_executable(asterion_recorder main.cpp recording.cpp recorder.cpp reader.cpp compaction.cpp database.cpp settings.cpp)
# The catalog job is shared with the market service.
target_include_directories(asterion_recorder PRIVATE ${CMAKE_SOURCE_DIR}/apps/services/market)
target_link_libraries(asterion_recorder PRIVATE asterion_ctp asterion_protocol asterion_bar_parquet duckdb::duckdb CLI11::CLI11)
asterion_target_defaults(asterion_recorder)
