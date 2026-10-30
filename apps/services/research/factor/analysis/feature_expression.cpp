#include "feature_expression.hpp"
#include <asterion/foundation/error.hpp>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <deque>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>

namespace asterion {
namespace {

enum class Op
{
    close,
    constant,
    neg,
    add,
    subtract,
    multiply,
    divide,
    lag,
    returns,
    mean,
    deviation,
    minimum,
    maximum
};

struct Node
{
    Op op;
    std::size_t position = 0, period = 0, depth = 1;
    double constant = 0;
    int power = 0;
    bool parameter = false;
    std::unique_ptr<Node> left, right;
};

class Parser
{
public:
    explicit Parser(std::string_view source)
        : source_(source)
    {}

    std::unique_ptr<Node> parse()
    {
        if (source_.size() > 8192)
            throw FeatureExpressionError(8192, "source exceeds 8192 bytes");
        auto root = expression(0);
        whitespace();
        if (position_ != source_.size())
            fail("unexpected token");
        return root;
    }

private:
    std::string_view source_;
    std::size_t position_ = 0, nodes_ = 0;

    [[noreturn]] void fail(const char* reason) const
    {
        throw FeatureExpressionError(position_, reason);
    }

    void whitespace()
    {
        while (position_ < source_.size()
            && std::isspace(static_cast<unsigned char>(source_[position_])))
            ++position_;
    }

    bool take(char c)
    {
        whitespace();
        if (position_ < source_.size() && source_[position_] == c) {
            ++position_;
            return true;
        }
        return false;
    }

    std::string_view identifier()
    {
        whitespace();
        const auto start = position_;
        while (position_ < source_.size()
            && (std::isalnum(static_cast<unsigned char>(source_[position_]))
                || source_[position_] == '_'))
            ++position_;
        return source_.substr(start, position_ - start);
    }

    std::unique_ptr<Node>
    make(Op op, std::size_t at, std::unique_ptr<Node> left = {}, std::unique_ptr<Node> right = {})
    {
        auto n = std::make_unique<Node>();
        n->op = op;
        n->position = at;
        n->left = std::move(left);
        n->right = std::move(right);
        n->depth = 1 + std::max(n->left ? n->left->depth : 0, n->right ? n->right->depth : 0);
        if (++nodes_ > 64 || n->depth > 32)
            throw FeatureExpressionError(at, "expression exceeds 64 nodes or depth 32");
        const int a = n->left ? n->left->power : 0, b = n->right ? n->right->power : 0;
        if ((op == Op::add || op == Op::subtract) && a != b)
            throw FeatureExpressionError(at, "addition and subtraction require equal units");
        n->power = op == Op::close                    ? 1
            : op == Op::constant || op == Op::returns ? 0
            : op == Op::multiply                      ? a + b
            : op == Op::divide                        ? a - b
                                                      : a;
        if (n->power < -8 || n->power > 8)
            throw FeatureExpressionError(at, "output price power exceeds limit");
        return n;
    }

    std::unique_ptr<Node> expression(unsigned depth)
    {
        auto left = product(depth);
        for (;;) {
            whitespace();
            const auto at = position_;
            Op op;
            if (take('+'))
                op = Op::add;
            else if (take('-'))
                op = Op::subtract;
            else
                break;
            left = make(op, at, std::move(left), product(depth));
        }
        return left;
    }

    std::unique_ptr<Node> product(unsigned depth)
    {
        auto left = primary(depth);
        for (;;) {
            whitespace();
            const auto at = position_;
            Op op;
            if (take('*'))
                op = Op::multiply;
            else if (take('/'))
                op = Op::divide;
            else
                break;
            left = make(op, at, std::move(left), primary(depth));
        }
        return left;
    }

    std::unique_ptr<Node> primary(unsigned depth)
    {
        whitespace();
        const auto at = position_;
        if (depth > 32)
            fail("expression exceeds 64 nodes or depth 32");
        if (take('-'))
            return make(Op::neg, at, primary(depth + 1));
        if (take('(')) {
            auto node = expression(depth + 1);
            if (!take(')'))
                fail("expected closing parenthesis");
            return node;
        }
        if (position_ == source_.size())
            fail("expected expression");
        const auto c = source_[position_];
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            // Floating-point from_chars requires macOS 26; the desktop supports 13.0.
            // Scan the same digits, point and exponent, then parse them in the classic locale.
            const auto digits = [&] {
                while (position_ < source_.size()
                    && std::isdigit(static_cast<unsigned char>(source_[position_])))
                    ++position_;
            };
            const auto start = position_;
            digits();
            if (position_ < source_.size() && source_[position_] == '.') {
                ++position_;
                digits();
            }
            if (position_ < source_.size()
                && (source_[position_] == 'e' || source_[position_] == 'E')) {
                auto exponent = position_ + 1;
                if (exponent < source_.size()
                    && (source_[exponent] == '+' || source_[exponent] == '-'))
                    ++exponent;
                if (exponent < source_.size()
                    && std::isdigit(static_cast<unsigned char>(source_[exponent]))) {
                    position_ = exponent;
                    digits();
                }
            }
            std::istringstream text(std::string(source_.substr(start, position_ - start)));
            text.imbue(std::locale::classic());
            double value = 0;
            if (!(text >> value) || text.peek() != std::char_traits<char>::eof()
                || !std::isfinite(value))
                fail("numeric constant must be finite");
            auto node = make(Op::constant, at);
            node->constant = value;
            return node;
        }
        const auto name = identifier();
        if (name == "close")
            return make(Op::close, at);
        Op op;
        if (name == "lag")
            op = Op::lag;
        else if (name == "return")
            op = Op::returns;
        else if (name == "mean")
            op = Op::mean;
        else if (name == "std")
            op = Op::deviation;
        else if (name == "min")
            op = Op::minimum;
        else if (name == "max")
            op = Op::maximum;
        else {
            position_ = at;
            fail("unknown field or function");
        }
        if (!take('('))
            fail("expected expression");
        auto child = expression(depth + 1);
        if (!take(','))
            fail("expected comma");
        whitespace();
        bool parameter = false;
        std::size_t period = 0;
        if (position_ < source_.size()
            && std::isdigit(static_cast<unsigned char>(source_[position_]))) {
            const auto parsed = std::from_chars(
                source_.data() + position_,
                source_.data() + source_.size(),
                period);
            if (parsed.ec != std::errc{})
                fail("window must be 1..10000");
            position_ = static_cast<std::size_t>(parsed.ptr - source_.data());
            if (!period || period > 10000)
                fail("window must be 1..10000");
        } else if (identifier() == "window")
            parameter = true;
        else
            fail("expected a positive integer or window");
        if (!take(')'))
            fail("expected closing parenthesis");
        auto node = make(op, at, std::move(child));
        node->parameter = parameter;
        node->period = period;
        if (op == Op::deviation && !parameter && period < 2)
            throw FeatureExpressionError(at, "standard deviation window must be at least 2");
        return node;
    }
};

std::size_t period(const Node& node, std::size_t window)
{
    const auto n = node.parameter ? window : node.period;
    if (node.op == Op::deviation && n < 2)
        throw FeatureExpressionError(node.position, "standard deviation window must be at least 2");
    return n;
}

std::size_t history(const Node& node, std::size_t window)
{
    const auto a = node.left ? history(*node.left, window) : 0,
               b = node.right ? history(*node.right, window) : 0;
    if (node.op == Op::lag || node.op == Op::returns)
        return a + period(node, window);
    if (node.op == Op::mean || node.op == Op::deviation || node.op == Op::minimum
        || node.op == Op::maximum)
        return a + period(node, window) - 1;
    return std::max(a, b);
}

void check(std::stop_token stop)
{
    if (stop.stop_requested())
        throw Error(ErrorCode::cancelled, "feature expression evaluation cancelled");
}

FeatureValue number(double value)
{
    return std::isfinite(value) ? FeatureValue{value, FeatureUndefined::none}
                                : FeatureValue{std::nullopt, FeatureUndefined::non_finite};
}

std::vector<FeatureValue> evaluate(
    const Node& node,
    std::span<const Decimal> closes,
    std::size_t window,
    std::stop_token stop)
{
    check(stop);
    std::vector<FeatureValue> result(closes.size());
    if (node.op == Op::close || node.op == Op::constant) {
        for (std::size_t i = 0; i < result.size(); ++i) {
            if (!(i % 4096))
                check(stop);
            result[i] = number(
                node.op == Op::close ? static_cast<double>(closes[i].raw()) / 100000000.0
                                     : node.constant);
        }
        return result;
    }
    const auto a = evaluate(*node.left, closes, window, stop);
    if (node.right) {
        const auto b = evaluate(*node.right, closes, window, stop);
        for (std::size_t i = 0; i < result.size(); ++i) {
            if (!(i % 4096))
                check(stop);
            if (!a[i].value) {
                result[i] = a[i];
                continue;
            }
            if (!b[i].value) {
                result[i] = b[i];
                continue;
            }
            const auto x = *a[i].value, y = *b[i].value;
            if (node.op == Op::divide && y == 0) {
                result[i] = {std::nullopt, FeatureUndefined::zero_denominator};
                continue;
            }
            result[i] = number(
                node.op == Op::add            ? x + y
                    : node.op == Op::subtract ? x - y
                    : node.op == Op::multiply ? x * y
                                              : x / y);
        }
        return result;
    }
    if (node.op == Op::neg) {
        for (std::size_t i = 0; i < result.size(); ++i) {
            if (!(i % 4096))
                check(stop);
            result[i] = a[i].value ? number(-*a[i].value) : a[i];
        }
        return result;
    }
    const auto n = period(node, window);
    if (node.op == Op::lag || node.op == Op::returns) {
        for (std::size_t i = n; i < result.size(); ++i) {
            if (!(i % 4096))
                check(stop);
            if (node.op == Op::lag)
                result[i] = a[i - n];
            else if (!a[i].value)
                result[i] = a[i];
            else if (!a[i - n].value)
                result[i] = a[i - n];
            else if (*a[i - n].value == 0)
                result[i] = {std::nullopt, FeatureUndefined::zero_denominator};
            else
                result[i] = number(*a[i].value / *a[i - n].value - 1);
        }
        return result;
    }
    std::size_t count = 0;
    long double mean = 0, m2 = 0;
    std::deque<std::size_t> invalid, extreme;
    const auto remove = [&](double x) {
        if (count == 1) {
            count = 0;
            mean = m2 = 0;
        } else {
            const auto next = (mean * count - x) / (count - 1);
            m2 -= (x - mean) * (x - next);
            mean = next;
            --count;
        }
    };
    const auto add = [&](double x) {
        ++count;
        const auto delta = x - mean;
        mean += delta / count;
        m2 += delta * (x - mean);
    };
    const auto needed = history(node, window);
    for (std::size_t i = 0; i < result.size(); ++i) {
        if (!(i % 4096))
            check(stop);
        if (i >= n && a[i - n].value)
            remove(*a[i - n].value);
        while (!invalid.empty() && invalid.front() + n <= i)
            invalid.pop_front();
        while (!extreme.empty() && extreme.front() + n <= i)
            extreme.pop_front();
        if (!a[i].value)
            invalid.push_back(i);
        else {
            add(*a[i].value);
            if (node.op == Op::minimum || node.op == Op::maximum) {
                while (!extreme.empty()
                    && (node.op == Op::minimum ? *a[extreme.back()].value >= *a[i].value
                                               : *a[extreme.back()].value <= *a[i].value))
                    extreme.pop_back();
                extreme.push_back(i);
            }
        }
        if (i < needed)
            continue;
        if (!invalid.empty()) {
            result[i] = a[invalid.front()];
            continue;
        }
        if (node.op == Op::minimum || node.op == Op::maximum)
            result[i] = a[extreme.front()];
        else
            result[i] = number(
                node.op == Op::mean ? static_cast<double>(mean)
                                    : static_cast<double>(std::sqrt(std::max(0.0L, m2) / (n - 1))));
    }
    return result;
}

} // namespace

struct FeatureExpression::Impl
{
    std::string source;
    std::unique_ptr<Node> root;
};

FeatureExpressionError::FeatureExpressionError(std::size_t position, std::string reason)
    : std::invalid_argument(
          "feature expression at byte " + std::to_string(position) + ": " + reason),
      position_(position),
      reason_(std::move(reason))
{}

FeatureExpression::FeatureExpression(std::string source)
{
    auto program = std::make_shared<Impl>();
    program->source = std::move(source);
    program->root = Parser(program->source).parse();
    program_ = std::move(program);
}

const std::string& FeatureExpression::source() const noexcept
{
    return program_->source;
}

int FeatureExpression::price_power() const noexcept
{
    return program_->root->power;
}

std::size_t FeatureExpression::history(std::size_t window) const
{
    if (!window || window > 10000)
        throw FeatureExpressionError(0, "window must be 1..10000");
    return asterion::history(*program_->root, window);
}

std::vector<FeatureValue> FeatureExpression::evaluate(
    std::span<const Decimal> closes,
    std::size_t window,
    std::stop_token stop) const
{
    const auto needed = history(window);
    auto values = asterion::evaluate(*program_->root, closes, window, stop);
    for (std::size_t i = 0; i < std::min(needed, values.size()); ++i) {
        if (!(i % 4096))
            check(stop);
        values[i] = {std::nullopt, FeatureUndefined::warmup};
    }
    return values;
}

} // namespace asterion
