#include "csv_market_data.hpp"
#include "pipeline.hpp"
#include "task_store.hpp"
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/research.hpp>
#include <fstream>
#include <gtest/gtest.h>
#include <thread>
using namespace asterion;
namespace fs = std::filesystem;
namespace {
struct Directory {
  fs::path path = fs::temp_directory_path() / ("asterion-data-" + unique_process_id());
  Directory() { fs::create_directory(path); }
  ~Directory() {
    std::error_code error;
    fs::remove_all(path, error);
  }
};
void write(const fs::path& path, const std::string& bytes) {
  std::ofstream file(path, std::ios::binary);
  file << bytes;
}
std::string csv(bool alternate = false, std::size_t count = 40) {
  const std::string end = alternate ? "\r\n" : "\n";
  std::string text = "timestamp_ns,price,quantity" + end;
  for (std::size_t i = 0; i < count; ++i)
    text += std::to_string(1790298000000000000LL + i * 1000000000LL) + "," +
            std::to_string(100 + i % 3) + (alternate ? ".000,1.0" : ",1") + end;
  return text;
}
data::v1::CsvImport spec(const fs::path& source, const std::string& bytes) {
  write(source, bytes);
  data::v1::CsvImport input;
  input.set_version(1);
  const auto name = source.u8string();
  input.set_source_path(std::string(name.begin(), name.end()));
  input.set_source_sha256(sha256_bytes(bytes));
  *input.mutable_contract() = protocol::encode_contract({{"venue", "SHFE"},
                                                         {"symbol", "rb2610"},
                                                         {"currency", "CNY"},
                                                         {"price_increment", "1"},
                                                         {"quantity_increment", "1"},
                                                         {"multiplier", "10"},
                                                         {"product", "rb"},
                                                         {"delivery_month", "2026-10"}});
  return input;
}
} // namespace
TEST(DataPipeline, NormalizedContentIdentityIsSharedByResearchWhileProvenanceStaysDistinct) {
  Directory root;
  const auto first = data_pipeline::import_csv(spec(root.path / "one.csv", csv()));
  const auto second = data_pipeline::import_csv(spec(root.path / "two.csv", csv(true)));
  EXPECT_EQ(first.dataset().revision(), second.dataset().revision());
  EXPECT_NE(first.source_sha256(), second.source_sha256());
  EXPECT_NE(first.id(), second.id());
  EXPECT_EQ(first.dataset().ticks_size(), 40);
  protocol::v1::PaperInput paper;
  *paper.mutable_contract() = first.dataset().contract();
  *paper.mutable_ticks() = first.dataset().ticks();
  paper.mutable_deposit()->set_units(999);
  EXPECT_EQ(protocol::dataset_revision(paper), first.dataset().revision());
  research::v1::FactorInput factor;
  factor.set_version(4);
  factor.set_full_sample(true);
  *factor.mutable_contract() = first.dataset().contract();
  *factor.mutable_ticks() = first.dataset().ticks();
  factor.add_lookbacks(2);
  factor.set_horizon(1);
  EXPECT_EQ(protocol::factor_dataset_revision(factor), first.dataset().revision());
  EXPECT_EQ(protocol::encode_publication(protocol::decode_publication(first)).SerializeAsString(),
            first.SerializeAsString());
  auto changed = first.dataset();
  changed.mutable_ticks(0)->mutable_quantity()->set_units(Decimal::parse("2").raw());
  EXPECT_THROW(protocol::decode_dataset(changed), std::invalid_argument);
}
TEST(DataPipeline, RejectsChangedSourceBadRowsOversizeAndCancellationWithoutRepairingInput) {
  Directory root;
  auto input = spec(root.path / "input.csv", csv());
  write(root.path / "input.csv", csv(true));
  EXPECT_THROW(data_pipeline::import_csv(input), std::invalid_argument);
  input = spec(root.path / "input.csv", "timestamp_ns,price,quantity\n2,100,1\n1,101,1\n");
  EXPECT_THROW(data_pipeline::import_csv(input), std::runtime_error);
  input = spec(root.path / "input.csv", "timestamp_ns,price,quantity\n1,100.5,1\n");
  EXPECT_THROW(data_pipeline::import_csv(input), std::runtime_error);
  input = spec(root.path / "input.csv", csv(false, 10001));
  EXPECT_THROW(data_pipeline::import_csv(input), std::invalid_argument);
  input = spec(root.path / "input.csv", "timestamp_ns,price,quantity\n");
  EXPECT_THROW(data_pipeline::import_csv(input), std::invalid_argument);
  input = spec(root.path / "input.csv", csv());
  std::stop_source stop;
  stop.request_stop();
  EXPECT_THROW(data_pipeline::import_csv(input, stop.get_token()), std::runtime_error);
  input.GetReflection()->MutableUnknownFields(&input)->AddVarint(99, 1);
  EXPECT_THROW(data_pipeline::import_csv(input), std::invalid_argument);
}
TEST(DataPipeline, RetainsDuplicateEventsAndDoesNotInventCalendarOrPriceRules) {
  Directory root;
  const auto input =
      spec(root.path / "input.csv", "timestamp_ns,price,quantity\n1,0,1\n1,0,1\n2,-1,1\n");
  const auto publication = data_pipeline::import_csv(input);
  ASSERT_EQ(publication.dataset().ticks_size(), 3);
  EXPECT_EQ(publication.dataset().ticks(0).SerializeAsString(),
            publication.dataset().ticks(1).SerializeAsString());
  EXPECT_LT(publication.dataset().ticks(2).price().units(), 0);
  auto invalid = protocol::decode_publication(publication);
  invalid["source_bytes"] = -1;
  EXPECT_THROW(protocol::encode_publication(invalid), std::invalid_argument);
  auto tampered = publication;
  tampered.set_source_name("other.csv");
  EXPECT_THROW(protocol::decode_publication(tampered), std::invalid_argument);
}
TEST(DataPipeline, CommittedPublicationSurvivesSourceDeletionAndRejectsOverwriteOrPartialWrites) {
  Directory root;
  const auto output = root.path / "published";
  fs::create_directory(output);
  const auto value = data_pipeline::import_csv(spec(root.path / "source.csv", csv()));
  EXPECT_TRUE(data_pipeline::publish(value, output));
  EXPECT_FALSE(data_pipeline::publish(value, output));
  fs::remove(root.path / "source.csv");
  EXPECT_EQ(data_pipeline::read(output).SerializeAsString(), value.SerializeAsString());
  auto other = value;
  other.set_source_name("other.csv");
  other.set_id(protocol::publication_id(other));
  EXPECT_THROW(data_pipeline::publish(other, output), std::invalid_argument);
  EXPECT_EQ(data_pipeline::read(output).SerializeAsString(), value.SerializeAsString());
  const auto partial = root.path / "partial";
  fs::create_directory(partial);
  write(partial / "pending.tmp", "interrupted");
  EXPECT_THROW(data_pipeline::publish(value, partial), std::invalid_argument);
  EXPECT_EQ(fs::file_size(partial / "pending.tmp"), 11U);
  auto stored = Json::parse(std::ifstream(output / "00000000.json"));
  stored["publication"]["dataset"]["ticks"][0]["price"] = "999";
  write(output / "00000000.json", stored.dump());
  EXPECT_THROW(data_pipeline::read(output), std::invalid_argument);
}
TEST(DataPipeline, IndependentProgramPublishesAndInspectsWithoutOriginalInput) {
  Directory root;
  const auto output = root.path / "published";
  fs::create_directory(output);
  const auto source = root.path / fs::path(u8"历史.csv"), input_file = root.path / "import.pb";
  const auto input = spec(source, csv());
  write(input_file, input.SerializeAsString());
  auto execute = [&](const std::vector<std::string>& args) {
    ChildProcess process(ASTERION_PIPELINE_PATH, args);
    EXPECT_TRUE(process.wait(std::chrono::seconds(10)));
    return process.exit_code();
  };
  EXPECT_EQ(execute({"--input", input_file.string(), "--directory", output.string()}), 0);
  const auto id = data_pipeline::read(output).id();
  EXPECT_EQ(execute({"--input", input_file.string(), "--directory", output.string()}), 0);
  write(source, csv(true));
  EXPECT_NE(execute({"--input", input_file.string(), "--directory", output.string()}), 0);
  fs::remove(source);
  fs::remove(input_file);
  EXPECT_EQ(execute({"--inspect", "--directory", output.string()}), 0);
  EXPECT_EQ(data_pipeline::read(output).id(), id);
}

namespace wire = asterion::research::v1;
using namespace std::chrono_literals;
class DataTasks : public ::testing::Test {
protected:
  Directory directory;
  fs::path root;
  data::v1::CsvSnapshot input;
  void SetUp() override {
    input = data_pipeline::capture_csv(spec(directory.path / "source.csv", csv()));
    fs::remove(directory.path / "source.csv");
    root = directory.path / "tasks";
    fs::create_directory(root);
  }
};
TEST_F(DataTasks, DurableUploadCancellationFencingAndResultValidation) {
  const auto expected = data_pipeline::import_snapshot(input);
  {
    tasks::Store store(root);
    EXPECT_EQ(store.submit("data", input).kind(), wire::DATA_IMPORT);
    EXPECT_EQ(store.submit("data", input).attempt(), 0U);
    EXPECT_FALSE(store.list().tasks(0).has_data());
    EXPECT_EQ(store.list().tasks(0).source_name(), input.source_name());
    auto changed = input;
    changed.set_source_name("other.csv");
    EXPECT_THROW(store.submit("data", changed), std::invalid_argument);
    auto oversized = input;
    oversized.set_contents(std::string(4 * 1024 * 1024 + 1, '0'));
    oversized.set_source_sha256(sha256_bytes(oversized.contents()));
    EXPECT_THROW(store.submit("oversized", oversized), std::invalid_argument);
    auto token = store.claim("data");
    auto bad = expected;
    bad.mutable_dataset()->mutable_ticks(0)->mutable_price()->set_units(999);
    EXPECT_THROW(store.finish("data", token, bad), std::invalid_argument);
    store.cancel("data");
    store.finish("data", token, expected);
    EXPECT_EQ(store.get("data").state(), wire::CANCELLED);
    store.retry("data");
    const auto next = store.claim("data");
    EXPECT_THROW(store.finish("data", token, expected), std::invalid_argument);
    store.finish("data", next, expected);
    EXPECT_EQ(store.get("data").completed(), input.contents().size());
    EXPECT_THROW(store.result("data"), std::invalid_argument);
    EXPECT_THROW(store.factor_result("data"), std::invalid_argument);
  }
  tasks::Store recovered(root);
  EXPECT_EQ(recovered.publication("data").SerializeAsString(), expected.SerializeAsString());
  EXPECT_EQ(recovered.get("data").attempt(), 2U);
}
TEST_F(DataTasks, TypedWorkerRunsAndWrongExecutableCannotClaim) {
#ifdef _WIN32
  const std::string endpoint = "asterion.data." + unique_process_id();
#else
  const auto socket_root =
      std::filesystem::path("/tmp") / ("ast-f-" + unique_process_id().substr(0, 12));
  std::filesystem::create_directory(socket_root);
  std::filesystem::permissions(socket_root, std::filesystem::perms::owner_all);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  } cleanup{socket_root};
  const auto endpoint = (socket_root / "task.sock").string();
#endif
  const auto utf8 = root.u8string();
  ChildProcess service(ASTERION_TASK_SERVICE_PATH,
                       {"--directory", std::string(utf8.begin(), utf8.end()), "--endpoint",
                        endpoint, "--session", "data-tests"});
  auto call = [&](wire::TaskRequest request) {
    request.set_version(1);
    request.set_service_id("data-tests");
    request.set_correlation_id(unique_process_id());
    auto channel = ipc::Channel::connect(endpoint, 2s);
    channel.send(request.SerializeAsString(), 2s);
    wire::TaskResponse response;
    if (!response.ParseFromString(channel.receive(2s)))
      throw std::runtime_error("bad response");
    if (response.has_error())
      throw std::runtime_error(response.error().message());
    return response;
  };
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  for (;;) {
    try {
      wire::TaskRequest ping;
      ping.mutable_heartbeat();
      call(ping);
      break;
    } catch (const std::exception&) {
      if (service.exited() || std::chrono::steady_clock::now() > deadline)
        throw;
      std::this_thread::sleep_for(20ms);
    }
  }
  wire::TaskRequest submit;
  submit.mutable_submit()->set_id("run");
  *submit.mutable_submit()->mutable_data() = input;
  call(submit);
  {
    ChildProcess wrong(ASTERION_BACKTEST_PATH,
                       {"--endpoint", endpoint, "--session", "data-tests", "--task", "run"});
    ASSERT_TRUE(wrong.wait(10s));
    EXPECT_NE(wrong.exit_code(), 0);
  }
  wire::TaskRequest get;
  get.mutable_get()->set_id("run");
  EXPECT_EQ(call(get).task().state(), wire::QUEUED);
  EXPECT_EQ(call(get).task().attempt(), 0U);
  {
    ChildProcess worker(ASTERION_PIPELINE_PATH,
                        {"--endpoint", endpoint, "--session", "data-tests", "--task", "run"});
    ASSERT_TRUE(worker.wait(10s));
    EXPECT_EQ(worker.exit_code(), 0);
  }
  wire::TaskRequest result;
  result.mutable_result()->set_id("run");
  EXPECT_EQ(call(result).publication().SerializeAsString(),
            data_pipeline::import_snapshot(input).SerializeAsString());
  auto invalid = input;
  invalid.set_contents("timestamp_ns,price,quantity\n2,100,1\n1,101,1\n");
  invalid.set_source_sha256(sha256_bytes(invalid.contents()));
  submit.mutable_submit()->set_id("bad-rows");
  *submit.mutable_submit()->mutable_data() = invalid;
  call(submit);
  {
    ChildProcess worker(ASTERION_PIPELINE_PATH,
                        {"--endpoint", endpoint, "--session", "data-tests", "--task", "bad-rows"});
    ASSERT_TRUE(worker.wait(10s));
    EXPECT_NE(worker.exit_code(), 0);
  }
  get.mutable_get()->set_id("bad-rows");
  EXPECT_EQ(call(get).task().state(), wire::FAILED);
  EXPECT_EQ(call(get).task().attempt(), 1U);
  result.mutable_result()->set_id("bad-rows");
  EXPECT_THROW(call(result), std::runtime_error);
}
