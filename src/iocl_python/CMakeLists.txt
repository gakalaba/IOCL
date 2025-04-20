cmake_minimum_required(VERSION 3.10)
project(IOCL_Python LANGUAGES CXX)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Set RPATH properties
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# Find Python development headers
find_package(Python COMPONENTS Interpreter Development REQUIRED)

# Explicitly set pybind11 include directory
set(PYBIND11_DIR "/users/akalaba/redis-chat-transformed/redis-chat/lib/python3.6/site-packages/pybind11")
include_directories(${PYBIND11_DIR}/include)

# Include Python include directories
include_directories(${Python_INCLUDE_DIRS})

# Include the generated protobuf headers and project source directories
include_directories(${CMAKE_BINARY_DIR})
include_directories(${CMAKE_SOURCE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/include)

# Find pybind11 and Protobuf
find_package(pybind11 REQUIRED PATHS ${PYBIND11_DIR}/share/cmake/pybind11)
find_package(Protobuf REQUIRED)

# Generate protobuf files
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS 
    ${CMAKE_SOURCE_DIR}/lib/latency-format.proto
)

# Collect all .cc files in the current directory
file(GLOB SourceFiles "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

# Create a library for the Python module
add_library(iocl_python_lib ${SourceFiles} ${PROTO_SRCS}) # Include PROTO_SRCS here

# Include additional project directories
target_include_directories(iocl_python_lib PRIVATE
    ${PYBIND11_DIR}/include
    ${Python_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/rss
    ${CMAKE_SOURCE_DIR}/store
    ${CMAKE_SOURCE_DIR}/store/benchmark/async 
    ${CMAKE_SOURCE_DIR}/store/common
    ${CMAKE_SOURCE_DIR}/store/common/frontend
    ${PROTO_HDRS} # Include generated headers for the library
)

# Create the Python module
pybind11_add_module(redisstorepython 
    binding.cc 
)

# Link libraries
target_link_libraries(redisstorepython PRIVATE 
    ${PROTOBUF_LIBRARIES}
    ${Python_LIBRARIES}
    iocl_python_lib
    rss
    Common
    StoreCommon
    BenchmarkMicro
    BenchmarkAsync
)

# Ensure include directories are set for the Python module
target_include_directories(redisstorepython PRIVATE
    ${PYBIND11_DIR}/include
    ${Python_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/store/benchmark/async  # Add this line
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}
${PROTO_HDRS} # Include generated headers for the library
)
