#include <gtest/gtest.h>
#include "firewall.hpp"
#include <asterion/kernel/process/child.hpp>
#include <fstream>
using namespace asterion;
TEST(Firewall, RequiresOneConcreteSourceAndOwnedRule) {
  for (const auto* source : {"", "0.0.0.0", "::", "224.0.0.1", "0.0.0.0/0", "192.0.2.1/24",
                             "host.example", "1.2.3.4;id", "fe80::1%en0"})
    EXPECT_THROW(node::validate_firewall_source(source), std::invalid_argument);
  EXPECT_NO_THROW(node::validate_firewall_source("192.0.2.1"));
  EXPECT_NO_THROW(node::validate_firewall_source("2001:db8::1"));
  EXPECT_THROW(node::firewall_change("linux", "192.0.2.1", 7442, "foreign-rule", false),
               std::invalid_argument);
  EXPECT_THROW(
      node::firewall_change("macos", "192.0.2.1", 7442, "asterion-" + std::string(32, 'a'), false),
      std::invalid_argument);
}
TEST(Firewall, WindowsRuleIsScopedAndOwned) {
  const auto script = node::firewall_change("windows", "192.0.2.1", 7442,
                                            "asterion-" + std::string(32, 'a'), false);
  EXPECT_NE(script.find("-RemoteAddress '192.0.2.1'"), std::string::npos);
  EXPECT_NE(script.find("-LocalPort 7442"), std::string::npos);
  EXPECT_NE(script.find("owned rule changed"), std::string::npos);
  EXPECT_EQ(script.find("Set-NetFirewallProfile"), std::string::npos);
}
#ifndef _WIN32
namespace {
void replace(std::string& value, const std::string& from, const std::string& to) {
  std::size_t position = 0;
  while ((position = value.find(from, position)) != std::string::npos) {
    value.replace(position, from.size(), to);
    position += to.size();
  }
}
struct UfwFixture : testing::Test {
  std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("asterion-fw-test-" + unique_process_id());
  std::string rule = "asterion-" + std::string(32, 'a');
  void write(const std::string& file, const std::string& content) {
    std::ofstream out(root / file);
    out << content;
  }
  std::string read(const std::string& file) {
    std::ifstream in(root / file);
    return {std::istreambuf_iterator<char>(in), {}};
  }
  void SetUp() override {
    std::filesystem::create_directory(root);
    write("status", "Status: active\n");
    write("ufw",
          "#!/bin/sh\nset -eu\ncd '" + root.string() +
              "'\nif test \"$1\" = status; then cat status; exit 0; fi\nprintf '%s\\n' \"$*\" >> "
              "calls\nif test \"$1\" = delete; then printf 'Status: active\\n' > status; "
              "else\nsource=$6; shift 9; port=$1; shift 2; tag=$1\nprintf 'Status: active\\n%s/tcp "
              "ALLOW IN %s # %s\\n' \"$port\" \"$source\" \"$tag\" > status\nfi\n");
    std::filesystem::permissions(root / "ufw", std::filesystem::perms::owner_all);
  }
  ~UfwFixture() override {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
  Json execute(bool remove = false) {
    auto script = node::firewall_change("linux", "192.0.2.1", 7442, rule, remove);
    // Test-only command substitution: execute the production rule parser against an isolated UFW
    // double.
    replace(script, "/usr/sbin/ufw", "'" + (root / "ufw").string() + "'");
    replace(script, "prefix='sudo -n'", "prefix=''");
    return node::run_firewall_script("linux", script);
  }
};
} // namespace
TEST_F(UfwFixture, ApplyRetryAndRemoveOnlyOwnedRule) {
  EXPECT_EQ(execute(), Json({{"changed", true}}));
  const auto first = read("calls");
  EXPECT_NE(first.find("from 192.0.2.1 to any port 7442"), std::string::npos);
  EXPECT_EQ(execute(), Json({{"changed", true}}));
  EXPECT_EQ(read("calls"), first);
  EXPECT_EQ(execute(true), Json({{"changed", true}}));
  EXPECT_NE(read("calls").find("delete allow"), std::string::npos);
}
TEST_F(UfwFixture, ForeignOrChangedRulesCannotBeOverwrittenOrRemoved) {
  write("status", "Status: active\n7442/tcp ALLOW IN 192.0.2.1 # administrator\n");
  EXPECT_THROW(execute(), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(root / "calls"));
  EXPECT_EQ(execute(true), Json({{"changed", true}}));
  EXPECT_FALSE(std::filesystem::exists(root / "calls"));
  write("status", "Status: active\n17442/tcp ALLOW IN 192.0.2.10 # " + rule +
                      "\n7442/tcp ALLOW IN 192.0.2.1 # administrator\n");
  EXPECT_THROW(execute(true), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(root / "calls"));
  write("status", "Status: active\n7442/tcp ALLOW IN 192.0.2.1 # " + rule +
                      "\n17442/tcp ALLOW IN 192.0.2.10 # " + rule + "\n");
  EXPECT_THROW(execute(true), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(root / "calls"));
}
TEST_F(UfwFixture, DisabledFirewallIsNeverEnabled) {
  write("status", "Status: inactive\n");
  EXPECT_THROW(execute(), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(root / "calls"));
}
#endif
