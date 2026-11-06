#include <asterion/system/crash.hpp>
#include <stdexcept>
#include <string_view>
#include <sys/mman.h>
#include <sys/resource.h>
#include <thread>

[[gnu::noinline]] void crash_site()
{
    const auto memory = ::mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    *static_cast<volatile int*>(memory) = 1;
}

int main(int argc, char** argv)
{
    if (argc != 3)
        return 2;
    const rlimit no_core{0, 0};
    ::setrlimit(RLIMIT_CORE, &no_core);
    asterion::install_crash_logging(argv[1], "crash-test");
    if (std::string_view(argv[2]) == "exception") {
        std::thread([] {
            throw std::runtime_error("fixture uncaught exception");
        }).join();
    } else {
        std::thread(crash_site).join();
    }
}
