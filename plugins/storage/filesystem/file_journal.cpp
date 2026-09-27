#include "file_journal.hpp"
#include <fstream>
#include <algorithm>
#include <cerrno>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
namespace asterion {
namespace {
std::string name(std::size_t index) { std::ostringstream out; out << std::setfill('0') << std::setw(8) << index << ".json"; return out.str(); }
void durable_write(const std::filesystem::path& path, const std::string& contents) {
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("无法写入模拟交易日志");
    DWORD written = 0;
    bool ok = WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) && written == contents.size() && FlushFileBuffers(file);
    CloseHandle(file);
#else
    int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (file < 0) throw std::runtime_error("无法写入模拟交易日志");
    std::size_t offset = 0;
    while (offset < contents.size()) {
        auto n = ::write(file, contents.data() + offset, contents.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        offset += static_cast<std::size_t>(n);
    }
    bool ok = offset == contents.size() && ::fsync(file) == 0;
    ::close(file);
#endif
    if (!ok) throw std::runtime_error("模拟交易日志写入或同步失败，请重新打开会话恢复");
}
}
FileJournal::FileJournal(std::filesystem::path directory) : directory_(std::move(directory)) {
    if (!directory_.is_absolute()) throw std::invalid_argument("交易记录目录必须是绝对路径");
}
FileJournal::~FileJournal() { stop(); }
PluginDescriptor FileJournal::descriptor() const { return {"asterion.storage.filesystem-journal", PluginKind::storage, plugin_contract_version, {}}; }
void FileJournal::start() {
    if (handle_ != -1) throw std::logic_error("存储插件已经启动");
    if (!std::filesystem::is_directory(directory_)) throw std::invalid_argument("请选择已存在的交易记录目录");
    if (std::filesystem::is_symlink(directory_ / "writer.lock")) throw std::invalid_argument("拒绝符号链接锁文件");
#ifdef _WIN32
    const auto handle = CreateFileW((directory_ / "writer.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("交易记录目录被占用或不可写");
    handle_ = reinterpret_cast<std::intptr_t>(handle);
#else
    const auto handle = ::open((directory_ / "writer.lock").c_str(), O_RDWR | O_CREAT, 0600);
    if (handle < 0) throw std::runtime_error("交易记录目录不可写");
    if (::flock(handle, LOCK_EX | LOCK_NB) != 0) { ::close(handle); throw std::runtime_error("交易记录目录已被另一个终端占用"); }
    handle_ = handle;
#endif
    try {
        poisoned_ = false; count_ = read().size(); bytes_ = 0;
        for (std::size_t i = 0; i < count_; ++i) bytes_ += std::filesystem::file_size(directory_ / name(i));
    } catch (...) { stop(); throw; }
}
void FileJournal::stop() noexcept {
    if (handle_ == -1) return;
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
    ::close(static_cast<int>(handle_));
#endif
    handle_ = -1;
}
std::vector<Json> FileJournal::read() const {
    if (handle_ == -1) throw std::logic_error("存储插件未启动");
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
        const auto file = entry.path().filename().string();
        if (file == "writer.lock" || file == "pending.tmp") {
            if (!entry.is_regular_file() || entry.is_symlink()) throw std::invalid_argument("无效交易目录内部文件");
            continue;
        }
        if (!entry.is_regular_file() || entry.is_symlink() || entry.path().extension() != ".json") throw std::invalid_argument("交易目录含未知文件，请使用专用目录");
        paths.push_back(entry.path());
    }
    if (paths.size() > 20001) throw std::invalid_argument("交易日志超出本版容量");
    std::sort(paths.begin(), paths.end());
    std::vector<Json> result;
    std::uintmax_t total = 0;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        const auto size = std::filesystem::file_size(paths[i]); total += size;
        if (paths[i].filename() != name(i) || size > (i == 0 ? 16ULL * 1024 * 1024 : 65536ULL) || total > 64 * 1024 * 1024) throw std::invalid_argument("交易日志不连续或文件过大");
        std::ifstream input(paths[i], std::ios::binary); if (!input) throw std::runtime_error("无法读取交易日志");
        std::string raw{std::istreambuf_iterator<char>(input), {}};
        if (raw.size() != size || input.bad()) throw std::runtime_error("交易日志读取不完整");
        result.push_back(parse_json(raw, 16 * 1024 * 1024));
    }
    return result;
}
void FileJournal::append(const Json& record) {
    if (handle_ == -1 || poisoned_) throw std::runtime_error("交易存储不可写，请关闭并重新打开会话");
    if (count_ >= 20001) throw std::invalid_argument("交易日志已达容量上限");
    const auto data = record.dump();
    if (data.size() > (count_ == 0 ? 16ULL * 1024 * 1024 : 65536ULL) || bytes_ + data.size() > 64 * 1024 * 1024) throw std::invalid_argument("交易日志记录过大");
    try {
        const auto temporary = directory_ / "pending.tmp";
        if (std::filesystem::is_symlink(temporary)) throw std::invalid_argument("拒绝符号链接临时日志");
        durable_write(temporary, data);
        const auto target = directory_ / name(count_);
        if (std::filesystem::exists(target)) throw std::runtime_error("交易日志序号冲突");
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("交易日志提交失败");
#else
        std::filesystem::rename(temporary, target);
        const int dir = ::open(directory_.c_str(), O_RDONLY);
        if (dir < 0) throw std::runtime_error("无法同步交易记录目录");
        const int result = ::fsync(dir); ::close(dir);
        if (result != 0) throw std::runtime_error("交易记录目录同步失败");
#endif
        ++count_; bytes_ += data.size();
    } catch (...) { poisoned_ = true; throw; }
}
}
