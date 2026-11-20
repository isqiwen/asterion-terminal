#include "archive.hpp"
#include <zlib.h>
#include <cstdint>
#include <stdexcept>

namespace asterion::warehouse::acquisition {
namespace {

std::uint32_t integer(std::string_view data, std::size_t at, unsigned size)
{
    if (at > data.size() || size > data.size() - at)
        throw std::invalid_argument("truncated exchange archive");
    std::uint32_t value = 0;
    for (unsigned n = 0; n < size; ++n)
        value |= std::uint32_t(static_cast<unsigned char>(data[at + n])) << (n * 8);
    return value;
}

} // namespace

std::string archive_member(std::string_view zip, std::string_view name)
{
    constexpr std::size_t plain_limit = 32 * 1024 * 1024;
    if (zip.size() < 22)
        throw std::invalid_argument("missing exchange archive directory");
    auto end = zip.size() - 22;
    for (;;) {
        if (integer(zip, end, 4) == 0x06054b50
            && end + 22 + integer(zip, end + 20, 2) == zip.size())
            break;
        if (!end || zip.size() - end > 65557)
            throw std::invalid_argument("missing exchange archive directory");
        --end;
    }
    const auto count = integer(zip, end + 10, 2);
    auto entry = static_cast<std::size_t>(integer(zip, end + 16, 4));
    if (integer(zip, end + 4, 2) || integer(zip, end + 6, 2) || integer(zip, end + 8, 2) != count
        || count > 4096 || entry + integer(zip, end + 12, 4) != end)
        throw std::invalid_argument("unsupported exchange archive layout");
    for (std::uint32_t n = 0; n < count; ++n) {
        if (integer(zip, entry, 4) != 0x02014b50)
            throw std::invalid_argument("invalid exchange archive directory");
        const auto filename = integer(zip, entry + 28, 2);
        const auto next =
            entry + 46 + filename + integer(zip, entry + 30, 2) + integer(zip, entry + 32, 2);
        if (next > end)
            throw std::invalid_argument("truncated exchange archive entry");
        if (zip.substr(entry + 46, filename) == name) {
            const auto flags = integer(zip, entry + 8, 2), method = integer(zip, entry + 10, 2);
            const auto size = integer(zip, entry + 24, 4), compressed = integer(zip, entry + 20, 4);
            const auto local = static_cast<std::size_t>(integer(zip, entry + 42, 4));
            if ((flags & 1) || (method != 0 && method != 8) || size > plain_limit
                || integer(zip, local, 4) != 0x04034b50)
                throw std::invalid_argument("unsupported or oversized exchange archive member");
            const auto begin =
                local + 30 + integer(zip, local + 26, 2) + integer(zip, local + 28, 2);
            if (begin > entry || compressed > entry - begin)
                throw std::invalid_argument("truncated exchange archive member");
            std::string result(size, '\0');
            if (method == 0) {
                if (compressed != size)
                    throw std::invalid_argument("invalid stored archive member size");
                result.assign(zip.substr(begin, size));
            } else {
                struct Inflater
                {
                    ~Inflater()
                    {
                        if (opened)
                            inflateEnd(&stream);
                    }

                    z_stream stream{};
                    bool opened = false;
                } inflater;

                if (inflateInit2(&inflater.stream, -MAX_WBITS) != Z_OK)
                    throw std::runtime_error("cannot open exchange archive");
                inflater.opened = true;
                auto& stream = inflater.stream;
                stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(zip.data() + begin));
                stream.avail_in = compressed;
                stream.next_out = reinterpret_cast<Bytef*>(result.data());
                stream.avail_out = size;
                if (inflate(&stream, Z_FINISH) != Z_STREAM_END || stream.total_out != size
                    || stream.total_in != compressed)
                    throw std::invalid_argument("invalid compressed exchange archive member");
            }
            if (crc32(0, reinterpret_cast<const Bytef*>(result.data()), result.size())
                != integer(zip, entry + 16, 4))
                throw std::invalid_argument("corrupt exchange archive member");
            return result;
        }
        entry = next;
    }
    throw std::invalid_argument("exchange archive does not contain the year's table");
}

} // namespace asterion::warehouse::acquisition
