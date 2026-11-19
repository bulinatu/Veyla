#pragma once

#include <memory>
#include <typeindex>
#include <unordered_map>

namespace brp::next {

// Minimal service locator used by the application host. It is deliberately
// small: one instance per interface, no lifetime magic, no global state.
class ServiceRegistry {
public:
    template <typename T>
    void Set(std::shared_ptr<T> service) {
        services_[std::type_index(typeid(T))] = std::move(service);
    }

    template <typename T>
    std::shared_ptr<T> Get() const {
        const auto it = services_.find(std::type_index(typeid(T)));
        if (it == services_.end()) {
            return nullptr;
        }
        return std::static_pointer_cast<T>(it->second);
    }

    template <typename T>
    bool Has() const {
        return services_.find(std::type_index(typeid(T))) != services_.end();
    }

    void Clear() { services_.clear(); }

private:
    std::unordered_map<std::type_index, std::shared_ptr<void>> services_;
};

}  // namespace brp::next
