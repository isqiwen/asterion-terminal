#pragma once
#include "service_endpoint.hpp"
#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/research.hpp>
#include <memory>
namespace asterion::terminal {
class ResearchClient {
public:
  explicit ResearchClient(ServiceEndpoint endpoint);
  ~ResearchClient();
  Json status() const;
  void submit(const std::string& id, const research::v1::BacktestInput& input);
  void submit(const std::string& id, const research::v1::FactorInput& input);
  void submit(const std::string&, const research::v1::DailyFactorRequest&);
  void submit(const std::string& id, const data::v1::CsvSnapshot& input);
  void submit(const std::string&, const data::v1::CalendarCsvSnapshot&);
  void submit(const std::string&, const data::v1::MinuteDownload&, const std::string& token);
  void submit(const std::string&, const data::v1::DailyDownload&, const std::string& token);
  Json daily_page(const data::v1::DailyPageQuery&);
  void action(const std::string& id, const std::string& action);
  Json result(const std::string& id);
  Json minute_page(const data::v1::MinutePageQuery&);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::terminal
