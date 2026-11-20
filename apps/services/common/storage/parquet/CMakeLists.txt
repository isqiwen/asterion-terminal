add_library(asterion_parquet STATIC parquet_engine.cpp)
target_link_libraries(asterion_parquet PRIVATE Asterion::System duckdb::duckdb)
target_include_directories(asterion_parquet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
