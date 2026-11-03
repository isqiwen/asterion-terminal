#include <asterion/system/process/artifact.hpp>
#include <openssl/evp.h>
#include <fstream>
#include <array>
#include <memory>
#include <stdexcept>

namespace asterion {

HostPlatform current_platform()
{
    HostPlatform result;
#if defined(__APPLE__)
    result.os = "macos";
#else
    result.os = "linux";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    result.arch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    result.arch = "x86_64";
#else
#error Unsupported deployment architecture
#endif
    return result;
}

std::string sha256_bytes(std::string_view bytes)
{
    std::array<unsigned char, 32> digest{};
    unsigned int size = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1
        || size != digest.size())
        throw std::runtime_error("SHA-256 hashing failed");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto byte : digest) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

std::string sha256_file(const std::filesystem::path& path)
{
    if (!std::filesystem::is_regular_file(path) || std::filesystem::is_symlink(path)
        || std::filesystem::file_size(path) > max_artifact_bytes)
        throw std::invalid_argument("artifact must be a regular file within the Agent size limit");
    std::ifstream input(path, std::ios::binary);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
        EVP_MD_CTX_new(),
        EVP_MD_CTX_free);
    if (!input || !context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("cannot hash artifact");
    std::array<char, 65536> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount())
        if (EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(input.gcount()))
            != 1)
            throw std::runtime_error("artifact hashing failed");
    if (!input.eof())
        throw std::runtime_error("artifact read failed");
    std::array<unsigned char, 32> digest{};
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &size) != 1 || size != digest.size())
        throw std::runtime_error("artifact hashing failed");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto byte : digest) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

} // namespace asterion
