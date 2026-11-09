#pragma once
#include "source.hpp"

namespace asterion::warehouse::acquisition {

// Public exchange files have no credential. Origins and paths are selected by
// the built-in source, never supplied by a remote acquisition request.
struct HttpRequest
{
    std::string origin, path, body, content_type;
};

struct HttpReply
{
    int status;
    std::string body;
};

using Fetch = std::function<HttpReply(const HttpRequest&, std::stop_token)>;
HttpReply exchange_fetch(const HttpRequest&, std::stop_token);

class Exchange final : public Source
{
public:
    // One file holds one table's rows.
    struct Parsed
    {
        std::vector<DailyBar> daily;
        std::vector<Settlement> settlement;
        std::vector<PriceTick> price_ticks;
    };

    Exchange(std::string exchange, std::stop_token, Fetch = exchange_fetch);
    bool supports(std::string_view table, std::string_view exchange) const override;
    std::vector<DailyBar> daily(const std::string&, const std::string&) override;
    std::vector<Settlement> settlement(const std::string&, const std::string&) override;
    std::vector<PriceTick> price_ticks(const std::string&, const std::string&) override;

private:
    Parsed read(const std::string& table, const std::string& exchange, const std::string& day);
    std::string exchange_;
    std::stop_token stop_;
    Fetch fetch_;
    std::string archive_year_, archive_;
};

} // namespace asterion::warehouse::acquisition
