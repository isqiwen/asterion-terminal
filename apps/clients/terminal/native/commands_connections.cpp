#include "application_impl.hpp"
namespace asterion::terminal {
DataConnection Application::Impl::resolve_data_connection(const std::string& id,
                                                          const std::string& revision,
                                                          const std::string& source) {
  if (!research)
    throw std::invalid_argument("connect research service first");
  auto connection = data_connections.get(id);
  const auto provider = research->source(source);
  if (connection.revision != revision)
    throw Error(ErrorCode::conflict, "data connection changed; inspect again");
  if (connection.source != source || connection.plugin_id != provider.plugin_id() ||
      !provider.has_connection())
    throw std::invalid_argument("data connection provider is unavailable");
  const auto& schema = provider.connection();
  if (connection.requests_per_minute > schema.requests_per_minute_max() ||
      connection.credential.size() > schema.credential_max_length() ||
      (schema.credential_required() && connection.credential.empty()) ||
      (connection.remember && !schema.remember_allowed()))
    throw std::invalid_argument("data connection requires configuration");
  return connection;
}
void Application::Impl::register_connection_commands() {
  core.command("research.connections.save", "node.manage", [this](const json& params) {
    fields(params, {"id", "name", "source", "revision", "requests_per_minute", "remember",
                    "credential", "credential_action"});
    if (!research)
      throw std::invalid_argument("connect research service first");
    const auto source = research->source(text(params, "source"));
    if (!source.has_connection())
      throw std::invalid_argument("history connection configuration is unsupported");
    if (!params.at("requests_per_minute").is_number_unsigned() ||
        !params.at("remember").is_boolean())
      throw std::invalid_argument("invalid data connection settings");
    DataConnection connection{text(params, "id"),
                              text(params, "name"),
                              source.id(),
                              source.plugin_id(),
                              {},
                              params.at("requests_per_minute").get<unsigned>(),
                              params.at("remember").get<bool>(),
                              text(params, "credential", true)};
    data_connections.save(std::move(connection), text(params, "revision", true),
                          text(params, "credential_action"), source.connection());
    connection_verification = nullptr;
    return snapshot();
  });
  core.command("research.connections.remove", "node.manage", [this](const json& params) {
    fields(params, {"id", "revision"});
    data_connections.remove(text(params, "id"), text(params, "revision"));
    connection_verification = nullptr;
    return snapshot();
  });
  core.command("research.connections.verify", "node.manage", [this](const json& params) {
    fields(params, {"id", "revision", "source"});
    return snapshot();
  });
}
} // namespace asterion::terminal
