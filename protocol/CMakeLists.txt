set(ASTERION_PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(ASTERION_PROTO_GEN ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${ASTERION_PROTO_GEN})
set(ASTERION_PROTO_SOURCES)
file(GLOB ASTERION_PROTO_FILES CONFIGURE_DEPENDS ${ASTERION_PROTO_ROOT}/asterion/*.proto)
foreach(schema health domain resource trading node manager market task_state task research_assessment research research_project research_variable research_statistics research_evaluation research_sample research_strategy research_logic research_batch research_model backtest factor data data_service analytics account_review material)
  add_custom_command(
    OUTPUT ${ASTERION_PROTO_GEN}/asterion/${schema}.pb.cc ${ASTERION_PROTO_GEN}/asterion/${schema}.pb.h
    COMMAND protobuf::protoc --proto_path=${ASTERION_PROTO_ROOT} --cpp_out=${ASTERION_PROTO_GEN} ${ASTERION_PROTO_ROOT}/asterion/${schema}.proto
    DEPENDS ${ASTERION_PROTO_FILES} protobuf::protoc VERBATIM)
  list(APPEND ASTERION_PROTO_SOURCES ${ASTERION_PROTO_GEN}/asterion/${schema}.pb.cc)
endforeach()
# Python IPC bindings use the same locked protoc as the C++ transport.
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/generated-python/asterion/analytics_pb2.py"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated-python"
  COMMAND protobuf::protoc --proto_path=${ASTERION_PROTO_ROOT} --python_out=${CMAKE_BINARY_DIR}/generated-python ${ASTERION_PROTO_ROOT}/asterion/analytics.proto
  DEPENDS ${ASTERION_PROTO_ROOT}/asterion/analytics.proto)
add_custom_target(asterion_analytics_python DEPENDS "${CMAKE_BINARY_DIR}/generated-python/asterion/analytics_pb2.py")

add_library(asterion_protocol STATIC
  ${ASTERION_PROTO_SOURCES}
  src/trading.cpp
  src/health.cpp
  src/market.cpp
  src/bar_dataset.cpp
  src/resource.cpp
  src/research_project.cpp
  src/research_variable.cpp
  src/research_statistics.cpp
  src/research_evaluation.cpp
  src/research_sample.cpp
  src/research_strategy.cpp
  src/research_logic.cpp
  src/research_batch.cpp
  src/research_assessment.cpp
  src/research_model.cpp
  src/research_model_time.cpp
  src/research_model_decision.cpp
  src/research_model_replay.cpp
  src/observation.cpp
  src/trading_facts.cpp
  src/account_review.cpp
  src/material.cpp
  src/task.cpp
  src/task_execution.cpp
  src/backtest.cpp
  src/backtest_result.cpp
  src/factor.cpp
  src/data.cpp
  src/table_series.cpp
)
target_include_directories(asterion_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${ASTERION_PROTO_GEN})
target_link_libraries(asterion_protocol PUBLIC Asterion::Domain protobuf::libprotobuf)
add_library(asterion_data_client STATIC src/data_client.cpp)
target_link_libraries(asterion_data_client PUBLIC asterion_protocol)

add_library(asterion_service_discovery STATIC src/service_discovery.cpp)
target_link_libraries(asterion_service_discovery PUBLIC asterion_protocol)
add_library(asterion_retention_release STATIC src/retention_release.cpp)
target_link_libraries(asterion_retention_release PUBLIC asterion_service_discovery)
asterion_target_defaults(asterion_retention_release)
