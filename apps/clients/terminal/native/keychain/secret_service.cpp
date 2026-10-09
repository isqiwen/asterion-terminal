// asterion-keychain: stores data-source credentials in the user's login
// keyring through the desktop's Secret Service. libsecret belongs to the
// desktop, so it is loaded when the helper runs rather than linked; without
// it or without an unlocked keyring every command fails.
//   asterion-keychain get <account>     secret on stdout; exit 3 when absent
//   asterion-keychain set <account>     secret on stdin
//   asterion-keychain delete <account>  succeeds when absent
#include <cstdio>
#include <dlfcn.h>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
namespace {
constexpr int absent = 3;
// The C ABI of libsecret-1: secret-schema.h and secret-password.h.
struct SecretSchemaAttribute {
  const char* name;
  int type;
};
struct SecretSchema {
  const char* name;
  int flags;
  SecretSchemaAttribute attributes[32];
  int reserved;
  void* reserved_pointers[7];
};
// Variadic arguments are attribute name/value pairs ended by a null pointer.
using Lookup = char* (*)(const SecretSchema*, void* cancellable, void** error, ...);
using Store = int (*)(const SecretSchema*, const char* collection, const char* label,
                      const char* password, void* cancellable, void** error, ...);
using Clear = int (*)(const SecretSchema*, void* cancellable, void** error, ...);
using Free = void (*)(char*);
const SecretSchema schema{"me.asterion.terminal.data-connection", 0, {{"account", 0}}, 0, {}};
constexpr const char* end = nullptr;

template <typename Function> Function symbol(void* library, const char* name) {
  return reinterpret_cast<Function>(::dlsym(library, name));
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
int get(void* library, const std::string& account) {
  const auto lookup = symbol<Lookup>(library, "secret_password_lookup_sync");
  const auto release = symbol<Free>(library, "secret_password_free");
  if (!lookup || !release)
    return 1;
  void* error = nullptr;
  char* secret = lookup(&schema, nullptr, &error, "account", account.c_str(), end);
  if (error)
    return 1;
  if (!secret)
    return absent;
  const std::string_view text(secret);
  const bool written =
      std::fwrite(text.data(), 1, text.size(), stdout) == text.size() && std::fflush(stdout) == 0;
  release(secret);
  return written ? 0 : 1;
}
int set(void* library, const std::string& account) {
  const std::string secret{std::istreambuf_iterator<char>(std::cin), {}};
  if (secret.empty() || secret.size() > 4096 || secret.find('\0') != std::string::npos)
    return 1;
  const auto store = symbol<Store>(library, "secret_password_store_sync");
  if (!store)
    return 1;
  void* error = nullptr;
  // A null collection is the login keyring; a matching item is replaced.
  return store(&schema, nullptr, "Asterion Terminal credential", secret.c_str(), nullptr, &error,
               "account", account.c_str(), end) &&
                 !error
             ? 0
             : 1;
}
int remove(void* library, const std::string& account) {
  const auto clear = symbol<Clear>(library, "secret_password_clear_sync");
  if (!clear)
    return 1;
  void* error = nullptr;
  (void)clear(&schema, nullptr, &error, "account", account.c_str(), end);
  return error ? 1 : 0;
}
} // namespace
int main(int argc, char** argv) {
  if (argc != 3 || !valid(argv[2]))
    return 2;
  const std::string command = argv[1], account = argv[2];
  if (command != "get" && command != "set" && command != "delete")
    return 2;
  // Never unloaded: the process exits as soon as the command returns.
  void* library = ::dlopen("libsecret-1.so.0", RTLD_NOW);
  if (!library)
    return 1;
  if (command == "get")
    return get(library, account);
  if (command == "set")
    return set(library, account);
  return remove(library, account);
}
