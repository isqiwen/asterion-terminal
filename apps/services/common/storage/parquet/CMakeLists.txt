add_library(asterion_bar_parquet STATIC parquet_engine.cpp bar_parquet.cpp table_parquet.cpp)
target_link_libraries(asterion_bar_parquet PUBLIC Asterion::Futures PRIVATE Asterion::System duckdb::duckdb)
target_include_directories(asterion_bar_parquet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
