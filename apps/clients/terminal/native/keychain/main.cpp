// asterion-keychain: stores data-source credentials in the user's login
// keychain. Kept deliberately small and dependency-free so its code signature
// stays stable across Terminal builds; the keychain grants access by signature.
//   asterion-keychain get <account>     secret on stdout; exit 3 when absent
//   asterion-keychain set <account>     secret on stdin
//   asterion-keychain delete <account>  succeeds when absent
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
namespace {
constexpr auto service = "me.asterion.terminal.data-connection";
constexpr int absent = 3;
struct Ref {
  CFTypeRef value = nullptr;
  ~Ref() {
    if (value)
      CFRelease(value);
  }
};
CFStringRef text(std::string_view value) {
  return CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(value.data()),
                                 static_cast<CFIndex>(value.size()), kCFStringEncodingUTF8, false);
}
CFMutableDictionaryRef query(const std::string& account) {
  auto* dictionary = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
  Ref name{text(service)}, user{text(account)};
  CFDictionarySetValue(dictionary, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(dictionary, kSecAttrService, name.value);
  CFDictionarySetValue(dictionary, kSecAttrAccount, user.value);
  return dictionary;
}
bool valid(std::string_view account) {
  if (account.empty() || account.size() > 160)
    return false;
  for (const auto c : account)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
          c == '_' || c == '/'))
      return false;
  return true;
}
int get(const std::string& account) {
  Ref search{query(account)};
  auto* dictionary = (CFMutableDictionaryRef)search.value;
  CFDictionarySetValue(dictionary, kSecReturnData, kCFBooleanTrue);
  CFDictionarySetValue(dictionary, kSecMatchLimit, kSecMatchLimitOne);
  Ref data;
  const auto status = SecItemCopyMatching(dictionary, &data.value);
  if (status == errSecItemNotFound)
    return absent;
  if (status != errSecSuccess || !data.value)
    return 1;
  const auto* bytes = CFDataGetBytePtr((CFDataRef)data.value);
  std::fwrite(bytes, 1, static_cast<std::size_t>(CFDataGetLength((CFDataRef)data.value)), stdout);
  return std::fflush(stdout) == 0 ? 0 : 1;
}
int set(const std::string& account) {
  const std::string secret{std::istreambuf_iterator<char>(std::cin), {}};
  if (secret.empty() || secret.size() > 4096 || secret.find('\0') != std::string::npos)
    return 1;
  Ref value{CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(secret.data()),
                         static_cast<CFIndex>(secret.size()))};
  Ref search{query(account)};
  Ref update{CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                       &kCFTypeDictionaryValueCallBacks)};
  CFDictionarySetValue((CFMutableDictionaryRef)update.value, kSecValueData, value.value);
  auto status = SecItemUpdate((CFDictionaryRef)search.value, (CFDictionaryRef)update.value);
  if (status == errSecItemNotFound) {
    CFDictionarySetValue((CFMutableDictionaryRef)search.value, kSecValueData, value.value);
    CFDictionarySetValue((CFMutableDictionaryRef)search.value, kSecAttrAccessible,
                         kSecAttrAccessibleWhenUnlockedThisDeviceOnly);
    status = SecItemAdd((CFDictionaryRef)search.value, nullptr);
  }
  return status == errSecSuccess ? 0 : 1;
}
int remove(const std::string& account) {
  Ref search{query(account)};
  const auto status = SecItemDelete((CFDictionaryRef)search.value);
  return status == errSecSuccess || status == errSecItemNotFound ? 0 : 1;
}
} // namespace
int main(int argc, char** argv) {
  if (argc != 3 || !valid(argv[2]))
    return 2;
  const std::string command = argv[1], account = argv[2];
  if (command == "get")
    return get(account);
  if (command == "set")
    return set(account);
  if (command == "delete")
    return remove(account);
  return 2;
}
