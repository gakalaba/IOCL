add_subdirectory(common)
add_subdirectory(retwis)
add_subdirectory(micro)

file(GLOB SourceFiles "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

# Create the BenchmarkAsync shared library
add_library(BenchmarkAsync SHARED ${SourceFiles})
target_include_directories(BenchmarkAsync
    PUBLIC
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}" # Add the current source directory
)
target_link_libraries(BenchmarkAsync BenchmarkCommon BenchmarkRetwis BenchmarkMicro StoreStrong Common)

add_executable(benchmark ${SourceFiles})
target_include_directories(benchmark PUBLIC ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR})
target_link_libraries(benchmark BenchmarkCommon BenchmarkRetwis BenchmarkMicro StoreStrong Common)
