#include "TabMotion.h"
#include <algorithm>
#include <cmath>

namespace header::motion {
namespace {
// One coordinate of the cubic bezier through (0,0), (p1), (p2), (1,1).
double Coordinate(double p1, double p2, double s) {
    const double r = 1 - s;
    return 3 * r * r * s * p1 + 3 * r * s * s * p2 + s * s * s;
}
double Derivative(double p1, double p2, double s) {
    const double r = 1 - s;
    return 3 * r * r * p1 + 6 * r * s * (p2 - p1) + 3 * s * s * (1 - p2);
}
// The curve parameter whose x equals u. x(s) is monotonic for CSS curves
// (x1, x2 in [0,1]), so bisection always converges; Newton steps refine it.
double Parameter(const Bezier& curve, double u) {
    double low = 0, high = 1, s = u;
    for (int i = 0; i < 8; ++i) {
        const double x = Coordinate(curve.x1, curve.x2, s) - u, slope = Derivative(curve.x1, curve.x2, s);
        if (std::abs(x) < 1e-9) return s;
        if (std::abs(slope) < 1e-6) break;
        const double next = s - x / slope;
        if (next < 0 || next > 1) break;
        s = next;
    }
    for (int i = 0; i < 60; ++i) {
        s = (low + high) / 2;
        if (Coordinate(curve.x1, curve.x2, s) < u) low = s; else high = s;
    }
    return s;
}
} // namespace

double Ease(const Bezier& curve, double u) {
    if (u <= 0) return 0;
    if (u >= 1) return 1;
    return Coordinate(curve.y1, curve.y2, Parameter(curve, u));
}

double Slope(const Bezier& curve, double u) {
    const double s = Parameter(curve, std::clamp(u, 0.0, 1.0));
    const double dx = Derivative(curve.x1, curve.x2, s), dy = Derivative(curve.y1, curve.y2, s);
    if (std::abs(dx) > 1e-9) return dy / dx;
    // Vertical tangent: estimate from a small step instead.
    const double h = 1e-4, a = std::max(0.0, u - h), b = std::min(1.0, u + h);
    return (Ease(curve, b) - Ease(curve, a)) / (b - a);
}

std::vector<Segment> Segments(const Bezier& curve, double from, double to, double seconds, int count) {
    std::vector<Segment> result;
    if (seconds <= 0 || count <= 0) return result;
    result.reserve(static_cast<size_t>(count));
    const double travel = to - from;
    auto boundary = [&](int k) { const double f = static_cast<double>(k) / count; return f * f; };
    for (int k = 0; k < count; ++k) {
        const double u0 = boundary(k), u1 = boundary(k + 1);
        const double t0 = u0 * seconds, h = (u1 - u0) * seconds;
        const double p0 = from + travel * Ease(curve, u0), p1 = from + travel * Ease(curve, u1);
        // Slopes in value per second.
        const double m0 = travel * Slope(curve, u0) / seconds, m1 = travel * Slope(curve, u1) / seconds;
        Segment segment{t0, p0, m0, (3 * (p1 - p0) / h - 2 * m0 - m1) / h, (2 * (p0 - p1) / h + m0 + m1) / (h * h)};
        result.push_back(segment);
    }
    return result;
}

double Evaluate(std::span<const Segment> segments, double seconds, double to, double t) {
    if (segments.empty() || t >= seconds) return to;
    if (t <= 0) return segments.front().a;
    const Segment* current = &segments.front();
    for (const auto& segment : segments) {
        if (segment.begin > t) break;
        current = &segment;
    }
    const double tau = t - current->begin;
    return current->a + tau * (current->b + tau * (current->c + tau * current->d));
}

double Transition::At(std::uint64_t now) const {
    if (milliseconds <= 0 || now >= End()) return to;
    if (now <= start) return from;
    const double u = static_cast<double>(now - start) / milliseconds;
    return from + (to - from) * Ease(curve, u);
}

Change Classify(std::span<const std::uint64_t> before, std::span<const std::uint64_t> after, int widthBefore,
                int widthAfter) {
    if (before.size() != after.size() || !std::is_permutation(before.begin(), before.end(), after.begin(), after.end()))
        return Change::Membership;
    if (!std::equal(before.begin(), before.end(), after.begin(), after.end())) return Change::Order;
    return widthBefore != widthAfter ? Change::Geometry : Change::None;
}

Timing TimingFor(Change change) {
    switch (change) {
    case Change::Membership: return {OpenMs, EaseOut};
    case Change::Order: return {ReorderMs, Photon};
    case Change::Geometry: return {SettleMs, EaseOut};
    case Change::None: break;
    }
    return {SettleMs, EaseOut};
}
}
