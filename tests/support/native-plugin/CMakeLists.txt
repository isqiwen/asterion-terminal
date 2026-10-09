cmake_minimum_required(VERSION 3.24)
project(asterion_independent_plugin_tests LANGUAGES C)
# Can be built independently: only the public SDK headers are required.
set(ASTERION_PLUGIN_SDK "${CMAKE_CURRENT_LIST_DIR}/../../../bindings/plugin/include" CACHE PATH "Public plugin SDK headers")
foreach(variant good newer bad_abi missing_entry empty_table)
  add_library(plugin_fixture_${variant} MODULE fixture.c)
  target_include_directories(plugin_fixture_${variant} PRIVATE "${ASTERION_PLUGIN_SDK}")
  set_target_properties(plugin_fixture_${variant} PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${variant}")
  if(APPLE)
    set_target_properties(plugin_fixture_${variant} PROPERTIES SUFFIX ".dylib")
  endif()
endforeach()
target_compile_definitions(plugin_fixture_bad_abi PRIVATE BAD_ABI=999)
target_compile_definitions(plugin_fixture_missing_entry PRIVATE MISSING_ENTRY=1)
target_compile_definitions(plugin_fixture_empty_table PRIVATE EMPTY_TABLE=1)

target_compile_definitions(plugin_fixture_newer PRIVATE FIXTURE_VERSION="2.0.0")

find_package(Threads REQUIRED)
add_library(plugin_fixture_risk MODULE risk_fixture.c)
target_link_libraries(plugin_fixture_risk PRIVATE Threads::Threads)
target_include_directories(plugin_fixture_risk PRIVATE "${ASTERION_PLUGIN_SDK}")
set_target_properties(plugin_fixture_risk PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/risk")
if(APPLE)
  set_target_properties(plugin_fixture_risk PROPERTIES SUFFIX ".dylib")
endif()
