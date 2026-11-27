// High-performance strictly causal C++ kernels for asterion_strategy operators.
// Rolling moments and regressions use a two-stack sliding monoid queue (Chan/Pebay merge)
// so each bar is O(1) amortized with zero subtraction drift and exact NaN invalidation.
#include "operators.hpp"

#include <pybind11/numpy.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <vector>

namespace py = pybind11;

namespace asterion {
namespace {

using FloatArray = py::array_t<double, py::array::c_style | py::array::forcecast>;
constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

// Fixed-width sliding window aggregator over any associative monoid Traits.
// Chronological order is preserved: merge(left, right) always receives older items in left.
template <typename Traits>
class SlidingMonoid
{
public:
    using State = typename Traits::State;

    explicit SlidingMonoid(std::size_t window)
        : window_(window)
    {
        back_items_.reserve(window);
        front_acc_.reserve(window);
    }

    void push(const State& item)
    {
        if (back_items_.size() + front_acc_.size() == window_) {
            if (front_acc_.empty()) {
                const std::size_t k = back_items_.size();
                front_acc_.push_back(back_items_[k - 1]);
                for (std::size_t idx = k - 1; idx > 0; --idx)
                    front_acc_.push_back(Traits::merge(back_items_[idx - 1], front_acc_.back()));
                back_items_.clear();
            }
            front_acc_.pop_back();
        }
        back_acc_ = back_items_.empty() ? item : Traits::merge(back_acc_, item);
        back_items_.push_back(item);
    }

    bool full() const noexcept
    {
        return back_items_.size() + front_acc_.size() == window_;
    }

    State query() const noexcept
    {
        if (front_acc_.empty())
            return back_acc_;
        if (back_items_.empty())
            return front_acc_.back();
        return Traits::merge(front_acc_.back(), back_acc_);
    }

private:
    std::size_t window_;
    std::vector<State> back_items_;
    std::vector<State> front_acc_;
    State back_acc_{};
};

// Maintains the finite values of the active sliding window in ascending order using memmove.
class SortedWindow
{
public:
    explicit SortedWindow(std::size_t window)
        : buf_(window)
    {}

    void step(bool has_old, double old_val, double new_val) noexcept
    {
        const bool old_ok = has_old && std::isfinite(old_val);
        const bool new_ok = std::isfinite(new_val);
        if (old_ok && new_ok) {
            replace(old_val, new_val);
        } else {
            if (old_ok)
                erase(old_val);
            if (new_ok)
                insert(new_val);
        }
    }

    std::size_t size() const noexcept
    {
        return size_;
    }

    const double* data() const noexcept
    {
        return buf_.data();
    }

private:
    std::size_t lower_idx(double val) const noexcept
    {
        std::size_t lo = 0;
        std::size_t hi = size_;
        while (lo < hi) {
            const std::size_t mid = lo + ((hi - lo) >> 1);
            if (buf_[mid] < val)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    }

    void insert(double val) noexcept
    {
        const std::size_t pos = lower_idx(val);
        if (pos < size_)
            std::memmove(buf_.data() + pos + 1, buf_.data() + pos, (size_ - pos) * sizeof(double));
        buf_[pos] = val;
        ++size_;
    }

    void erase(double val) noexcept
    {
        const std::size_t pos = lower_idx(val);
        if (pos < size_ && buf_[pos] == val) {
            --size_;
            if (pos < size_) {
                std::memmove(
                    buf_.data() + pos,
                    buf_.data() + pos + 1,
                    (size_ - pos) * sizeof(double));
            }
        }
    }

    void replace(double old_val, double new_val) noexcept
    {
        const std::size_t old_pos = lower_idx(old_val);
        const std::size_t new_pos = lower_idx(new_val);
        if (old_pos < new_pos) {
            const std::size_t count = new_pos - old_pos - 1;
            if (count > 0) {
                std::memmove(
                    buf_.data() + old_pos,
                    buf_.data() + old_pos + 1,
                    count * sizeof(double));
            }
            buf_[new_pos - 1] = new_val;
        } else if (new_pos < old_pos) {
            const std::size_t count = old_pos - new_pos;
            std::memmove(buf_.data() + new_pos + 1, buf_.data() + new_pos, count * sizeof(double));
            buf_[new_pos] = new_val;
        } else {
            buf_[old_pos] = new_val;
        }
    }

    std::vector<double> buf_;
    std::size_t size_{0};
};

struct MeanTraits
{
    struct State
    {
        int n{0};
        double mean{0.0};
        bool valid{false};
    };

    static State single(double x) noexcept
    {
        return {1, std::isfinite(x) ? x : 0.0, std::isfinite(x)};
    }

    static State merge(const State& a, const State& b) noexcept
    {
        if (!a.valid || !b.valid)
            return {a.n + b.n, 0.0, false};
        const int n = a.n + b.n;
        const double delta = b.mean - a.mean;
        return {n, a.mean + delta * (static_cast<double>(b.n) / static_cast<double>(n)), true};
    }
};

struct VarTraits
{
    struct State
    {
        int n{0};
        double mean{0.0};
        double m2{0.0};
        bool valid{false};
    };

    static State single(double x) noexcept
    {
        return {1, std::isfinite(x) ? x : 0.0, 0.0, std::isfinite(x)};
    }

    static State merge(const State& a, const State& b) noexcept
    {
        if (!a.valid || !b.valid)
            return {a.n + b.n, 0.0, 0.0, false};
        const int n = a.n + b.n;
        const double na = static_cast<double>(a.n);
        const double nb = static_cast<double>(b.n);
        const double nd = static_cast<double>(n);
        const double delta = b.mean - a.mean;
        const double mean = a.mean + delta * (nb / nd);
        const double m2 = a.m2 + b.m2 + delta * delta * (na * nb / nd);
        return {n, mean, m2, true};
    }
};

// Pebay (2008), "Formulas for Robust, One-Pass Parallel Computation of Covariances
// and Arbitrary-Order Statistical Moments", Sandia Report SAND2008-6212.
struct Moment3Traits
{
    struct State
    {
        int n{0};
        double mean{0.0};
        double m2{0.0};
        double m3{0.0};
        bool valid{false};
    };

    static State single(double x) noexcept
    {
        return {1, std::isfinite(x) ? x : 0.0, 0.0, 0.0, std::isfinite(x)};
    }

    static State merge(const State& a, const State& b) noexcept
    {
        if (!a.valid || !b.valid)
            return {a.n + b.n, 0.0, 0.0, 0.0, false};
        const int n = a.n + b.n;
        const double na = static_cast<double>(a.n);
        const double nb = static_cast<double>(b.n);
        const double nd = static_cast<double>(n);
        const double d = b.mean - a.mean;
        const double d_n = d / nd;
        const double term1 = d * d_n * na * nb;
        const double mean = a.mean + nb * d_n;
        const double m2 = a.m2 + b.m2 + term1;
        const double m3 =
            a.m3 + b.m3 + term1 * d_n * (na - nb) + 3.0 * d_n * (na * b.m2 - nb * a.m2);
        return {n, mean, m2, m3, true};
    }
};

struct Moment4Traits
{
    struct State
    {
        int n{0};
        double mean{0.0};
        double m2{0.0};
        double m3{0.0};
        double m4{0.0};
        bool valid{false};
    };

    static State single(double x) noexcept
    {
        return {1, std::isfinite(x) ? x : 0.0, 0.0, 0.0, 0.0, std::isfinite(x)};
    }

    static State merge(const State& a, const State& b) noexcept
    {
        if (!a.valid || !b.valid)
            return {a.n + b.n, 0.0, 0.0, 0.0, 0.0, false};
        const int n = a.n + b.n;
        const double na = static_cast<double>(a.n);
        const double nb = static_cast<double>(b.n);
        const double nd = static_cast<double>(n);
        const double d = b.mean - a.mean;
        const double d_n = d / nd;
        const double d_n2 = d_n * d_n;
        const double term1 = d * d_n * na * nb;
        const double mean = a.mean + nb * d_n;
        const double m2 = a.m2 + b.m2 + term1;
        const double m3 =
            a.m3 + b.m3 + term1 * d_n * (na - nb) + 3.0 * d_n * (na * b.m2 - nb * a.m2);
        const double m4 = a.m4 + b.m4 + term1 * d_n2 * (na * na - na * nb + nb * nb)
            + 6.0 * d_n2 * (na * na * b.m2 + nb * nb * a.m2) + 4.0 * d_n * (na * b.m3 - nb * a.m3);
        return {n, mean, m2, m3, m4, true};
    }
};

struct CovTraits
{
    struct State
    {
        int n{0};
        double mx{0.0};
        double my{0.0};
        double sxx{0.0};
        double syy{0.0};
        double sxy{0.0};
        bool valid{false};
    };

    static State single(double x, double y) noexcept
    {
        const bool ok = std::isfinite(x) && std::isfinite(y);
        return {1, ok ? x : 0.0, ok ? y : 0.0, 0.0, 0.0, 0.0, ok};
    }

    static State merge(const State& a, const State& b) noexcept
    {
        if (!a.valid || !b.valid)
            return {a.n + b.n, 0.0, 0.0, 0.0, 0.0, 0.0, false};
        const int n = a.n + b.n;
        const double na = static_cast<double>(a.n);
        const double nb = static_cast<double>(b.n);
        const double nd = static_cast<double>(n);
        const double dx = b.mx - a.mx;
        const double dy = b.my - a.my;
        const double factor = na * nb / nd;
        const double mx = a.mx + dx * (nb / nd);
        const double my = a.my + dy * (nb / nd);
        const double sxx = a.sxx + b.sxx + dx * dx * factor;
        const double syy = a.syy + b.syy + dy * dy * factor;
        const double sxy = a.sxy + b.sxy + dx * dy * factor;
        return {n, mx, my, sxx, syy, sxy, true};
    }
};

struct DecayLinearTraits
{
    struct State
    {
        int n{0};
        double sum{0.0};
        double wsum{0.0};
        bool valid{false};
    };

    static State single(double x) noexcept
    {
        const bool ok = std::isfinite(x);
        return {1, ok ? x : 0.0, ok ? x : 0.0, ok};
    }

    static State merge(const State& left, const State& right) noexcept
    {
        if (!left.valid || !right.valid)
            return {left.n + right.n, 0.0, 0.0, false};
        return {
            left.n + right.n,
            left.sum + right.sum,
            left.wsum + right.wsum + static_cast<double>(left.n) * right.sum,
            true
        };
    }
};

struct VwapTraits
{
    struct State
    {
        double v_sum{0.0};
        double pv_sum{0.0};
        bool valid{false};
    };

    static State single(double c, double v) noexcept
    {
        const bool ok = std::isfinite(c) && std::isfinite(v) && v >= 0.0;
        return {ok ? v : 0.0, ok ? c * v : 0.0, ok};
    }

    static State merge(const State& a, const State& b) noexcept
    {
        if (!a.valid || !b.valid)
            return {0.0, 0.0, false};
        return {a.v_sum + b.v_sum, a.pv_sum + b.pv_sum, true};
    }
};

py::array_t<double> ts_mean_cpp(const FloatArray& values, int window)
{
    const auto n = values.size();
    py::array_t<double> out(n);
    auto* dst = out.mutable_data();
    const auto* src = values.data();
    {
        py::gil_scoped_release release;
        std::fill_n(dst, n, kNan);
        if (n >= window) {
            SlidingMonoid<MeanTraits> queue(static_cast<std::size_t>(window));
            for (py::ssize_t i = 0; i < n; ++i) {
                queue.push(MeanTraits::single(src[i]));
                if (queue.full()) {
                    const auto win = queue.query();
                    if (win.valid)
                        dst[i] = win.mean;
                }
            }
        }
    }
    return out;
}

py::array_t<double> ts_std_cpp(const FloatArray& values, int window, int ddof)
{
    const auto n = values.size();
    py::array_t<double> out(n);
    auto* dst = out.mutable_data();
    const auto* src = values.data();
    {
        py::gil_scoped_release release;
        std::fill_n(dst, n, kNan);
        if (n >= window) {
            const double denom = static_cast<double>(window - ddof);
            SlidingMonoid<VarTraits> queue(static_cast<std::size_t>(window));
            for (py::ssize_t i = 0; i < n; ++i) {
                queue.push(VarTraits::single(src[i]));
                if (queue.full()) {
                    const auto win = queue.query();
                    if (win.valid)
                        dst[i] = std::sqrt(std::max(0.0, win.m2 / denom));
                }
            }
        }
    }
    return out;
}

py::array_t<double> ts_zscore_cpp(const FloatArray& values, int window, int ddof)
{
    const auto n = values.size();
    py::array_t<double> out(n);
    auto* dst = out.mutable_data();
    const auto* src = values.data();
    {
        py::gil_scoped_release release;
        std::fill_n(dst, n, kNan);
        if (n >= window) {
            const double denom = static_cast<double>(window - ddof);
            SlidingMonoid<VarTraits> queue(static_cast<std::size_t>(window));
            for (py::ssize_t i = 0; i < n; ++i) {
                queue.push(VarTraits::single(src[i]));
                if (queue.full()) {
                    const auto win = queue.query();
                    if (win.valid) {
                        const double sigma = std::sqrt(std::max(0.0, win.m2 / denom));
                        dst[i] = sigma > 0.0 ? (src[i] - win.mean) / sigma : 0.0;
                    }
                }
            }
        }
    }
    return out;
}

py::array_t<double> ts_rank_cpp(const FloatArray& values, int window)
{
    const auto n = values.size();
    py::array_t<double> out(n);
    auto* dst = out.mutable_data();
    const auto* src = values.data();
    {
        py::gil_scoped_release release;
        std::fill_n(dst, n, kNan);
        if (n >= window) {
            if (window == 1) {
                for (py::ssize_t i = 0; i < n; ++i) {
                    if (std::isfinite(src[i]))
                        dst[i] = 0.5;
                }
            } else {
                const double inv_denom = 1.0 / static_cast<double>(window - 1);
                const std::size_t w = static_cast<std::size_t>(window);
                SortedWindow sorted(w);
                for (py::ssize_t i = 0; i < n; ++i) {
                    sorted.step(i >= window, i >= window ? src[i - window] : 0.0, src[i]);
                    if (sorted.size() == w) {
                        const double cur = src[i];
                        const double* begin = sorted.data();
                        const double* end = begin + w;
                        const auto less =
                            static_cast<std::size_t>(std::lower_bound(begin, end, cur) - begin);
                        const auto upper = static_cast<std::size_t>(
                            std::upper_bound(begin + less, end, cur) - begin);
                        const auto equal = upper - less;
                        dst[i] = (static_cast<double>(less) + 0.5 * static_cast<double>(equal - 1))
                            * inv_denom;
                    }
                }
            }
        }
    }
    return out;
}

py::array_t<double> ts_corr_cpp(const FloatArray& x, const FloatArray& y, int window)
{
    const auto n = x.size();
    py::array_t<double> out(n);
    auto* dst = out.mutable_data();
    const auto* xp = x.data();
    const auto* yp = y.data();
    {
        py::gil_scoped_release release;
        std::fill_n(dst, n, kNan);
        if (n >= window) {
            SlidingMonoid<CovTraits> queue(static_cast<std::size_t>(window));
            for (py::ssize_t i = 0; i < n; ++i) {
                queue.push(CovTraits::single(xp[i], yp[i]));
                if (queue.full()) {
                    const auto win = queue.query();
                    if (win.valid) {
                        const double denom =
                            std::sqrt(std::max(0.0, win.sxx) * std::max(0.0, win.syy));
                        if (denom > 0.0)
                            dst[i] = std::clamp(win.sxy / denom, -1.0, 1.0);
                    }
                }
            }
        }
    }
    return out;
}

py::array_t<double> decay_linear_cpp(const FloatArray& values, int window)
{
    const auto n = values.size();
    py::array_t<double> out(n);
    auto* dst = out.mutable_data();
    const auto* src = values.data();
    {
        py::gil_scoped_release release;
        std::fill_n(dst, n, kNan);
        if (n >= window) {
            const double weight_sum =
                0.5 * static_cast<double>(window) * static_cast<double>(window + 1);
            SlidingMonoid<DecayLinearTraits> queue(static_cast<std::size_t>(window));
            for (py::ssize_t i = 0; i < n; ++i) {
                queue.push(DecayLinearTraits::single(src[i]));
                if (queue.full()) {
                    const auto win = queue.query();
                    if (win.valid)
                        dst[i] = win.wsum / weight_sum;
                }
            }
        }
    }
    return out;
}

py::tuple rolling_ols_cpp(const FloatArray& y, const FloatArray& x, int window)
{
    const auto n = y.size();
    py::array_t<double> beta_out(n), resid_out(n), r2_out(n);
    auto* bp = beta_out.mutable_data();
    auto* rp = resid_out.mutable_data();
    auto* r2p = r2_out.mutable_data();
    const auto* yp = y.data();
    const auto* xp = x.data();
    {
        py::gil_scoped_release release;
        std::fill_n(bp, n, kNan);
        std::fill_n(rp, n, kNan);
        std::fill_n(r2p, n, kNan);
        if (n >= window) {
            SlidingMonoid<CovTraits> queue(static_cast<std::size_t>(window));
            for (py::ssize_t i = 0; i < n; ++i) {
                queue.push(CovTraits::single(xp[i], yp[i]));
                if (queue.full()) {
                    const auto win = queue.query();
                    if (win.valid && win.sxx > 1e-24) {
                        const double beta = win.sxy / win.sxx;
                        bp[i] = beta;
                        rp[i] = (yp[i] - win.my) - beta * (xp[i] - win.mx);
                        if (win.syy > 1e-24)
                            r2p[i] =
                                std::clamp((win.sxy * win.sxy) / (win.sxx * win.syy), 0.0, 1.0);
                    }
                }
            }
        }
    }
    return py::make_tuple(beta_out, resid_out, r2_out);
}

py::array_t<double> ts_autocorr_cpp(const FloatArray& values, int window, int lag)
{
    const auto n = values.size();
    py::array_t<double> out(n);
    auto* dst = out.mutable_data();
    const auto* src = values.data();
    {
        py::gil_scoped_release release;
        std::fill_n(dst, n, kNan);
        if (n >= window) {
            SlidingMonoid<CovTraits> queue(static_cast<std::size_t>(window - lag));
            int invalid_in_window = 0;
            for (py::ssize_t i = 0; i < n; ++i) {
                if (!std::isfinite(src[i]))
                    ++invalid_in_window;
                if (i >= window && !std::isfinite(src[i - window]))
                    --invalid_in_window;
                if (i >= lag)
                    queue.push(CovTraits::single(src[i - lag], src[i]));
                if (i >= window - 1 && invalid_in_window == 0 && queue.full()) {
                    const auto win = queue.query();
                    if (win.valid) {
                        const double denom =
                            std::sqrt(std::max(0.0, win.sxx) * std::max(0.0, win.syy));
                        if (denom > 1e-24)
                            dst[i] = std::clamp(win.sxy / denom, -1.0, 1.0);
                    }
                }
            }
        }
    }
    return out;
}

py::array_t<double> ts_skew_cpp(const FloatArray& values, int window)
{
    const auto n = values.size();
    py::array_t<double> out(n);
    auto* dst = out.mutable_data();
    const auto* src = values.data();
    {
        py::gil_scoped_release release;
        std::fill_n(dst, n, kNan);
        if (n >= window) {
            const double inv_w = 1.0 / static_cast<double>(window);
            SlidingMonoid<Moment3Traits> queue(static_cast<std::size_t>(window));
            for (py::ssize_t i = 0; i < n; ++i) {
                queue.push(Moment3Traits::single(src[i]));
                if (queue.full()) {
                    const auto win = queue.query();
                    if (win.valid) {
                        const double m2 = win.m2 * inv_w;
                        if (m2 > 1e-24)
                            dst[i] = (win.m3 * inv_w) / std::pow(m2, 1.5);
                    }
                }
            }
        }
    }
    return out;
}

py::array_t<double> ts_kurt_cpp(const FloatArray& values, int window)
{
    const auto n = values.size();
    py::array_t<double> out(n);
    auto* dst = out.mutable_data();
    const auto* src = values.data();
    {
        py::gil_scoped_release release;
        std::fill_n(dst, n, kNan);
        if (n >= window) {
            const double inv_w = 1.0 / static_cast<double>(window);
            SlidingMonoid<Moment4Traits> queue(static_cast<std::size_t>(window));
            for (py::ssize_t i = 0; i < n; ++i) {
                queue.push(Moment4Traits::single(src[i]));
                if (queue.full()) {
                    const auto win = queue.query();
                    if (win.valid) {
                        const double m2 = win.m2 * inv_w;
                        if (m2 > 1e-24)
                            dst[i] = (win.m4 * inv_w) / (m2 * m2) - 3.0;
                    }
                }
            }
        }
    }
    return out;
}

py::array_t<double> ts_variance_ratio_cpp(const FloatArray& close, int window, int q)
{
    const auto n = close.size();
    py::array_t<double> out(n);
    auto* dst = out.mutable_data();
    const auto* src = close.data();
    {
        py::gil_scoped_release release;
        std::fill_n(dst, n, kNan);
        if (n > window) {
            std::vector<double> log_p(static_cast<std::size_t>(n), kNan);
            for (py::ssize_t i = 0; i < n; ++i) {
                if (std::isfinite(src[i]) && src[i] > 0.0)
                    log_p[static_cast<std::size_t>(i)] = std::log(src[i]);
            }
            const int m = window - q + 1;
            const double md = static_cast<double>(m);
            const double qd = static_cast<double>(q);
            const double denom_q = qd * md * (1.0 - qd / static_cast<double>(window));
            const double inv_w_minus_1 = 1.0 / static_cast<double>(window - 1);
            SlidingMonoid<VarTraits> ret_queue(static_cast<std::size_t>(window));
            SlidingMonoid<VarTraits> sum_queue(static_cast<std::size_t>(m));
            for (py::ssize_t i = 1; i < n; ++i) {
                const double r =
                    log_p[static_cast<std::size_t>(i)] - log_p[static_cast<std::size_t>(i - 1)];
                ret_queue.push(VarTraits::single(r));
                if (i >= q) {
                    const double sq =
                        log_p[static_cast<std::size_t>(i)] - log_p[static_cast<std::size_t>(i - q)];
                    sum_queue.push(VarTraits::single(sq));
                }
                if (i >= window && ret_queue.full() && sum_queue.full()) {
                    const auto rw = ret_queue.query();
                    const auto sw = sum_queue.query();
                    if (rw.valid && sw.valid) {
                        const double var1 = rw.m2 * inv_w_minus_1;
                        if (var1 > 1e-24) {
                            // Parallel-axis identity: sum (s_t - q*mu)^2 = M2_s + m*(mu_s - q*mu)^2.
                            const double diff = sw.mean - qd * rw.mean;
                            const double var_q = (sw.m2 + md * diff * diff) / denom_q;
                            dst[i] = var_q / var1;
                        }
                    }
                }
            }
        }
    }
    return out;
}

py::array_t<double> ts_hurst_cpp(const FloatArray& close, int window, int max_lag)
{
    const auto n = close.size();
    py::array_t<double> out(n);
    auto* dst = out.mutable_data();
    const auto* src = close.data();
    {
        py::gil_scoped_release release;
        std::fill_n(dst, n, kNan);
        if (n >= window) {
            const int upper = std::min(max_lag, window / 2);
            const std::size_t num_lags = static_cast<std::size_t>(upper - 1);
            std::vector<double> x_c(num_lags);
            double mean_log_lag = 0.0;
            for (std::size_t k = 0; k < num_lags; ++k) {
                x_c[k] = std::log(static_cast<double>(k + 2));
                mean_log_lag += x_c[k];
            }
            mean_log_lag /= static_cast<double>(num_lags);
            double sxx = 0.0;
            for (double& v : x_c) {
                v -= mean_log_lag;
                sxx += v * v;
            }
            const double inv_sxx = 1.0 / sxx;
            const double inv_lags = 1.0 / static_cast<double>(num_lags);

            std::vector<double> log_p(static_cast<std::size_t>(n), kNan);
            for (py::ssize_t i = 0; i < n; ++i) {
                if (std::isfinite(src[i]) && src[i] > 0.0)
                    log_p[static_cast<std::size_t>(i)] = std::log(src[i]);
            }

            std::vector<SlidingMonoid<MeanTraits>> queues;
            queues.reserve(num_lags);
            for (int tau = 2; tau <= upper; ++tau)
                queues.emplace_back(static_cast<std::size_t>(window - tau));
            std::vector<double> log_rms(num_lags);

            int invalid = 0;
            for (py::ssize_t i = 0; i < n; ++i) {
                const double lp = log_p[static_cast<std::size_t>(i)];
                if (!std::isfinite(lp))
                    ++invalid;
                if (i >= window && !std::isfinite(log_p[static_cast<std::size_t>(i - window)]))
                    --invalid;
                for (int tau = 2; tau <= upper; ++tau) {
                    if (i >= tau) {
                        const double d = lp - log_p[static_cast<std::size_t>(i - tau)];
                        queues[static_cast<std::size_t>(tau - 2)].push(MeanTraits::single(d * d));
                    }
                }
                if (i >= window - 1 && invalid == 0) {
                    bool ok = true;
                    double mean_y = 0.0;
                    for (std::size_t k = 0; k < num_lags; ++k) {
                        const auto win = queues[k].query();
                        if (!win.valid || win.mean <= 1e-24) {
                            ok = false;
                            break;
                        }
                        const double y = 0.5 * std::log(win.mean);
                        log_rms[k] = y;
                        mean_y += y;
                    }
                    if (ok) {
                        mean_y *= inv_lags;
                        double dot = 0.0;
                        for (std::size_t k = 0; k < num_lags; ++k)
                            dot += (log_rms[k] - mean_y) * x_c[k];
                        dst[i] = dot * inv_sxx;
                    }
                }
            }
        }
    }
    return out;
}

py::array_t<double> ts_vwap_dev_cpp(const FloatArray& close, const FloatArray& volume, int window)
{
    const auto n = close.size();
    py::array_t<double> out(n);
    auto* dst = out.mutable_data();
    const auto* cp = close.data();
    const auto* vp = volume.data();
    {
        py::gil_scoped_release release;
        std::fill_n(dst, n, kNan);
        if (n >= window) {
            SlidingMonoid<VwapTraits> queue(static_cast<std::size_t>(window));
            for (py::ssize_t i = 0; i < n; ++i) {
                queue.push(VwapTraits::single(cp[i], vp[i]));
                if (queue.full()) {
                    const auto win = queue.query();
                    if (win.valid && win.v_sum > 0.0 && win.pv_sum > 0.0) {
                        const double vwap = win.pv_sum / win.v_sum;
                        dst[i] = cp[i] / vwap - 1.0;
                    }
                }
            }
        }
    }
    return out;
}

py::array_t<double> ts_expected_shortfall_cpp(const FloatArray& values, int window, int count)
{
    const auto n = values.size();
    py::array_t<double> out(n);
    auto* dst = out.mutable_data();
    const auto* src = values.data();
    {
        py::gil_scoped_release release;
        std::fill_n(dst, n, kNan);
        if (n >= window) {
            const std::size_t w = static_cast<std::size_t>(window);
            SortedWindow sorted(w);
            const double inv_count = 1.0 / static_cast<double>(count);
            for (py::ssize_t i = 0; i < n; ++i) {
                sorted.step(i >= window, i >= window ? src[i - window] : 0.0, src[i]);
                if (sorted.size() == w) {
                    const double* data = sorted.data();
                    double sum = 0.0;
                    for (int j = 0; j < count; ++j)
                        sum += data[j];
                    dst[i] = -(sum * inv_count);
                }
            }
        }
    }
    return out;
}

py::array_t<double> ts_ema_cpp(const FloatArray& values, int span)
{
    const auto n = values.size();
    py::array_t<double> out(n);
    auto* dst = out.mutable_data();
    const auto* src = values.data();
    {
        py::gil_scoped_release release;
        std::fill_n(dst, n, kNan);
        const double alpha = 2.0 / (static_cast<double>(span) + 1.0);
        const double decay = 1.0 - alpha;
        double ema = 0.0;
        bool has_ema = false;
        int seen = 0;
        for (py::ssize_t i = 0; i < n; ++i) {
            const double val = src[i];
            if (!std::isfinite(val))
                continue;
            ema = has_ema ? (decay * ema + alpha * val) : val;
            has_ema = true;
            if (++seen >= span)
                dst[i] = ema;
        }
    }
    return out;
}

py::array_t<double> ts_ewma_vol_cpp(const FloatArray& close, int span, double scale)
{
    const auto n = close.size();
    py::array_t<double> out(n);
    auto* dst = out.mutable_data();
    const auto* src = close.data();
    {
        py::gil_scoped_release release;
        std::fill_n(dst, n, kNan);
        if (n >= 2) {
            const double alpha = 2.0 / (static_cast<double>(span) + 1.0);
            const double decay = 1.0 - alpha;
            double var = 0.0;
            bool has_var = false;
            int seen = 0;
            for (py::ssize_t i = 1; i < n; ++i) {
                const double prev = src[i - 1];
                const double cur = src[i];
                if (!(std::isfinite(prev) && std::isfinite(cur) && prev > 0.0 && cur > 0.0))
                    continue;
                const double r = std::log(cur / prev);
                const double sq = r * r;
                var = has_var ? (decay * var + alpha * sq) : sq;
                has_var = true;
                if (++seen >= span)
                    dst[i] = std::sqrt(var) * scale;
            }
        }
    }
    return out;
}

py::array_t<double> kalman_filter_cpp(const FloatArray& values, double q, double r)
{
    const auto n = values.size();
    py::array_t<double> out(n);
    auto* dst = out.mutable_data();
    const auto* src = values.data();
    {
        py::gil_scoped_release release;
        std::fill_n(dst, n, kNan);
        double x_hat = 0.0;
        double p = r;
        bool has_x = false;
        for (py::ssize_t i = 0; i < n; ++i) {
            const double z = src[i];
            if (!std::isfinite(z)) {
                if (has_x)
                    p += q;
                continue;
            }
            if (!has_x) {
                x_hat = z;
                p = r;
                has_x = true;
            } else {
                const double p_pred = p + q;
                const double gain = p_pred / (p_pred + r);
                x_hat += gain * (z - x_hat);
                p = (1.0 - gain) * p_pred;
            }
            dst[i] = x_hat;
        }
    }
    return out;
}

py::tuple kalman_hedge_cpp(const FloatArray& y, const FloatArray& x, double q, double r)
{
    const auto n = y.size();
    py::array_t<double> beta_out(n), intercept_out(n), spread_out(n);
    auto* bp = beta_out.mutable_data();
    auto* ip = intercept_out.mutable_data();
    auto* sp = spread_out.mutable_data();
    const auto* yp = y.data();
    const auto* xp = x.data();
    {
        py::gil_scoped_release release;
        std::fill_n(bp, n, kNan);
        std::fill_n(ip, n, kNan);
        std::fill_n(sp, n, kNan);
        double t0 = 0.0, t1 = 0.0;
        double p00 = 1e6, p01 = 0.0, p11 = 1e6;
        bool seen = false;
        for (py::ssize_t i = 0; i < n; ++i) {
            const double yt = yp[i];
            const double xt = xp[i];
            if (!(std::isfinite(yt) && std::isfinite(xt))) {
                if (seen) {
                    p00 += q;
                    p11 += q;
                }
                continue;
            }
            const double p00_p = seen ? (p00 + q) : p00;
            const double p01_p = p01;
            const double p11_p = seen ? (p11 + q) : p11;
            const double err = yt - (t0 * xt + t1);
            if (seen)
                sp[i] = err;
            const double s_var = xt * (p00_p * xt + p01_p) + (p01_p * xt + p11_p) + r;
            const double k0 = (p00_p * xt + p01_p) / s_var;
            const double k1 = (p01_p * xt + p11_p) / s_var;
            t0 += k0 * err;
            t1 += k1 * err;
            const double a00 = 1.0 - k0 * xt;
            const double a01 = -k0;
            const double a10 = -k1 * xt;
            const double a11 = 1.0 - k1;
            const double b00 = a00 * p00_p + a01 * p01_p;
            const double b01 = a00 * p01_p + a01 * p11_p;
            const double b10 = a10 * p00_p + a11 * p01_p;
            const double b11 = a10 * p01_p + a11 * p11_p;
            p00 = b00 * a00 + b01 * a01 + k0 * r * k0;
            p01 = 0.5 * ((b00 * a10 + b01 * a11) + (b10 * a00 + b11 * a01)) + k0 * r * k1;
            p11 = b10 * a10 + b11 * a11 + k1 * r * k1;
            seen = true;
            bp[i] = t0;
            ip[i] = t1;
        }
    }
    return py::make_tuple(beta_out, intercept_out, spread_out);
}

} // namespace

void register_operators(py::module_& module)
{
    module.def("ts_mean", &ts_mean_cpp);
    module.def("ts_std", &ts_std_cpp);
    module.def("ts_zscore", &ts_zscore_cpp);
    module.def("ts_rank", &ts_rank_cpp);
    module.def("ts_corr", &ts_corr_cpp);
    module.def("decay_linear", &decay_linear_cpp);
    module.def("rolling_ols", &rolling_ols_cpp);
    module.def("ts_autocorr", &ts_autocorr_cpp);
    module.def("ts_skew", &ts_skew_cpp);
    module.def("ts_kurt", &ts_kurt_cpp);
    module.def("ts_variance_ratio", &ts_variance_ratio_cpp);
    module.def("ts_hurst", &ts_hurst_cpp);
    module.def("ts_vwap_dev", &ts_vwap_dev_cpp);
    module.def("ts_expected_shortfall", &ts_expected_shortfall_cpp);
    module.def("ts_ema", &ts_ema_cpp);
    module.def("ts_ewma_vol", &ts_ewma_vol_cpp);
    module.def("kalman_filter", &kalman_filter_cpp);
    module.def("kalman_hedge", &kalman_hedge_cpp);
}

} // namespace asterion
