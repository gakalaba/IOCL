cmake_minimum_required(VERSION 3.10)
project(IOCL_Python LANGUAGES CXX)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# RPATH settings
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# Python
find_package(PythonInterp 3.6 REQUIRED)
find_package(PythonLibs 3.6 REQUIRED)

# Explicitly add the Python include directory to fix Python.h not found
include_directories(${Python_INCLUDE_DIRS} /usr/include/python3.6m)

# Link the Python libraries
link_libraries(${PYTHON_LIBRARIES})

# pybind11
set(PYBIND11_DIR "/users/akalaba/redis-chat-transformed/redis-chat/lib/python3.6/site-packages/pybind11")
find_package(pybind11 REQUIRED PATHS ${PYBIND11_DIR}/share/cmake/pybind11)
include_directories(${PYBIND11_DIR}/include)

# Protobuf
find_package(Protobuf REQUIRED)

# Proto files used by iocl_python
set(PROTO_FILES
    ${CMAKE_SOURCE_DIR}/lib/latency-format.proto
    ${CMAKE_SOURCE_DIR}/replication/vr/vr-proto.proto
    ${CMAKE_SOURCE_DIR}/replication/common/request.proto
)

# Generate C++ sources and headers from all proto files
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

# Collect all .cc files in the current directory
file(GLOB SourceFiles "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

# Create a library containing sources + generated protos
add_library(iocl_python_lib ${SourceFiles} ${PROTO_SRCS})

# Include directories for the library
target_include_directories(iocl_python_lib PRIVATE
    ${CMAKE_BINARY_DIR}           # protobuf generated headers
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/rss
    ${CMAKE_SOURCE_DIR}/store
    ${CMAKE_SOURCE_DIR}/store/benchmark/async
    ${CMAKE_SOURCE_DIR}/store/common
    ${CMAKE_SOURCE_DIR}/store/common/frontend
    ${PYBIND11_DIR}/include
    ${Python_INCLUDE_DIRS}
)

# Link libraries for the library
target_link_libraries(iocl_python_lib PRIVATE
    ${PROTOBUF_LIBRARIES}
    rss
    Common
    StoreCommon
    BenchmarkMicro
    BenchmarkAsync
)

# Create Python module
pybind11_add_module(redisstorepython binding.cc)

# Link the library + dependencies
target_link_libraries(redisstorepython PRIVATE
    iocl_python_lib
    ${PROTOBUF_LIBRARIES}
    ${Python_LIBRARIES}
    rss
    Common
    StoreCommon
    BenchmarkMicro
    BenchmarkAsync
)

# Include directories for Python module
target_include_directories(redisstorepython PRIVATE
    ${CMAKE_BINARY_DIR}           # protobuf generated headers
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/store/benchmark/async
    ${PYBIND11_DIR}/include
    ${Python_INCLUDE_DIRS}
)
