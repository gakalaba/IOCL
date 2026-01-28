protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS request.proto)

file(GLOB SourceFiles "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

add_library(ReplicationCommon ${SourceFiles} ${PROTO_SRCS} ${PROTO_HDRS})
target_include_directories(ReplicationCommon PUBLIC ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR})
target_link_libraries(ReplicationCommon PUBLIC rss)

add_dependencies(ReplicationCommon ReplicationCRAQ)
add_dependencies(ReplicationCommon ReplicationVR)
add_dependencies(ReplicationCommon ReplicationIOCL_CT)
add_dependencies(ReplicationCommon StoreStrong)