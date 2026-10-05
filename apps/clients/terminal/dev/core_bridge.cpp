#include <asterion/terminal.h>
#include <CLI/CLI.hpp>
#include <iostream>
#include <memory>
#include <string>

int main(int argc, char** argv) {
  CLI::App app{"Terminal development transport; JSON lines on stdin/stdout"};
  app.set_version_flag("--version", "Asterion Terminal bridge 0.1.0");
  argv = app.ensure_utf8(argv);
  CLI11_PARSE(app, argc, argv);
  char* failure = nullptr;
  std::unique_ptr<void, decltype(&asterion_terminal_destroy)> runtime(
      asterion_terminal_create(&failure), asterion_terminal_destroy);
  std::unique_ptr<char, decltype(&asterion_terminal_free)> detail(failure, asterion_terminal_free);
  if (!runtime) {
    std::cerr
        << (detail
                ? detail.get()
                : R"({"error":{"code":"resource_exhausted","message":"Cannot allocate the C++ initialization error"}})")
        << std::endl;
    return 1;
  }
  std::string line;
  while (std::getline(std::cin, line)) {
    std::unique_ptr<char, decltype(&asterion_terminal_free)> response(
        asterion_terminal_call(runtime.get(), line.c_str()), asterion_terminal_free);
    if (!response)
      return 1;
    std::cout << response.get() << std::endl;
  }
}
