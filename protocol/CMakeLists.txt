set(ASTERION_PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(ASTERION_PROTO_GEN ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${ASTERION_PROTO_GEN})
set(ASTERION_PROTO_SOURCES)
file(GLOB ASTERION_PROTO_FILES CONFIGURE_DEPENDS ${ASTERION_PROTO_ROOT}/asterion/*.proto)
foreach(schema health domain trading node market lab recording dataset acquisition strategy)
  add_custom_command(
    OUTPUT ${ASTERION_PROTO_GEN}/asterion/${schema}.pb.cc ${ASTERION_PROTO_GEN}/asterion/${schema}.pb.h
    COMMAND protobuf::protoc --proto_path=${ASTERION_PROTO_ROOT} --cpp_out=${ASTERION_PROTO_GEN} ${ASTERION_PROTO_ROOT}/asterion/${schema}.proto
    DEPENDS ${ASTERION_PROTO_FILES} protobuf::protoc VERBATIM)
  list(APPEND ASTERION_PROTO_SOURCES ${ASTERION_PROTO_GEN}/asterion/${schema}.pb.cc)
endforeach()
# Python IPC bindings use the same locked protoc as the C++ transport.
set(ASTERION_PYTHON_PROTOCOL)
foreach(schema lab market recording dataset acquisition strategy domain health trading)
  set(output "${CMAKE_BINARY_DIR}/generated-python/asterion/${schema}_pb2.py")
  add_custom_command(OUTPUT ${output}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated-python"
    COMMAND protobuf::protoc --proto_path=${ASTERION_PROTO_ROOT} --python_out=${CMAKE_BINARY_DIR}/generated-python ${ASTERION_PROTO_ROOT}/asterion/${schema}.proto
    DEPENDS ${ASTERION_PROTO_ROOT}/asterion/${schema}.proto)
  list(APPEND ASTERION_PYTHON_PROTOCOL ${output})
endforeach()
add_custom_target(asterion_python_protocol ALL DEPENDS ${ASTERION_PYTHON_PROTOCOL})

add_library(asterion_protocol STATIC
  ${ASTERION_PROTO_SOURCES}
  src/trading.cpp
  src/health.cpp
  src/market.cpp
)
target_include_directories(asterion_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${ASTERION_PROTO_GEN})
target_link_libraries(asterion_protocol PUBLIC Asterion::Futures protobuf::libprotobuf)

