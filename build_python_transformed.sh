#!/bin/bash

# Build and run script for IOCL project
# Exit on error
set -e

echo "Starting IOCL build and run process..."

# Navigate to redis-chat-transformed directory
sudo -s
cd /users/akalaba/redis-chat-transformed

# Activate virtual environment
echo "Activating virtual environment..."
source redis-chat/bin/activate

# Navigate to build directory and clean it
echo "Cleaning build directory..."
cd /users/akalaba/IOCL/src/build && rm -rf *

# Configure with CMake
echo "Configuring with CMake..."
cmake .. -DPYTHON_EXECUTABLE=/users/akalaba/redis-chat-transformed/redis-chat/bin/python \
-DPYBIND11_PYTHON_VERSION=3.6 \
-Dpybind11_DIR=/users/akalaba/redis-chat-transformed/redis-chat/lib/python3.6/site-packages/pybind11/share/cmake/pybind11

# Clean and build
echo "Building project..."
make clean
make -j

# Set library path
echo "Setting LD_LIBRARY_PATH..."
export LD_LIBRARY_PATH=/users/akalaba/IOCL/src/build/store/benchmark/async:/users/akalaba/IOCL/src/build/lib:/users/akalaba/IOCL/src/build/rss:$LD_LIBRARY_PATH

# Copy the built library
echo "Copying built library..."
cp -f /users/akalaba/IOCL/src/build/iocl_python/redisstorepython.cpython-36m-x86_64-linux-gnu.so /users/akalaba/IOCL/src/iocl_python/redisstore

# Install iocl_python package
echo "Installing iocl_python package..."
cd ../iocl_python
pip install -e .

# Run experiments
# echo "Running experiments..."
# cd ../../
# python3 ./experiments/run_multiple_experiments.py experiments/configs/1shard_transformed_test_multiple_.json

echo "Process complete!"