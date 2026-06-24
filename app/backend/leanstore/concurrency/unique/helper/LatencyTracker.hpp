#ifndef CHANGE_DETECTOR_HPP
#define CHANGE_DETECTOR_HPP

#include <algorithm>
#include <cstdint>

namespace mean
{
struct LatencyTracker {
    static constexpr uint64_t WINDOW = 10000; // tune to your workload
    uint64_t above = 0;
    uint64_t total = 0;
    double last_pressure = 0.0;
    bool valid = false; 

    void record(uint64_t latency_us, uint64_t target_us) {
        total++;
        if (latency_us > target_us)
            above++;
    }

    void recompute() {
        double violation_rate = static_cast<double>(above) / total;
        // positive = good (below p99 target), negative = bad (above target)
        last_pressure = (0.01 - violation_rate) * 100.0;
        above = 0;
        total = 0;
    }
};
}  // namespace mean

#endif