- compile flags have been added to CMakeList.txt
- to compile:
	1. `cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..`
	2. `make -j`
- add flag to config `perf_profile`
- scripts take in flag to run binaries with `sudo perf record -F 999 -g -- ./your_binary args...`
- can then look at performance with `sudo perf report -i perf.data`
