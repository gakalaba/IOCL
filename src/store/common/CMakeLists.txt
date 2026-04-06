add_subdirectory(backend)
add_subdirectory(frontend)

file(GLOB SourceFiles "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

add_library(StoreCommon ${SourceFiles} ${PROTO_SRCS} ${PROTO_HDRS})
target_include_directories(StoreCommon PUBLIC ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR})