file(GLOB SourceFiles "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

add_library(BenchmarkMicro ${SourceFiles})
target_include_directories(BenchmarkMicro PUBLIC ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR})
target_link_libraries(BenchmarkMicro BenchmarkCommon StoreCommonFrontend)
