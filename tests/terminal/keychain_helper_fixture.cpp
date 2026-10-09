// Test-only process double; never calls Security.framework or a real keychain.
#include <array>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unistd.h>
int main(int argc, char** argv) {
  if (argc != 3)
    return 2;
  const std::string command = argv[1], account = argv[2];
  const auto separator = account.find(':');
  const auto mode = account.substr(0, separator);
  if (separator != std::string::npos)
    std::ofstream(account.substr(separator + 1)) << ::getpid();
  if (mode == "hang" || mode == "closed-output") {
    if (mode == "closed-output")
      ::close(STDOUT_FILENO);
    for (;;)
      ::pause();
  }
  if (mode == "overflow") {
    const std::array<char, 1024> block{};
    for (;;) {
      if (std::fwrite(block.data(), 1, block.size(), stdout) != block.size() || std::fflush(stdout))
        return 1;
    }
  }
  if (mode == "early-exit")
    return 1;
  if (mode == "absent")
    return 3;
  if (command == "get")
    std::cout << "fixture-only-token";
  else if (command == "set") {
    const std::string input{std::istreambuf_iterator<char>(std::cin), {}};
    return input == "fixture-only-token" ? 0 : 1;
  } else if (command != "delete")
    return 2;
  return 0;
}
