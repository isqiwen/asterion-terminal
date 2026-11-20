#include <asterion/system/crash_report.hpp>
#include <asterion/system/durable_file.hpp>
#include <asterion/version.hpp>
#include <cpptrace/basic.hpp>
#include <cstring>
#include <chrono>
#include <cstdint>
#include <fmt/chrono.h>
#include <fstream>
#include <stdexcept>

namespace asterion {

void write_crash_report(const std::filesystem::path& frames)
{
    auto destination = frames;
    destination.replace_extension(".report.log");
    if (std::filesystem::exists(destination))
        return;
    std::ifstream input(frames, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot read crash record: " + frames.string());
    std::string header;
    std::getline(input, header);
    if (header != "Asterion " + std::string(build::description))
        throw std::runtime_error("crash symbols belong to another build: " + frames.string());
    std::string details;
    if (!std::getline(input, details, '\0') || input.eof())
        throw std::runtime_error("incomplete crash record: " + frames.string());
    std::int64_t crashed_at;
    if (!input.read(reinterpret_cast<char*>(&crashed_at), sizeof(crashed_at)))
        throw std::runtime_error("missing crash timestamp: " + frames.string());
    const std::chrono::sys_time<std::chrono::milliseconds> timestamp{
        std::chrono::milliseconds{crashed_at}
    };
    details += fmt::format("crashed_at: {:%Y-%m-%dT%H:%M:%S}Z\n", timestamp);
    cpptrace::object_trace trace;
    cpptrace::safe_object_frame frame;
    while (input.read(reinterpret_cast<char*>(&frame), sizeof(frame))) {
        if (trace.frames.size() >= 128
            || !std::memchr(frame.object_path, '\0', sizeof(frame.object_path)))
            throw std::runtime_error("invalid crash frame: " + frames.string());
        trace.frames.push_back(frame.resolve());
    }
    if (input.gcount() != 0 || trace.empty())
        throw std::runtime_error("incomplete crash frames: " + frames.string());
    replace_file_durably(
        destination,
        header + "\n" + details + "\n" + trace.resolve().to_string() + "\n");
}

} // namespace asterion
