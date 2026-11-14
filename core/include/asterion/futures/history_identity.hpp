#pragma once
#include <string>

namespace asterion {

// Stable history identity. Exchange symbols alone may repeat across decades.
struct HistoryIdentity
{
    // "<venue>/<product>/<YYYY-MM>", as research and Warehouse name a contract;
    // throws for an unsupported exchange, product or month.
    std::string key() const;

    std::string venue, product, delivery_month;
};

} // namespace asterion
