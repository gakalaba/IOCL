file(GLOB SourceFiles "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

add_executable(iocl_craq-test ${SourceFiles})
target_include_directories(iocl_craq-test PUBLIC ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR})

target_link_libraries(iocl_craq-test
    PRIVATE
        ReplicationIOCL_CRAQ
        ReplicationCRAQ
        ReplicationCommon
        StoreStrong
        StoreCommon
        StoreCommonBackend
        Common
        gtest
        gtest_main
)