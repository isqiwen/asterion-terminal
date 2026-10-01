#pragma once
// Test-only credential store: never touches the login keychain.
#include "data_connections.hpp"
#include <map>
namespace asterion::test {
class MemoryCredentials final : public terminal::CredentialStore {
public:
  std::map<std::string, std::string> items;
  std::optional<std::string> load(const std::string& account) override {
    const auto found = items.find(account);
    return found == items.end() ? std::nullopt : std::optional(found->second);
  }
  void store(const std::string& account, const std::string& secret) override {
    items[account] = secret;
  }
  void erase(const std::string& account) override { items.erase(account); }
};
} // namespace asterion::test
