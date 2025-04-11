from setuptools import setup, Extension
from pybind11.setup_helpers import Pybind11Extension, build_ext
from skbuild import setup

ext_modules = [
    Pybind11Extension(
        "redisstore",
        [
            "binding.cc",  
            "/users/akalaba/IOCL/src/store/common/backend/redis_store.cc",
            "src/store/benchmark/async/bench_client.cc",
        ],
        include_dirs=[
            "./",  
            "src",  
            "/users/akalaba/IOCL/src", 
            "/users/akalaba/IOCL/src/lib",  # Add this
        ],
        extra_compile_args=["-std=c++17"],
    ),
]

setup(
    name="redistore",
    version="0.1",
    author="Your Name",
    author_email="your.email@example.com",
    description="Python bindings for Redis Store C++ library",
    packages=[],  # or list your Python packages if you have any
    python_requires=">=3.6",
)