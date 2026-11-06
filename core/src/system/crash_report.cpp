#include "crash_report.hpp"
#include <asterion/system/durable_file.hpp>
#include <asterion/version.hpp>
#include <cpptrace/basic.hpp>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace asterion {

void write_crash_report(const std::filesystem::path& frames)
{
    auto raw = frames;
    raw.replace_extension();
    const auto destination = raw.parent_path() / (raw.stem().string() + ".report.log");
    if (std::filesystem::exists(destination))
        return;
    std::ifstream log(raw);
    std::string header;
    std::getline(log, header);
    if (header != "Asterion " + std::string(build::description))
        throw std::runtime_error("crash symbols belong to another build: " + raw.string());
    const std::string details{std::istreambuf_iterator<char>(log), {}};
    std::ifstream input(frames, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot read crash frames: " + frames.string());
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
    replace_file_durably(destination, header + "\n" + trace.resolve().to_string() + "\n" + details);
}

} // namespace asterion
