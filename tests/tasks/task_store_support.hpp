#pragma once
#include "research_submission.hpp"
#include "data_submission.hpp"
#include "task_store.hpp"
#include <stdexcept>
#include <utility>

// The store's split submit, finish and read steps composed on one thread. The
// task host runs the same steps across its I/O, file and journal owners.
namespace asterion::tasks {

inline task::proto::Task submit(Store& store, Store::Submission submission)
{
    store.admit_submission(submission);
    std::string id;
    try {
        submission.prepare_files();
        id = store.commit(store.register_submission(submission)).task().id();
    } catch (...) {
        store.abandon_submission(submission);
        throw;
    }
    return store.get(id);
}

template <class... Input>
task::proto::Task submit(Store& store, Input&&... input)
{
    return submit(store, task_submission(store, std::forward<Input>(input)...));
}

inline Store::Change finish(Store& store, task::proto::TaskFinish request)
{
    auto completion = store.prepare_finish(request);
    completion.prepare_payload();
    return store.finish(std::move(completion));
}

inline Store::Change finish(
    Store& store,
    const std::string& id,
    const std::string& token,
    const backtest::proto::BacktestResult& result)
{
    task::proto::TaskFinish request;
    request.set_id(id);
    request.set_token(token);
    *request.mutable_result() = result;
    return finish(store, std::move(request));
}

inline Store::Change finish(
    Store& store,
    const std::string& id,
    const std::string& token,
    const factor::proto::FactorResult& result)
{
    task::proto::TaskFinish request;
    request.set_id(id);
    request.set_token(token);
    *request.mutable_factor() = result;
    return finish(store, std::move(request));
}

inline task::proto::TaskResponse verified_result(const Store& store, const std::string& id)
{
    auto read = store.prepare_result(id);
    read.verify();
    return store.confirm_result(std::move(read));
}

inline backtest::proto::BacktestResult result(const Store& store, const std::string& id)
{
    const auto response = verified_result(store, id);
    if (!response.has_backtest())
        throw std::invalid_argument("not a backtest result");
    return response.backtest();
}

inline factor::proto::FactorResult factor_result(const Store& store, const std::string& id)
{
    const auto response = verified_result(store, id);
    if (!response.has_factor())
        throw std::invalid_argument("factor result is not confirmed");
    return response.factor();
}

} // namespace asterion::tasks
