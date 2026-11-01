#pragma once
#include "source.hpp"

namespace asterion::market_data::acquisition {

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
    Exchange(std::string exchange, std::stop_token, Fetch = exchange_fetch);
    bool supports(std::string_view table, std::string_view exchange) const override;
    Rows daily(const std::string&, const std::string&) override;
    Rows settlement(const std::string&, const std::string&) override;

private:
    Rows read(const std::string& table, const std::string& exchange, const std::string& day);
    std::string exchange_;
    std::stop_token stop_;
    Fetch fetch_;
    std::string archive_year_, archive_;
};

} // namespace asterion::market_data::acquisition
