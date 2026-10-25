#pragma once
#include "service_endpoint.hpp"
#include "service_io.hpp"
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/task.hpp>
#include <asterion/research.pb.h>
#include <memory>
#include <chrono>

namespace asterion::terminal {

class Application;

class ResearchClient
{
public:
    [[nodiscard]] static std::future<std::shared_ptr<ResearchClient>>
    open(ServiceIo&, ServiceEndpoint);
    ~ResearchClient();
    ResearchClient(const ResearchClient&) = delete;
    ResearchClient& operator=(const ResearchClient&) = delete;
    [[nodiscard]] std::future<Json> status() const;
    [[nodiscard]] std::future<Json> resources(const resource::proto::CatalogueQuery&);
    [[nodiscard]] std::future<Json> project(const research::proto::ProjectRequest&);
    [[nodiscard]] std::future<Json> variable(const research::proto::VariableRequest&);
    [[nodiscard]] std::future<Json> strategy(const research::proto::StrategyRequest&);
    [[nodiscard]] std::future<Json> logic(const research::proto::LogicRequest&);
    [[nodiscard]] std::future<void>
    submit(const std::string&, const research::proto::LogicTrialRequest&);
    [[nodiscard]] std::future<Json> project_tasks(const research::proto::ProjectTaskQuery&);
    [[nodiscard]] std::future<void> page(unsigned before_sequence);
    ServiceEndpoint endpoint() const;
    [[nodiscard]] std::future<void> submit(
        const std::string&,
        const backtest::proto::BacktestRequest&,
        const data::proto::DataOwner&,
        const research::proto::StrategyUse& = {},
        const research::proto::ProjectBinding& = {});
    [[nodiscard]] std::future<void> submit(
        const std::string&,
        const factor::proto::FactorRequest&,
        const data::proto::DataOwner&,
        const task::proto::TaskDerivation& = {},
        const research::proto::ProjectBinding& = {});
    [[nodiscard]] std::future<void>
    submit(const std::string&, const research::proto::AccountReviewRequest&);
    [[nodiscard]] std::future<void>
    submit(const std::string&, const research::proto::EvaluationRequest&);
    [[nodiscard]] std::future<void> submit(
        const std::string&,
        const research::proto::StatisticsRequest&,
        const research::proto::ProjectBinding& = {});
    [[nodiscard]] std::future<void> submit(
        const std::string&,
        const research::proto::SampleRequest&,
        const data::proto::DataOwner&,
        const research::proto::ProjectBinding& = {});
    [[nodiscard]] std::future<void> reuse_backtest(
        const std::string&,
        const backtest::proto::BacktestReuse&,
        const data::proto::DataOwner&,
        const research::proto::StrategyUse& = {});
    [[nodiscard]] std::future<Json> history_usage(const std::string&);
    [[nodiscard]] std::future<Json> retirement(const std::string&, bool execute);
    [[nodiscard]] static std::future<Json> inspect_history_usage(
        ServiceIo&,
        const ServiceEndpoint&,
        const std::string&,
        std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] std::future<nlohmann::json> metadata(const std::string&);
    [[nodiscard]] std::future<void> edit_metadata(const task::proto::TaskMetadataEdit&);
    [[nodiscard]] std::future<void> action(const std::string& id, const std::string& action);
    [[nodiscard]] std::future<Json> result(const std::string& id);

private:
    friend class Application;

    struct Read
    {
        std::shared_ptr<const Json> page;
        Json connection;
        Json render() const;
        bool operator==(const Read&) const = default;
    };

    // Application collects selected service views in one I/O owner turn.
    Read owner_view() const;
    ResearchClient(ServiceIo&, ServiceEndpoint);
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace asterion::terminal
