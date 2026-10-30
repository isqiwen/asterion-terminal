add_library(asterion_history_files STATIC
  history_archive.cpp
  history_retirement.cpp
  table_retirement.cpp
  history_minutes.cpp
  history_tables.cpp
  table_observations.cpp
)
target_include_directories(asterion_history_files PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(asterion_history_files PUBLIC asterion_protocol PRIVATE asterion_chart_indicators asterion_bar_parquet asterion_sqlite)
asterion_target_defaults(asterion_history_files)

