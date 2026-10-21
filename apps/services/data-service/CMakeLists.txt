add_library(asterion_data_queries STATIC
  historical_margins.cpp
  bar_dataset_source.cpp
  dominant_plan.cpp
  history_coverage.cpp
  history_update.cpp
  table_series.cpp
  table_update.cpp
)
target_include_directories(asterion_data_queries PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/plugins/data/registry)
target_link_libraries(asterion_data_queries PUBLIC asterion_history_files asterion_protocol PRIVATE asterion_chart_indicators asterion_tushare asterion_shfe)
add_library(asterion_data_store STATIC data_store.cpp download_budget.cpp)
target_include_directories(asterion_data_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(asterion_data_store PUBLIC asterion_data_queries PRIVATE asterion_history_providers asterion_shfe)
add_executable(asterion_data_service main.cpp data_host.cpp)
target_compile_features(asterion_data_service PRIVATE cxx_std_20)
target_compile_definitions(asterion_data_service PRIVATE ASTERION_PRODUCT_VERSION="${PROJECT_VERSION}")
target_link_libraries(asterion_data_service PRIVATE asterion_data_store asterion_history_providers CLI11::CLI11)
asterion_target_defaults(asterion_data_service)
