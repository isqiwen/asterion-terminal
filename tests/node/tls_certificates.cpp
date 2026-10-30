// Test identities only: writes <name>.crt and <name>.key into the directory for
// each name and prints "<name> <fingerprint>" lines. Never installed.
#include <asterion/kernel/ipc/tcp_channel.hpp>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::cerr << "usage: asterion_test_certificates <directory> <name>...\n";
        return 2;
    }
    const std::filesystem::path directory(argv[1]);
    for (int i = 2; i < argc; ++i)
        std::cout << argv[i] << ' '
                  << asterion::ipc::create_identity(
                         directory / (std::string(argv[i]) + ".crt"),
                         directory / (std::string(argv[i]) + ".key"),
                         argv[i])
                  << '\n';
}
