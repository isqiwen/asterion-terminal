#include <asterion/version.hpp>
#include "terminal_application.hpp"
#include <asterion/base/serialization.hpp>
#include <CLI/CLI.hpp>
#include <iostream>
#include <string>

namespace {

std::string error(const char* code, const char* message)
{
    return nlohmann::json{{"error", {{"code", code}, {"message", message}}}}.dump();
}

std::string call(asterion::terminal::Application& runtime, std::string request)
{
    try {
        asterion::Payload response;
        std::exception_ptr failure;
        runtime
            .request(
                std::move(request),
                [&](asterion::Payload value, std::exception_ptr e) noexcept {
                    response = std::move(value);
                    failure = e;
                })
            .get();
        if (failure)
            std::rethrow_exception(failure);
        return *response;
    } catch (const std::exception& e) {
        return error(asterion::error_name(asterion::classify(e)).data(), e.what());
    } catch (...) {
        return error("internal_error", "native core call failed");
    }
}

} // namespace

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
        while (std::getline(std::cin, line))
            std::cout << call(runtime, std::move(line)) << std::endl;
    } catch (const std::exception& e) {
        std::cerr << error(asterion::error_name(asterion::classify(e)).data(), e.what())
                  << std::endl;
        return 1;
    } catch (...) {
        std::cerr << error("internal_error", "native core initialization failed") << std::endl;
        return 1;
    }
}
