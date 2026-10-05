#pragma once
#include <asterion/foundation/error.hpp>
#include <set>
#include <string>
#include <utility>
namespace asterion::tasks {
// Owned by the task I/O thread. Two worker completions retain their own capacity; a public result
// read cannot occupy either slot. Reject excess/duplicate work before loading owned inputs.
class VerificationSlots {
public:
  class Lease {
  public:
    Lease(Lease&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), completion_(other.completion_),
          id_(std::move(other.id_)) {}
    Lease(const Lease&) = delete;
    ~Lease() {
      if (owner_) {
        owner_->ids(completion_).erase(id_);
      }
    }

  private:
    friend class VerificationSlots;
    Lease(VerificationSlots* owner, bool completion, const std::string& id)
        : owner_(owner), completion_(completion), id_(id) {}
    VerificationSlots* owner_;
    bool completion_;
    std::string id_;
  };
  Lease acquire(bool completion, const std::string& id) {
    // Allocate the lease identity before publishing its reservation.
    Lease lease(nullptr, completion, id);
    auto& active = ids(completion);
    if (active.size() >= (completion ? 2U : 1U) || !active.insert(id).second)
      throw Error(ErrorCode::resource_exhausted, "task verification capacity is busy");
    lease.owner_ = this;
    return lease;
  }

private:
  std::set<std::string>& ids(bool completion) { return completion ? completions_ : reads_; }
  std::set<std::string> completions_, reads_;
};
} // namespace asterion::tasks
