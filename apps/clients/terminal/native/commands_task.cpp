#include "application_impl.hpp"
#include <asterion/protocol/factor.hpp>

namespace asterion::terminal {
void Application::Impl::register_task_commands() {
  command("task.page", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"before_sequence"});
    const auto& before = p.at("before_sequence");
    if (!before.is_number_integer() || before < 0 || before > 4294967295ULL)
      throw std::invalid_argument("invalid task page cursor");
    if (!task_client)
      throw std::invalid_argument("task service is not connected");
    const auto client = task_client;
    (co_await PollFuture{client->page(before.get<unsigned>())});
    if (task_client != client)
      throw Error(ErrorCode::conflict, "task service changed during page query");
    co_return snapshot();
  });
  command("task.action", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"id", "action"});
    if (!task_client)
      throw std::invalid_argument("task service is not connected");
    const auto client = task_client;
    const auto generation = data_task_generation;
    (co_await PollFuture{client->action(text(p, "id"), text(p, "action"))});
    if (generation != data_task_generation)
      throw Error(ErrorCode::conflict,
                  "data/task service selection changed during operation; inspect the "
                  "original services before retrying");
    co_return snapshot();
  });
  command("task.result", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"id"});
    if (!task_client)
      throw std::invalid_argument("task service is not connected");
    validate_id(text(p, "id"));
    const auto client = task_client;
    const auto generation = data_task_generation;
    auto evidence = (co_await PollFuture{client->result(text(p, "id"))});
    if (generation != data_task_generation)
      throw Error(ErrorCode::conflict,
                  "data/task service selection changed during operation; inspect the "
                  "original services before retrying");
    task_result = std::make_shared<const json>(std::move(evidence));
    co_return snapshot();
  });
}
} // namespace asterion::terminal
