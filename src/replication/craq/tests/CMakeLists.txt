file(GLOB SourceFiles "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

add_executable(craq-test ${SourceFiles})
target_include_directories(craq-test PUBLIC ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR})