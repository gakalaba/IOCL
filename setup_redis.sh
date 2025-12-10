#!/bin/bash

# Setup script for redis-chat-transformed
# Exit on error
set -e

echo "Starting redis-chat-transformed setup..."

# Clone the repository (if needed)
echo "Checking repository..."
if [ ! -d "/users/akalaba/IOCL/redis-chat-transformed" ]; then
    cd /users/akalaba/IOCL
    git clone https://github.com/0austinli4/redis-chat-transformed.git
fi

# Navigate to the target directory
cd /users/akalaba/IOCL/redis-chat-transformed

# Remove existing redis-chat directory if it exists
echo "Cleaning up existing installation..."
rm -rf redis-chat

# Install python3-venv
echo "Installing python3-venv..."
apt-get update
apt-get install -y python3-venv
apt-get install python3.6-dev

# Create virtual environment
echo "Creating virtual environment..."
python3 -m venv redis-chat

# Activate virtual environment
echo "Activating virtual environment..."
source redis-chat/bin/activate

# Upgrade pip
echo "Upgrading pip..."
pip install --upgrade pip

# Install requirements
echo "Installing requirements..."
pip install -r requirements.txt

# Install additional packages
echo "Installing additional packages..."
pip install numpy
pip install pybind11

echo "Setup complete! To activate the environment, run:"
echo "source /users/akalaba/IOCL/redis-chat-transformed/redis-chat/bin/activate"