#pragma once
// Pure tab-strip motion math: CSS cubic-bezier easing, its approximation by the
// cubic time segments DirectComposition animations accept, and the timings
// Firefox uses for its tab strip (tabs.css).
#include <cstdint>
#include <span>
#include <vector>

namespace header::motion {
struct Bezier {
    double x1{}, y1{}, x2{1}, y2{1};
};
// CSS ease-out, and Firefox's "photon" reorder curve.
inline constexpr Bezier EaseOut{0, 0, .58, 1};
inline constexpr Bezier Photon{.07, .95, 0, 1};

inline constexpr int OpenMs = 100;    // a new tab grows in
inline constexpr int CloseMs = 100;   // a closed tab shrinks away
inline constexpr int SettleMs = 150;  // width unlock, scrolling, edge fades
inline constexpr int ReorderMs = 200; // neighbours slide aside while dragging

// Progress (0..1) of the curve at time fraction u (0..1), and its slope.
double Ease(const Bezier& curve, double u);
double Slope(const Bezier& curve, double u);

// value(t) = a + b*tau + c*tau^2 + d*tau^3 with tau = t - begin, in seconds.
struct Segment {
    double begin{}, a{}, b{}, c{}, d{};
};
// Hermite segments, denser near the start where these curves are steep.
std::vector<Segment> Segments(const Bezier& curve, double from, double to, double seconds, int count = 20);
double Evaluate(std::span<const Segment> segments, double seconds, double to, double t);

// One animated scalar, so a new target can start from the value on screen.
struct Transition {
    double from{}, to{};
    std::uint64_t start{};
    int milliseconds{};
    Bezier curve{EaseOut};
    double At(std::uint64_t now) const;
    bool Running(std::uint64_t now) const { return milliseconds > 0 && now < start + static_cast<std::uint64_t>(milliseconds); }
    std::uint64_t End() const { return start + static_cast<std::uint64_t>(milliseconds); }
};

// What changed between two tab sequences, choosing the curve and duration.
enum class Change { None, Membership, Order, Geometry };
Change Classify(std::span<const std::uint64_t> before, std::span<const std::uint64_t> after, int widthBefore,
                int widthAfter);
struct Timing {
    int milliseconds{};
    Bezier curve{EaseOut};
};
Timing TimingFor(Change change);
}
