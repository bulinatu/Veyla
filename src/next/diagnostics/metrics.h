#pragma once

#include "core/interfaces.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace brp::next {

// Bounded rolling window. Percentiles sort a copy of the live samples; the
// window is small enough that this is cheap at UI refresh rates.
class RollingStats {
public:
    explicit RollingStats(std::size_t capacity = 1024);

    void Add(double value);
    void Reset();

    std::size_t Count() const { return count_; }
    double Mean() const;
    double Min() const;
    double Max() const;
    double Percentile(double quantile) const;
    const std::deque<double>& Samples() const { return samples_; }

private:
    std::size_t capacity_;
    std::deque<double> samples_;
    std::size_t count_ = 0;
    double sum_ = 0.0;
};

class DiagnosticsCollector final : public IDiagnosticsService {
public:
    void AddFrame(double frame_ms, double decode_ms, double sr_ms, double fg_ms,
                  double present_ms);
    void AddLate(std::uint64_t count) { late_ += count; }
    void AddDropped(std::uint64_t count) { dropped_ += count; }
    void AddGenerated(std::uint64_t count) { generated_ += count; }

    void Tick(double seconds) override;
    DiagnosticsSnapshot Snapshot() const override;
    Status WriteReport(const std::string& path) const override;

    double UptimeSeconds() const { return uptime_seconds_; }

private:
    RollingStats frames_{2048};
    RollingStats decode_{1024};
    RollingStats sr_{1024};
    RollingStats fg_{1024};
    RollingStats present_{1024};
    std::uint64_t late_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t generated_ = 0;
    double uptime_seconds_ = 0.0;
};

}  // namespace brp::next
