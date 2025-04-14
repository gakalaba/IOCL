from setuptools import setup, Extension
from pybind11.setup_helpers import Pybind11Extension, build_ext
from skbuild import setup

ext_modules = [
    Pybind11Extension(
        "redisstore",
        [
            "iocl_python/binding.cc",
            "store/common/backend/redis_store.cc",
            "store/benchmark/async/bench_client.cc",
        ],
        include_dirs=[
            ".",
            "store",
            "lib",
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
    packages=[],
    python_requires=">=3.6",
    ext_modules=ext_modules,
    cmdclass={"build_ext": build_ext},
    cmake_install_dir=".",
)