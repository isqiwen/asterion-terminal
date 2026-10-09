#pragma once
#include "task_store.hpp"
#include <stdexcept>
#include <utility>
// The store's split submit, finish and read steps composed on one thread. The
// task host runs the same steps across its I/O, file and journal owners.
namespace asterion::tasks {
template <class... Input> task::v1::Task submit(Store& store, Input&&... input) {
  auto submission = store.submission(std::forward<Input>(input)...);
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
inline Store::Change finish(Store& store, task::v1::TaskFinish request) {
  auto completion = store.prepare_finish(request);
  completion.prepare_payload();
  return store.finish(std::move(completion));
}
inline Store::Change finish(Store& store, const std::string& id, const std::string& token,
                            const backtest::v1::BacktestResult& result) {
  task::v1::TaskFinish request;
  request.set_id(id);
  request.set_token(token);
  *request.mutable_result() = result;
  return finish(store, std::move(request));
}
inline Store::Change finish(Store& store, const std::string& id, const std::string& token,
                            const factor::v1::FactorResult& result) {
  task::v1::TaskFinish request;
  request.set_id(id);
  request.set_token(token);
  *request.mutable_factor() = result;
  return finish(store, std::move(request));
}
inline task::v1::TaskResponse verified_result(const Store& store, const std::string& id) {
  auto read = store.prepare_result(id);
  read.verify();
  return store.confirm_result(std::move(read));
}
inline backtest::v1::BacktestResult result(const Store& store, const std::string& id) {
  const auto response = verified_result(store, id);
  if (!response.has_backtest())
    throw std::invalid_argument("not a backtest result");
  return response.backtest();
}
inline factor::v1::FactorResult factor_result(const Store& store, const std::string& id) {
  const auto response = verified_result(store, id);
  if (!response.has_factor())
    throw std::invalid_argument("factor result is not confirmed");
  return response.factor();
}
inline factor::v1::DailyFactorResult daily_factor_result(const Store& store,
                                                         const std::string& id) {
  const auto response = verified_result(store, id);
  if (!response.has_daily_factor())
    throw std::invalid_argument("daily factor result is not confirmed");
  return response.daily_factor();
}
} // namespace asterion::tasks
