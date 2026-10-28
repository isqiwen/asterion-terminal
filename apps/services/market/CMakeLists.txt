add_executable(asterion_market main.cpp session.cpp recorder.cpp)
target_link_libraries(asterion_market PRIVATE asterion_ctp asterion_protocol asterion_bar_parquet duckdb::duckdb CLI11::CLI11)
asterion_target_defaults(asterion_market)
