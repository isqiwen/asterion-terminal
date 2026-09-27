#include <asterion/kernel/process/artifact.hpp>
#include <openssl/evp.h>
#include <fstream>
#include <array>
#include <memory>
#include <stdexcept>
namespace asterion {
HostPlatform current_platform() {
  HostPlatform result;
#ifdef _WIN32
  result.os = "windows";
#elif defined(__APPLE__)
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
HostPlatform artifact_platform(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::array<unsigned char, 64> h{};
  if (!input.read(reinterpret_cast<char*>(h.data()), h.size()))
    throw std::invalid_argument("artifact is not a native executable");
  auto u32 = [&](std::size_t offset) {
    return std::uint32_t(h[offset]) | (std::uint32_t(h[offset + 1]) << 8) |
           (std::uint32_t(h[offset + 2]) << 16) | (std::uint32_t(h[offset + 3]) << 24);
  };
  if (h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F' && h[4] == 2 && h[5] == 1) {
    const auto machine = unsigned(h[18]) | (unsigned(h[19]) << 8);
    if (machine == 62)
      return {"linux", "x86_64"};
    if (machine == 183)
      return {"linux", "arm64"};
  } else if (u32(0) == 0xfeedfacf) {
    if (u32(4) == 0x01000007)
      return {"macos", "x86_64"};
    if (u32(4) == 0x0100000c)
      return {"macos", "arm64"};
  } else if (h[0] == 'M' && h[1] == 'Z') {
    const auto offset = u32(60);
    if (offset > 1024 * 1024)
      throw std::invalid_argument("invalid PE header");
    input.seekg(offset);
    input.read(reinterpret_cast<char*>(h.data()), 6);
    if (input && u32(0) == 0x4550) {
      const auto machine = unsigned(h[4]) | (unsigned(h[5]) << 8);
      if (machine == 0x8664)
        return {"windows", "x86_64"};
      if (machine == 0xaa64)
        return {"windows", "arm64"};
    }
  }
  throw std::invalid_argument("unsupported native executable platform");
}
std::string sha256_bytes(std::string_view bytes) {
  std::array<unsigned char, 32> digest{};
  unsigned int size = 0;
  if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1 ||
      size != digest.size())
    throw std::runtime_error("SHA-256 hashing failed");
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (auto byte : digest) {
    result += hex[byte >> 4];
    result += hex[byte & 15];
  }
  return result;
}
std::string sha256_file(const std::filesystem::path& path) {
  if (!std::filesystem::is_regular_file(path) || std::filesystem::is_symlink(path) ||
      std::filesystem::file_size(path) > 128 * 1024 * 1024)
    throw std::invalid_argument("artifact must be a regular file of at most 128 MiB");
  std::ifstream input(path, std::ios::binary);
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                  EVP_MD_CTX_free);
  if (!input || !context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("cannot hash artifact");
  std::array<char, 65536> buffer{};
  while (input.read(buffer.data(), buffer.size()) || input.gcount())
    if (EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(input.gcount())) !=
        1)
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
