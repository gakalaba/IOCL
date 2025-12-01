#include <chrono>
#include <cstdint>

using SteadyClock = std::chrono::steady_clock;

inline uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               SteadyClock::now().time_since_epoch())
        .count();
}