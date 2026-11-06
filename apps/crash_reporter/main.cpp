#include <asterion/system/crash_report.hpp>
#include <exception>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    try {
        asterion::write_crash_report(argv[1]);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
