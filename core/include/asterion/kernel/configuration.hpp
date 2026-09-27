#pragma once
#include <asterion/foundation/serialization.hpp>
#include <functional>
#include <map>
namespace asterion {
// Host-controlled startup configuration. Credentials must not enter this store.
class Configuration final {
public:
  using Validator = std::function<bool(const Json&)>;
  void declare(std::string key, Json initial, Validator validator) {
    if (sealed_)
      throw Error(ErrorCode::conflict, "configuration is sealed");
    validate_id(key);
    if (!validator || !validator(initial))
      throw Error(ErrorCode::invalid_request, "invalid configuration default");
    if (!fields_.emplace(std::move(key), Field{std::move(initial), std::move(validator)}).second)
      throw Error(ErrorCode::conflict, "duplicate configuration field");
  }
  void apply(const Json& patch) {
    if (sealed_)
      throw Error(ErrorCode::conflict, "configuration is sealed");
    if (!patch.is_object())
      throw Error(ErrorCode::invalid_request, "configuration patch must be an object");
    auto next = fields_;
    for (const auto& [key, value] : patch.items()) {
      auto found = next.find(key);
      if (found == next.end() || !found->second.validator(value))
        throw Error(ErrorCode::invalid_request, "unknown or invalid configuration field");
      found->second.value = value;
    }
    fields_.swap(next);
  }
  void seal() noexcept { sealed_ = true; }
  Json at(const std::string& key) const {
    const auto found = fields_.find(key);
    if (found == fields_.end())
      throw Error(ErrorCode::invalid_request, "unknown configuration field");
    return found->second.value;
  }
  Json snapshot() const {
    auto result = Json::object();
    for (const auto& [key, field] : fields_)
      result[key] = field.value;
    return result;
  }

private:
  struct Field {
    Json value;
    Validator validator;
  };
  std::map<std::string, Field> fields_;
  bool sealed_ = false;
};
} // namespace asterion
