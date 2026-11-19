#pragma once

#include <string>
#include <utility>

namespace brp::next {

// Small status type: no exceptions in service boundaries, no optional+error
// duplication. Success carries no payload; services use snapshots for data.
class Status {
public:
    Status() = default;

    static Status Ok() { return Status(); }
    static Status Error(std::string message) {
        Status status;
        status.ok_ = false;
        status.message_ = std::move(message);
        return status;
    }

    bool ok() const { return ok_; }
    const std::string& message() const { return message_; }
    explicit operator bool() const { return ok_; }

private:
    bool ok_ = true;
    std::string message_;
};

}  // namespace brp::next
