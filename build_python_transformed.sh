#!/bin/bash

# Exit on error and print each command
set -e
set -x

echo "=== Starting IOCL build and run process ==="

# ---- CONFIGURATION ----
PROJECT_ROOT="/users/akalaba/IOCL"
TRANSFORMED_DIR="$PROJECT_ROOT/redis-chat-transformed"
VENV_DIR="$TRANSFORMED_DIR/redis-chat"
BUILD_DIR="$PROJECT_ROOT/src/build"
PYTHON_EXEC="$VENV_DIR/bin/python"
PYBIND_DIR="$VENV_DIR/lib/python3.6/site-packages/pybind11/share/cmake/pybind11"
OUTPUT_SO="$PROJECT_ROOT/src/build/iocl_python/redisstorepython.cpython-36m-x86_64-linux-gnu.so"
DESTINATION="$PROJECT_ROOT/src/iocl_python/redisstore"


# ---- CHECKS ----
if [ ! -d "$PROJECT_ROOT" ]; then
    echo "ERROR: PROJECT_ROOT does not exist: $PROJECT_ROOT"
    exit 1
fi

# ---- Activate virtual environment ----
echo "Activating Python virtual environment..."
source "$VENV_DIR/bin/activate"

# ---- Ensure build directory exists ----
echo "Ensuring build directory exists..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# ---- Clean folder ----
echo "Cleaning previous build artifacts..."
rm -rf ./*

# ---- Configure CMake ----
echo "Configuring with CMake..."
cmake .. -DPYTHON_EXECUTABLE="$PYTHON_EXEC" \
         -DPYBIND11_PYTHON_VERSION=3.6 \
         -Dpybind11_DIR="$PYBIND_DIR"

# ---- Build ----
echo "Building project..."
make clean
make -j$(nproc)

# ---- Export library path ----
echo "Updating LD_LIBRARY_PATH..."
export LD_LIBRARY_PATH="$BUILD_DIR/store/benchmark/async:$BUILD_DIR/lib:$BUILD_DIR/rss:$LD_LIBRARY_PATH"

# ---- Copy built library ----
echo "Copying built redisstore Python shared object..."
mkdir -p "$DESTINATION"
cp -f "$OUTPUT_SO" "$DESTINATION"

# ---- Install Python package ----
echo "Installing Python package iocl_python..."
cd "$PROJECT_ROOT/src/iocl_python"
pip install -e .

echo "=== Build & setup complete! ==="
