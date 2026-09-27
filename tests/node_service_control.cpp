#include "node_program.hpp"
#include "node_service.hpp"
#include <CLI/CLI.hpp>
#include <iostream>
int main(int argc, char **argv) {
  CLI::App app{"Native user-service control acceptance"};
  std::string operation, executable, root, endpoint, name, source, expected;
  std::uint64_t pid = 0;
  app.add_option("--operation", operation)
      ->required()
      ->check(CLI::IsMember({"install", "stop", "replace", "upgrade", "inspect",
                             "verify-stopped"}));
  app.add_option("--executable", executable)->required();
  app.add_option("--root", root)->required();
  app.add_option("--endpoint", endpoint)->required();
  app.add_option("--name", name)->required();
  app.add_option("--pid", pid);
  app.add_option("--source", source);
  app.add_option("--expected", expected);
  argv = app.ensure_utf8(argv);
  CLI11_PARSE(app, argc, argv);
  try {
    if (name.rfind("me.asterion.acceptance.", 0) != 0)
      throw std::invalid_argument(
          "test requires an isolated acceptance service name");
    auto path = [](const std::string &value) {
      return std::filesystem::path(std::u8string(value.begin(), value.end()));
    };
    if (operation == "verify-stopped")
      asterion::terminal::verify_node_service_stopped(
          path(executable), path(root), endpoint, name);
    else if (operation == "inspect")
      std::cout << asterion::terminal::inspect_node_program(
                       path(source), path(executable), path(root))
                       .dump()
                << '\n';
    else if (operation == "upgrade")
      asterion::terminal::upgrade_node_service(
          path(source), path(executable), path(root), endpoint, expected, name);
    else if (operation == "replace")
      asterion::terminal::replace_node_program(path(source), path(executable),
                                               path(root), expected);
    else if (operation == "install")
      asterion::terminal::install_node_service(path(executable), path(root),
                                               endpoint, name);
    else
      asterion::terminal::stop_node_service(path(executable), path(root),
                                            endpoint, pid, name);
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
