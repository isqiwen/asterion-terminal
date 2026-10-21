cmake_minimum_required(VERSION 3.25)
project(Asterion VERSION 0.1.0 LANGUAGES CXX)
if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Darwin|Linux)$")
  message(FATAL_ERROR "Asterion supports macOS and Linux x86_64 only")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  message(FATAL_ERROR "Linux currently supports x86_64 only")
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/services/service_fingerprint.py"
  OUTPUT_VARIABLE ASTERION_SERVICE_SOURCE_SHA256 OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/services/service_fingerprint.py" --files
  OUTPUT_VARIABLE ASTERION_SERVICE_INPUTS OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ASTERION_SERVICE_INPUTS})
# Detect added/deleted inputs as well as edits to the existing source files.
file(GLOB_RECURSE ASTERION_SERVICE_TREE CONFIGURE_DEPENDS
  core/* protocol/* plugins/* bindings/* apps/services/* apps/management/* apps/common/* conan/* cmake/* scripts/node/*)
file(WRITE "${CMAKE_BINARY_DIR}/service-source.sha256" "${ASTERION_SERVICE_SOURCE_SHA256}\n")

# Ship the maintained defaults verbatim; services never create their own policy.
foreach(name task logging)
  configure_file(config/${name}.json config/${name}.json COPYONLY)
endforeach()

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include(CTest)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(OpenSSL CONFIG REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(ctp CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(duckdb CONFIG REQUIRED)
find_package(Iconv REQUIRED)

set(ASTERION_SANITIZERS "" CACHE STRING "Optional address,undefined or thread sanitizer")
set(ASTERION_NODE_HEADERS "" CACHE PATH "Prepared Electron Node-API include directory")
# Keep deployment and test artifact locations independent of source nesting.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
include(cmake/CompilerOptions.cmake)

add_subdirectory(core)
add_subdirectory(protocol)
add_subdirectory(plugins)
add_subdirectory(apps)
add_subdirectory(bindings)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
