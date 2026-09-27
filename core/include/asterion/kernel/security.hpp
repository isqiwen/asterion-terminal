#pragma once
#include <asterion/foundation/id.hpp>
#include <map>
#include <mutex>
#include <set>
namespace asterion {
// Policy is controlled by the trusted host, never by plugin-supplied JSON.
// This gates cooperating components; it is NOT native-code sandboxing.
class AccessPolicy final {
public:
    void grant(std::string principal, std::string capability) {
        validate_id(principal); validate_id(capability);
        std::lock_guard lock(mutex_);
        if (sealed_) throw Error(ErrorCode::conflict, "access policy sealed");
        grants_[std::move(principal)].insert(std::move(capability));
    }
    void seal() { std::lock_guard lock(mutex_); sealed_ = true; }
    void revoke(const std::string& principal) { std::lock_guard lock(mutex_); grants_.erase(principal); }
    void require(const std::string& principal, const std::string& capability) const {
        std::lock_guard lock(mutex_);
        const auto found = grants_.find(principal);
        if (found == grants_.end() || !found->second.contains(capability)) throw Error(ErrorCode::permission_denied, "capability denied");
    }
private:
    mutable std::mutex mutex_;
    std::map<std::string, std::set<std::string>> grants_;
    bool sealed_ = false;
};
}
