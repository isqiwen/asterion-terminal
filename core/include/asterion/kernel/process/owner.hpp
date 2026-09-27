#pragma once
#include <cstdint>
#include <memory>
namespace asterion {
std::uint64_t current_process_id() noexcept;
// A managed child must stop if its supervising process disappears.
class ProcessOwner {
public:
    explicit ProcessOwner(std::uint64_t pid);
    ~ProcessOwner();
    bool alive() const noexcept;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
}
