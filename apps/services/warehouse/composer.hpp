#pragma once
#include "composition.hpp"
#include <condition_variable>
#include <thread>

namespace asterion::warehouse {

// Composes fixed source versions, coalescing requests without blocking acquisition.
class Composer
{
public:
    explicit Composer(std::filesystem::path data);
    void request(const SourceVersions& committed);
    void stop();

private:
    void run(std::stop_token stop);

    const std::filesystem::path data_;
    Store research_;
    std::mutex mutex_;
    std::condition_variable_any changed_;
    SourceVersions versions_;
    bool requested_ = true;
    std::jthread thread_;
};

} // namespace asterion::warehouse
