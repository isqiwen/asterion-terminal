#include <asterion/version.hpp>
#include "terminal_application.hpp"
#include <asterion/base/serialization.hpp>
#include <CLI/CLI.hpp>
#include <iostream>
#include <mutex>
#include <string>

namespace {

std::string error(const char* code, const char* message)
{
    return nlohmann::json{{"error", {{"code", code}, {"message", message}}}}.dump();
}

std::mutex output;

void reply(std::int64_t id, const std::string& value)
{
    std::lock_guard lock(output);
    std::cout << "{\"id\":" << id << ",\"reply\":" << value << '}' << std::endl;
}

std::string failure(std::exception_ptr e)
{
    try {
        std::rethrow_exception(e);
    } catch (const std::exception& error_) {
        return error(asterion::error_name(asterion::classify(error_)).data(), error_.what());
    } catch (...) {
        return error("internal_error", "native core call failed");
    }
}

} // namespace

// Each input line is {"id": n, "request": <native request>}; each output line is
// {"id": n, "reply": <native reply>}, in the order the requests complete, as the
// Electron main process sees them: one slow service never holds back another.
int main(int argc, char** argv)
{
    CLI::App app{"Terminal development transport; JSON lines on stdin/stdout"};
    app.set_version_flag(
        "--version",
        std::string("Asterion Terminal bridge ") + asterion::build::description);
    argv = app.ensure_utf8(argv);
    CLI11_PARSE(app, argc, argv);
    try {
        asterion::terminal::Application runtime;
        std::string line;
        while (std::getline(std::cin, line)) {
            std::int64_t id = 0;
            try {
                const auto envelope = asterion::parse_json(line);
                id = envelope.at("id").get<std::int64_t>();
                (void)runtime.request(
                    envelope.at("request").dump(),
                    [id](asterion::Payload value, std::exception_ptr e) noexcept {
                        try {
                            reply(id, e ? failure(e) : *value);
                        } catch (...) {
                        }
                    });
            } catch (...) {
                reply(id, failure(std::current_exception()));
            }
        }
    } catch (const std::exception& e) {
        std::cerr << error(asterion::error_name(asterion::classify(e)).data(), e.what())
                  << std::endl;
        return 1;
    } catch (...) {
        std::cerr << error("internal_error", "native core initialization failed") << std::endl;
        return 1;
    }
}
