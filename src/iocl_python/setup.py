from setuptools import setup

setup(
    name="redisstore",          # distribution name
    version="0.1.0",
    packages=["redisstore"],    # actual folder on disk
    include_package_data=True,
    package_data={
        "redisstore": ["*.so"],
    },
)