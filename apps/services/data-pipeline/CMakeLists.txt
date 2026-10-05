add_executable(asterion-data-pipeline main.cpp)
target_compile_features(asterion-data-pipeline PRIVATE cxx_std_20)
target_compile_definitions(asterion-data-pipeline PRIVATE ASTERION_PRODUCT_VERSION="${PROJECT_VERSION}")
target_link_libraries(asterion-data-pipeline PRIVATE asterion_history_files asterion_history_providers asterion_task_client asterion_data_client CLI11::CLI11)
