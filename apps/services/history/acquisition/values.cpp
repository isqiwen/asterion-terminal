#include "values.hpp"

namespace asterion::history::acquisition::values {

using warehouse::Cell;

// Keep the JSON number token, including exponents and integers wider than 64
// bits. Binary is an internal marker that JSON input itself cannot produce.
class ExactJson : public nlohmann::json_sax<Json>
{
    struct Frame
    {
        Json* node;
        std::string key;
    };

    std::vector<Frame> stack_;

    Json* add(Json value)
    {
        if (stack_.empty()) {
            root = std::move(value);
            return &root;
        }
        auto& frame = stack_.back();
        if (frame.node->is_array()) {
            frame.node->push_back(std::move(value));
            return &frame.node->back();
        }
        auto& child = (*frame.node)[frame.key];
        child = std::move(value);
        return &child;
    }

    bool number(const std::string& value)
    {
        add(Json::binary(std::vector<std::uint8_t>(value.begin(), value.end())));
        return true;
    }

    bool start(Json value)
    {
        if (stack_.size() >= 16)
            return false;
        auto* child = add(std::move(value));
        stack_.push_back({child, {}});
        return true;
    }

public:
    Json root;

    bool null() override
    {
        add(nullptr);
        return true;
    }

    bool boolean(bool value) override
    {
        add(value);
        return true;
    }

    bool number_integer(number_integer_t value) override
    {
        return number(std::to_string(value));
    }

    bool number_unsigned(number_unsigned_t value) override
    {
        return number(std::to_string(value));
    }

    bool number_float(number_float_t, const string_t& raw) override
    {
        return number(raw);
    }

    bool string(string_t& value) override
    {
        add(value);
        return true;
    }

    bool binary(binary_t&) override
    {
        return false;
    }

    bool start_object(std::size_t) override
    {
        return start(Json::object());
    }

    bool start_array(std::size_t) override
    {
        return start(Json::array());
    }

    bool key(string_t& value) override
    {
        if (stack_.back().node->contains(value))
            return false;
        stack_.back().key = value;
        return true;
    }

    bool end_object() override
    {
        stack_.pop_back();
        return true;
    }

    bool end_array() override
    {
        stack_.pop_back();
        return true;
    }

    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override
    {
        return false;
    }
};

std::string numeric(const Json& value, std::int64_t shift)
{
    if (!value.is_binary())
        throw std::invalid_argument("expected numeric cell");
    const auto& bytes = value.get_binary();
    const std::string raw(bytes.begin(), bytes.end());
    const auto exponent = raw.find_first_of("eE");
    auto digits = raw.substr(0, exponent);
    if (exponent != std::string::npos)
        shift += std::stoi(raw.substr(exponent + 1));
    if (shift < -100 || shift > 100)
        throw std::invalid_argument("decimal exponent outside storage range");
    const bool negative = digits.starts_with('-');
    if (negative)
        digits.erase(0, 1);
    const auto dot = digits.find('.');
    int point = static_cast<int>(dot == std::string::npos ? digits.size() : dot) + shift;
    if (dot != std::string::npos)
        digits.erase(dot, 1);
    if (point <= 0)
        digits = "0." + std::string(-point, '0') + digits;
    else if (point >= static_cast<int>(digits.size()))
        digits.append(point - digits.size(), '0');
    else
        digits.insert(point, ".");
    if (digits.find('.') != std::string::npos) {
        while (digits.back() == '0')
            digits.pop_back();
        if (digits.back() == '.')
            digits.pop_back();
    }
    const auto first = digits.find_first_not_of('0');
    if (first == std::string::npos)
        return "0";
    digits.erase(0, digits[first] == '.' ? first - 1 : first);
    return negative ? "-" + digits : digits;
}

Cell number(const Json& value, int shift)
{
    return value.is_null() ? Cell{} : Cell{numeric(value, shift)};
}

Json parse(std::string_view raw)
{
    ExactJson parser;
    if (raw.size() > response_limit || !Json::sax_parse(raw, &parser))
        throw std::invalid_argument("invalid source JSON");
    return std::move(parser.root);
}

} // namespace asterion::history::acquisition::values
