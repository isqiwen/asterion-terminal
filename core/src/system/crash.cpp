#include <asterion/system/crash.hpp>
#include "crash_report.hpp"
#include <asterion/system/log.hpp>
#include <cpptrace/basic.hpp>
#include <limits.h>
#include <asterion/system/durable_file.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/version.hpp>
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace asterion {
namespace {

int output = -1;
int frames_output = -1;
char reporter[PATH_MAX];
char frames_path[PATH_MAX];
char partial_frames_path[PATH_MAX];

void write_bytes(int descriptor, const char* text, std::size_t length)
{
    while (length) {
        const auto written = ::write(descriptor, text, length);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return;
        text += written;
        length -= written;
    }
}

void write_text(const char* text, std::size_t length)
{
    write_bytes(output, text, length);
}

void write_text(const char* text)
{
    write_text(text, std::strlen(text));
}

void write_hex(unw_word_t value)
{
    char buffer[2 * sizeof(value)];
    auto position = sizeof(buffer);
    do {
        buffer[--position] = "0123456789abcdef"[value % 16];
        value /= 16;
    } while (value);
    write_text("0x");
    write_text(buffer + position, sizeof(buffer) - position);
}

void fatal_signal(int signal, siginfo_t*, void* context)
{
    const char* name = "unknown";
    switch (signal) {
        case SIGSEGV:
            name = "SIGSEGV";
            break;
        case SIGABRT:
            name = "SIGABRT";
            break;
        case SIGBUS:
            name = "SIGBUS";
            break;
        case SIGILL:
            name = "SIGILL";
            break;
        case SIGFPE:
            name = "SIGFPE";
            break;
    }
    write_text("fatal signal: ");
    write_text(name);
    write_text("\nstack (address function+offset):\n");
    unw_cursor_t cursor;
    if (unw_init_local2(&cursor, static_cast<unw_context_t*>(context), UNW_INIT_SIGNAL_FRAME)
        >= 0) {
        // Bound output when a corrupted stack cannot be unwound to its end.
        for (int frame = 0; frame < 128; ++frame) {
            unw_word_t address = 0, offset = 0;
            if (unw_get_reg(&cursor, UNW_REG_IP, &address) < 0 || !address)
                break;
            cpptrace::safe_object_frame object{};
            cpptrace::get_safe_object_frame(frame == 0 ? address : address - 1, &object);
            write_bytes(frames_output, reinterpret_cast<const char*>(&object), sizeof(object));
            write_hex(address);
            char function[512]{};
            if (unw_get_proc_name(&cursor, function, sizeof(function), &offset) == 0) {
                write_text(" ");
                write_text(function);
                write_text("+");
                write_hex(offset);
            }
            write_text("\n");
            if (unw_step(&cursor) <= 0)
                break;
        }
    }
    write_text("modules (/proc/self/maps):\n");
    const auto maps = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (maps >= 0) {
        char buffer[4096];
        for (ssize_t size; (size = ::read(maps, buffer, sizeof(buffer))) > 0;)
            write_text(buffer, static_cast<std::size_t>(size));
        ::close(maps);
    }
    ::fsync(output);
    ::fsync(frames_output);
    if (::rename(partial_frames_path, frames_path) == 0 && ::_Fork() == 0) {
        ::execl(reporter, reporter, frames_path, static_cast<char*>(nullptr));
        ::_exit(127);
    }
    // SA_RESETHAND restores the signal's fatal default; never resume trading.
    ::raise(signal);
}

void terminate()
{
    write_text("std::terminate");
    if (const auto exception = std::current_exception()) {
        try {
            std::rethrow_exception(exception);
        } catch (const std::exception& error) {
            write_text(": ");
            write_text(error.what());
        } catch (...) {
            write_text(": non-standard exception");
        }
    }
    write_text("\n");
    std::abort();
}

} // namespace

void install_crash_logging(const std::filesystem::path& directory, std::string_view program)
{
    create_directories_durably(directory);
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".frames")
            continue;
        try {
            write_crash_report(entry.path());
        } catch (const std::exception& error) {
            log::warn("crash report pending: {}", error.what());
        }
    }
    cpptrace::safe_object_frame warmup{};
    cpptrace::get_safe_object_frame(
        reinterpret_cast<cpptrace::frame_ptr>(&install_crash_logging),
        &warmup);
    if (!cpptrace::can_get_safe_object_frame())
        throw std::runtime_error("signal-safe object lookup is unavailable");
    const auto file = directory
        / (std::string(program) + "-" + std::to_string(::getpid()) + "-" + unique_process_id()
            + "-crash.log");
    write_file_durably(
        file,
        "Asterion " + std::string(build::description) + "\nprogram: " + std::string(program)
            + "\nexecutable: " + current_executable().string() + "\n");
    output = ::open(file.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
    if (output < 0)
        throw std::runtime_error("cannot open crash log: " + file.string());
    const auto helper = current_executable().parent_path() / "asterion_crash_reporter";
    if (!std::filesystem::is_regular_file(helper))
        throw std::runtime_error("crash reporter is missing: " + helper.string());
    const auto copy_path = [](const std::filesystem::path& path, char (&destination)[PATH_MAX]) {
        const auto text = path.string();
        if (text.size() >= PATH_MAX)
            throw std::runtime_error("crash diagnostic path is too long");
        std::memcpy(destination, text.c_str(), text.size() + 1);
    };
    copy_path(helper, reporter);
    copy_path(file.string() + ".frames", frames_path);
    copy_path(file.string() + ".frames.tmp", partial_frames_path);
    write_file_durably(partial_frames_path, "");
    frames_output = ::open(partial_frames_path, O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
    if (frames_output < 0)
        throw std::runtime_error("cannot open crash frame file");
    std::atexit([] {
        ::unlink(partial_frames_path);
    });
    struct sigaction action{};
    action.sa_sigaction = fatal_signal;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    ::sigemptyset(&action.sa_mask);
    for (const auto signal : {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE}) {
        if (::sigaction(signal, &action, nullptr) != 0)
            throw std::runtime_error("cannot install crash signal handler");
    }
    std::set_terminate(terminate);
}

} // namespace asterion
