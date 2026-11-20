#include "alert_webhook.hpp"
#include <asterion/system/log.hpp>
#include <httplib.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <stdexcept>

namespace asterion::trading {

WebhookTarget parse_webhook_url(std::string_view url)
{
    if (url.empty() || url.size() > 1024 || std::ranges::any_of(url, [](unsigned char c) {
            return c == '\0' || std::isspace(c) != 0;
        }))
        throw std::invalid_argument("invalid alert webhook URL");
    std::size_t scheme_end = 0;
    if (url.starts_with("https://"))
        scheme_end = 8;
    else if (url.starts_with("http://"))
        scheme_end = 7;
    else
        throw std::invalid_argument("alert webhook URL must start with https:// or http://");
    const auto slash = url.find('/', scheme_end);
    const auto authority = url.substr(
        scheme_end,
        slash == std::string_view::npos ? url.size() - scheme_end : slash - scheme_end);
    if (authority.empty() || authority.front() == ':' || authority.back() == ':')
        throw std::invalid_argument("alert webhook URL needs a host");
    return {
        std::string(slash == std::string_view::npos ? url : url.substr(0, slash)),
        slash == std::string_view::npos ? std::string("/") : std::string(url.substr(slash))
    };
}

AlertWebhook::AlertWebhook(std::string account, std::optional<std::string> url)
    : account_(std::move(account))
{
    if (!url)
        return;
    auto parsed = parse_webhook_url(*url);
    origin_ = std::move(parsed.origin);
    path_ = std::move(parsed.path);
    enabled_ = true;
    worker_ = std::jthread([this](std::stop_token stop) {
        run(std::move(stop));
    });
}

AlertWebhook::~AlertWebhook()
{
    close();
    if (worker_.joinable()) {
        {
            std::unique_lock lock(mutex_);
            drained_.wait_for(lock, std::chrono::seconds(2), [&] {
                return done_;
            });
        }
        worker_.request_stop();
        worker_.join();
    }
}

void AlertWebhook::notify(
    std::string_view kind,
    std::string key,
    std::string summary,
    Json details) noexcept
{
    if (!enabled_ || closing_.load(std::memory_order_relaxed))
        return;
    try {
        const auto time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
                                 .count();
        Json payload = {
            {"service", "asterion-trading"},
            {"account", account_},
            {"kind", kind},
            {"time_ms", time_ms},
            {"text", "[Asterion " + account_ + "] " + std::string(kind) + ": " + summary},
            {"details", std::move(details)}
        };
        auto body = payload.dump();
        {
            std::lock_guard lock(mutex_);
            if (seen_.contains(key))
                return;
            if (seen_.size() >= 4096)
                seen_.erase(seen_.begin());
            seen_.insert(std::move(key));
            if (queue_.size() >= 128) {
                queue_.pop_front();
                log::warn("alert webhook queue full for {}; dropping oldest alert", account_);
            }
            queue_.push_back(std::move(body));
        }
        wake_.notify_one();
    } catch (const std::exception& error) {
        log::warn("alert webhook notification skipped for {}: {}", account_, error.what());
    }
}

void AlertWebhook::clear(std::string_view key) noexcept
{
    if (!enabled_)
        return;
    std::lock_guard lock(mutex_);
    if (const auto found = seen_.find(key); found != seen_.end())
        seen_.erase(found);
}

void AlertWebhook::close() noexcept
{
    {
        std::lock_guard lock(mutex_);
        closing_.store(true, std::memory_order_release);
    }
    wake_.notify_all();
}

void AlertWebhook::run(std::stop_token stop)
{
    struct Finish
    {
        AlertWebhook& self;

        ~Finish()
        {
            {
                std::lock_guard lock(self.mutex_);
                self.done_ = true;
            }
            self.drained_.notify_all();
        }
    } finish{*this};

    constexpr std::array<std::chrono::milliseconds, 3> retry_delays{
        std::chrono::milliseconds(500),
        std::chrono::milliseconds(1500),
        std::chrono::milliseconds(3500),
    };
    while (!stop.stop_requested()) {
        std::string body;
        bool draining = false;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, stop, [&] {
                return !queue_.empty() || closing_.load(std::memory_order_acquire);
            });
            if (stop.stop_requested())
                return;
            if (queue_.empty()) {
                if (closing_.load(std::memory_order_acquire))
                    return;
                continue;
            }
            body = std::move(queue_.front());
            queue_.pop_front();
            draining = closing_.load(std::memory_order_acquire);
        }
        const std::size_t max_attempts = draining ? 1 : (1 + retry_delays.size());
        for (std::size_t attempt = 0; attempt < max_attempts && !stop.stop_requested(); ++attempt) {
            bool retryable = false;
            try {
                httplib::Client client(origin_);
                client.enable_server_certificate_verification(true);
                client.set_follow_location(false);
                client.set_connection_timeout(draining ? 1 : 2);
                client.set_read_timeout(draining ? 1 : 3);
                client.set_write_timeout(draining ? 1 : 3);
                std::stop_callback abort(stop, [&] {
                    client.stop();
                });
                const auto response = client.Post(path_, body, "application/json");
                if (response && response->status >= 200 && response->status < 300)
                    break;
                retryable = !response || response->status == 429 || response->status >= 500;
                log::warn(
                    "alert webhook delivery failed for {} (attempt {}/{}): {}",
                    account_,
                    attempt + 1,
                    max_attempts,
                    response ? "HTTP " + std::to_string(response->status)
                             : httplib::to_string(response.error()));
            } catch (const std::exception& error) {
                retryable = true;
                log::warn(
                    "alert webhook client error for {} (attempt {}/{}): {}",
                    account_,
                    attempt + 1,
                    max_attempts,
                    error.what());
            }
            if (!retryable || attempt + 1 >= max_attempts
                || closing_.load(std::memory_order_acquire))
                break;
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, stop, retry_delays[attempt], [&] {
                return closing_.load(std::memory_order_acquire);
            });
            if (closing_.load(std::memory_order_acquire))
                break;
        }
    }
}

} // namespace asterion::trading
