add_subdirectory(tests)

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${CMAKE_SOURCE_DIR}/replication/common/request.proto iocl_craq-proto.proto)

file(GLOB SourceFiles "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

add_library(ReplicationIOCL_CRAQ ${SourceFiles} ${PROTO_SRCS} ${PROTO_HDRS})
target_include_directories(ReplicationIOCL_CRAQ PUBLIC ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR})
target_link_libraries(ReplicationIOCL_CRAQ ReplicationCommon)