#include <asterion/kernel/native_plugin.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/foundation/error.hpp>
#include <asterion/plugin/sdk.hpp>
#include <asterion/plugin/history.h>
#include "support/native-plugin/fixture.h"
#include <gtest/gtest.h>
using namespace asterion;
namespace {
const TestLifetime& table(NativeInstance& instance) {
  return *static_cast<const TestLifetime*>(
      instance.query(AST_TEST_LIFETIME, 1, sizeof(TestLifetime)));
}
TEST(NativePlugin, InstancePinsLibraryAndOwnsDestruction) {
  std::unique_ptr<NativeInstance> instance;
  {
    NativeLibrary library(PLUGIN_GOOD);
    EXPECT_EQ(library.descriptor().id, "test.independent.c");
    EXPECT_EQ(library.descriptor().capabilities.size(), 3);
    instance = library.create(AST_TEST_LIFETIME, {});
  }
  const auto& api = table(*instance);
  EXPECT_EQ(api.running(instance->handle()), 0);
  instance->start();
  instance->start();
  EXPECT_EQ(api.running(instance->handle()), 1);
  instance->stop();
  instance->stop();
  EXPECT_EQ(api.stopped(), 1);
  instance->start();
  NativeLibrary observer(PLUGIN_GOOD);
  auto witness = observer.create(AST_TEST_LIFETIME, {});
  const auto before = api.destroyed();
  instance.reset();
  EXPECT_EQ(table(*witness).destroyed(), before + 1);
  EXPECT_EQ(table(*witness).stopped(), 2);
}
TEST(NativePlugin, FailedStartStopsPartialStateAndPreservesErrorCode) {
  NativeLibrary library(PLUGIN_GOOD);
  auto instance = library.create(AST_TEST_LIFETIME, {{"fail_start", "yes"}});
  const auto before = table(*instance).stopped();
  try {
    instance->start();
    FAIL();
  } catch (const Error& error) {
    EXPECT_EQ(error.code(), ErrorCode::unavailable);
  }
  EXPECT_EQ(table(*instance).running(instance->handle()), 0);
  EXPECT_EQ(table(*instance).stopped(), before + 1);
  EXPECT_THROW(library.create(AST_TEST_LIFETIME, {{"fail_create", "yes"}}), Error);
  EXPECT_THROW(library.create("missing", {}), Error);
  EXPECT_THROW(instance->query(AST_TEST_LIFETIME, 2, sizeof(TestLifetime)), Error);
  EXPECT_THROW(library.create(AST_TEST_LIFETIME, {{"x", "1"}, {"x", "2"}}), std::invalid_argument);
  EXPECT_THROW(library.create(AST_TEST_LIFETIME, {{nullptr, "secret"}}), std::invalid_argument);
}
TEST(NativePlugin, RejectsBrokenModulesAndDuplicateIdentity) {
  EXPECT_THROW(NativeLibrary(PLUGIN_BAD_ABI), std::invalid_argument);
  EXPECT_THROW(NativeLibrary(PLUGIN_MISSING_ENTRY), std::invalid_argument);
  EXPECT_THROW(NativeLibrary("relative.dylib"), std::invalid_argument);
  NativeLibrary empty(PLUGIN_EMPTY_TABLE);
  auto instance = empty.create(AST_TEST_LIFETIME, {});
  EXPECT_THROW(table(*instance), std::invalid_argument);
  auto directory = std::filesystem::temp_directory_path() / unique_process_id();
  std::filesystem::create_directories(directory);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{directory};
  auto extension = std::filesystem::path(PLUGIN_GOOD).extension();
  std::filesystem::copy_file(PLUGIN_GOOD, directory / ("one" + extension.string()));
  EXPECT_EQ(discover_native_plugins(directory).size(), 1);
  std::filesystem::copy_file(PLUGIN_GOOD, directory / ("two" + extension.string()));
  EXPECT_THROW(discover_native_plugins(directory), std::invalid_argument);
  std::filesystem::remove(directory / ("two" + extension.string()));
  std::filesystem::create_symlink(PLUGIN_GOOD, directory / ("link" + extension.string()));
  EXPECT_THROW(discover_native_plugins(directory), std::invalid_argument);
}
TEST(NativePlugin, SdkContainsExceptionsWithoutLeakingProviderText) {
  EXPECT_EQ(sdk::boundary([] { throw std::runtime_error("private vendor response"); }), AST_FAILED);
  EXPECT_EQ(sdk::boundary([] { throw std::invalid_argument("private credential"); }), AST_INVALID);
  EXPECT_EQ(sdk::boundary([] { sdk::check(AST_CANCELLED); }), AST_CANCELLED);
}
TEST(NativePlugin, TushareIsDiscoveredWithoutCredentialsOrNetwork) {
  NativeLibrary library(PLUGIN_TUSHARE);
  auto instance = library.create(AST_HISTORY_V2, {});
  auto* api =
      static_cast<const AstHistoryV2*>(instance->query(AST_HISTORY_V2, 2, sizeof(AstHistoryV2)));
  std::vector<std::string> sources;
  EXPECT_EQ(api->sources(instance->handle(), &sources,
                         [](void* context, const AstHistorySource* source) noexcept -> AstStatus {
                           return sdk::boundary([&] {
                             static_cast<std::vector<std::string>*>(context)->emplace_back(
                                 source->source);
                           });
                         }),
            AST_OK);
  EXPECT_EQ(sources, (std::vector<std::string>{"tushare.ft_mins", "tushare.fut_daily"}));
  EXPECT_EQ(api->sources(instance->handle(), nullptr, nullptr), AST_INVALID);
  EXPECT_THROW(library.create(AST_HISTORY_V2, {{"unknown", "x"}}), Error);
  instance->start();
  EXPECT_EQ(api->minutes(instance->handle(), nullptr, {}, {}, nullptr, nullptr), AST_INVALID);
}
TEST(NativePlugin, TushareRequiresAdmissionBeforeSendingDownloadRequests) {
  NativeLibrary library(PLUGIN_TUSHARE);
  auto instance =
      library.create(AST_HISTORY_V2, {{"source", "tushare.fut_daily"}, {"credential", "fixture"}});
  instance->start();
  const auto* api =
      static_cast<const AstHistoryV2*>(instance->query(AST_HISTORY_V2, 2, sizeof(AstHistoryV2)));
  const AstHistoryQuery query{
      {"SHFE", "cu", "2024-03"}, "CU2403.SHF", 0, 0, 0, "2024-03-01", "2024-03-01"};
  unsigned requests = 0, emitted = 0;
  const AstRequestBudget budget{&requests, [](void* value) noexcept -> AstStatus {
                                  ++*static_cast<unsigned*>(value);
                                  return AST_LIMIT;
                                }};
  const auto emit = [](void* value, const AstDaily*) noexcept -> AstStatus {
    ++*static_cast<unsigned*>(value);
    return AST_OK;
  };
  EXPECT_EQ(api->daily(instance->handle(), &query, {}, budget, &emitted, emit), AST_LIMIT);
  EXPECT_EQ(requests, 1);
  EXPECT_EQ(emitted, 0);
}
} // namespace
