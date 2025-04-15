from setuptools import setup, find_packages

setup(
    name='iocl',
    version='0.1.0',
    description='iocl library',
    packages=find_packages('iocl_python'),
    package_dir={'': 'iocl_python'},
    install_requires=[
        'numpy',  # Add any dependencies your library needs
    ],
    classifiers=[
        'Development Status :: 3 - Alpha',
    ],
    keywords='distributed-systems research database',
    python_requires='>=3.6',
)