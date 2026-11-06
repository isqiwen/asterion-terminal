#pragma once
#include "store.hpp"
#include <condition_variable>
#include <thread>

namespace asterion::market_data::warehouse {

// Keeps research current on its own thread: it composes at start and after each
// request, requests made meanwhile coalescing into one. Nothing that commits to
// a source waits for it, and its failure holds back only research.
class Composer
{
public:
    explicit Composer(std::filesystem::path data);
    void request();
    void stop();

private:
    void run(std::stop_token stop);

    const std::filesystem::path data_;
    Store research_;
    std::mutex mutex_;
    std::condition_variable_any changed_;
    bool requested_ = true;
    std::jthread thread_;
};

} // namespace asterion::market_data::warehouse
