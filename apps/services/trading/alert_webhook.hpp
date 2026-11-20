#pragma once
#include <asterion/base/serialization.hpp>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>

namespace asterion::trading {

struct WebhookTarget
{
    std::string origin;
    std::string path;
};

WebhookTarget parse_webhook_url(std::string_view url);

// Asynchronous, deduplicated HTTP POST alert sender owned by the trading host.
// Callers enqueue on a bounded in-memory queue and never wait on network I/O.
class AlertWebhook final
{
public:
    AlertWebhook(std::string account, std::optional<std::string> url);
    ~AlertWebhook();
    AlertWebhook(const AlertWebhook&) = delete;
    AlertWebhook& operator=(const AlertWebhook&) = delete;

    void notify(
        std::string_view kind,
        std::string key,
        std::string summary,
        Json details = Json::object()) noexcept;
    void clear(std::string_view key) noexcept;
    void close() noexcept;

private:
    void run(std::stop_token stop);

    std::string account_;
    std::string origin_;
    std::string path_;
    bool enabled_ = false;
    std::atomic<bool> closing_{false};
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::set<std::string, std::less<>> seen_;
    std::deque<std::string> queue_;
    bool done_ = false;
    std::condition_variable drained_;
    std::jthread worker_;
};

} // namespace asterion::trading
