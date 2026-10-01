#include "tushare.hpp"
#include <httplib.h>
#include <stdexcept>
namespace asterion::tushare {
Post https_transport() {
  return [](const std::string& body, std::stop_token stop) {
    if (stop.stop_requested())
      throw std::runtime_error("Tushare download cancelled");
    httplib::SSLClient client("api.tushare.pro", 443);
    client.enable_server_certificate_verification(true);
    client.set_follow_location(false);
    client.set_connection_timeout(10);
    client.set_read_timeout(20);
    client.set_write_timeout(10);
    client.set_max_timeout(30000);
    client.set_payload_max_length(8 * 1024 * 1024);
    std::stop_callback cancel(stop, [&] { client.stop(); });
    std::string contents;
    const auto response =
        client.Post("/", httplib::Headers{}, body, "application/json",
                    [&](const char* bytes, std::size_t size) {
                      if (stop.stop_requested() || size > 8 * 1024 * 1024 - contents.size())
                        return false;
                      contents.append(bytes, size);
                      return true;
                    });
    if (stop.stop_requested())
      throw std::runtime_error("Tushare download cancelled");
    if (!response)
      throw RequestError(
          AccessFailure::network,
          "Tushare HTTPS request failed (network, certificate, timeout or response limit)");
    if (response->status != 200)
      throw RequestError(response->status == 429 ? AccessFailure::rate_limit
                                                 : AccessFailure::network,
                         "Tushare HTTP status " + std::to_string(response->status));
    return contents;
  };
}
} // namespace asterion::tushare
