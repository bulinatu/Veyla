#include "diagnostics/metrics.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>

namespace brp::next {

RollingStats::RollingStats(std::size_t capacity)
    : capacity_(std::max<std::size_t>(1, capacity)) {}

void RollingStats::Add(double value) {
    if (!std::isfinite(value)) {
        return;
    }
    if (samples_.size() == capacity_) {
        sum_ -= samples_.front();
        samples_.pop_front();
    }
    samples_.push_back(value);
    sum_ += value;
    if (count_ < capacity_) {
        ++count_;
    }
}

void RollingStats::Reset() {
    samples_.clear();
    count_ = 0;
    sum_ = 0.0;
}

double RollingStats::Mean() const {
    return samples_.empty() ? 0.0 : sum_ / static_cast<double>(samples_.size());
}

double RollingStats::Min() const {
    return samples_.empty() ? 0.0 : *std::min_element(samples_.begin(), samples_.end());
}

double RollingStats::Max() const {
    return samples_.empty() ? 0.0 : *std::max_element(samples_.begin(), samples_.end());
}

double RollingStats::Percentile(double quantile) const {
    if (samples_.empty()) {
        return 0.0;
    }
    std::vector<double> sorted(samples_.begin(), samples_.end());
    std::sort(sorted.begin(), sorted.end());
    const double q = std::clamp(quantile, 0.0, 1.0);
    const std::size_t index = static_cast<std::size_t>(
        std::llround(q * static_cast<double>(sorted.size() - 1)));
    return sorted[index];
}

void DiagnosticsCollector::AddFrame(double frame_ms, double decode_ms, double sr_ms,
                                    double fg_ms, double present_ms) {
    frames_.Add(frame_ms);
    decode_.Add(decode_ms);
    sr_.Add(sr_ms);
    fg_.Add(fg_ms);
    present_.Add(present_ms);
}

void DiagnosticsCollector::Tick(double seconds) {
    if (seconds > 0.0 && std::isfinite(seconds)) {
        uptime_seconds_ += seconds;
    }
}

DiagnosticsSnapshot DiagnosticsCollector::Snapshot() const {
    DiagnosticsSnapshot snapshot;
    snapshot.decode_ms = decode_.Mean();
    snapshot.sr_ms = sr_.Mean();
    snapshot.fg_ms = fg_.Mean();
    snapshot.present_ms = present_.Mean();
    snapshot.frame_p50_ms = frames_.Percentile(0.50);
    snapshot.frame_p95_ms = frames_.Percentile(0.95);
    snapshot.frame_p99_ms = frames_.Percentile(0.99);
    snapshot.late_frames = late_;
    snapshot.dropped_frames = dropped_;
    snapshot.generated_frames = generated_;
    return snapshot;
}

Status DiagnosticsCollector::WriteReport(const std::string& path) const {
    if (path.empty()) {
        return Status::Error("report path is empty");
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return Status::Error("cannot write report: " + path);
    }
    const DiagnosticsSnapshot s = Snapshot();
    file << std::fixed << std::setprecision(3);
    file << "BetterRemotePlay Next diagnostics\n";
    file << "uptime_seconds=" << uptime_seconds_ << "\n";
    file << "samples=" << frames_.Count() << "\n";
    file << "decode_ms=" << s.decode_ms << "\n";
    file << "sr_ms=" << s.sr_ms << "\n";
    file << "fg_ms=" << s.fg_ms << "\n";
    file << "present_ms=" << s.present_ms << "\n";
    file << "frame_p50_ms=" << s.frame_p50_ms << "\n";
    file << "frame_p95_ms=" << s.frame_p95_ms << "\n";
    file << "frame_p99_ms=" << s.frame_p99_ms << "\n";
    file << "late_frames=" << s.late_frames << "\n";
    file << "dropped_frames=" << s.dropped_frames << "\n";
    file << "generated_frames=" << s.generated_frames << "\n";
    if (!file) {
        return Status::Error("report write failed: " + path);
    }
    return Status::Ok();
}

}  // namespace brp::next
