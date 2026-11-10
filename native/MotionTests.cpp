// Pure motion math: easing accuracy against the exact curve, segment fit for
// DirectComposition, transitions and change classification.
#include "TabMotion.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace header::motion;
namespace {
int checks = 0, failures = 0;
void Check(const char* name, bool pass) {
    ++checks;
    if (!pass) { ++failures; std::printf("FAIL %s\n", name); }
}
// Largest gap between the segment fit and the exact curve, as a share of travel.
double FitError(const Bezier& curve, double from, double to, double seconds) {
    const auto segments = Segments(curve, from, to, seconds);
    double worst = 0;
    for (int i = 0; i <= 2000; ++i) {
        const double t = seconds * i / 2000.0;
        const double exact = from + (to - from) * Ease(curve, t / seconds);
        worst = std::max(worst, std::abs(Evaluate(segments, seconds, to, t) - exact) / std::abs(to - from));
    }
    return worst;
}
}

int main() {
    Check("ease-out-endpoints", Ease(EaseOut, 0) == 0 && Ease(EaseOut, 1) == 1);
    Check("ease-out-midpoint", std::abs(Ease(EaseOut, .5) - 0.684643) < 1e-4);
    Check("photon-fast-start", Ease(Photon, .1) > 0.6);
    Check("ease-monotonic", [] {
        double last = 0;
        for (int i = 1; i <= 100; ++i) {
            const double v = Ease(Photon, i / 100.0);
            if (v + 1e-9 < last) return false;
            last = v;
        }
        return true;
    }());
    Check("ease-out-fit-under-half-percent", FitError(EaseOut, 0, 160, .1) < 0.005);
    Check("photon-fit-under-half-percent", FitError(Photon, 0, 160, .2) < 0.005);
    Check("photon-fit-reverse", FitError(Photon, 240, -80, .2) < 0.005);
    Check("settle-fit", FitError(EaseOut, 76, 140, .15) < 0.005);
    const auto segments = Segments(EaseOut, 10, 30, .1);
    Check("segments-count", segments.size() == 20 && segments.front().begin == 0);
    Check("segments-ordered", [&] {
        for (size_t i = 1; i < segments.size(); ++i)
            if (segments[i].begin <= segments[i - 1].begin) return false;
        return true;
    }());
    Check("evaluate-end-value", Evaluate(segments, .1, 30, .1) == 30 && Evaluate(segments, .1, 30, 5) == 30);
    Check("evaluate-start-value", std::abs(Evaluate(segments, .1, 30, 0) - 10) < 1e-9);
    Check("segments-zero-duration", Segments(EaseOut, 0, 1, 0).empty());

    Transition move{0, 100, 1000, 100, EaseOut};
    Check("transition-before-start", move.At(900) == 0);
    Check("transition-after-end", move.At(1100) == 100 && !move.Running(1100));
    Check("transition-midway", move.At(1050) > 50 && move.At(1050) < 80 && move.Running(1050));
    Transition instant{0, 42, 1000, 0, EaseOut};
    Check("transition-instant", instant.At(1000) == 42 && !instant.Running(1000));

    const std::vector<std::uint64_t> abc{1, 2, 3}, acb{1, 3, 2}, ab{1, 2}, abd{1, 2, 4};
    Check("classify-none", Classify(abc, abc, 100, 100) == Change::None);
    Check("classify-geometry", Classify(abc, abc, 100, 120) == Change::Geometry);
    Check("classify-order", Classify(abc, acb, 100, 100) == Change::Order);
    Check("classify-close", Classify(abc, ab, 100, 100) == Change::Membership);
    Check("classify-replace", Classify(abc, abd, 100, 100) == Change::Membership);
    Check("timing-open", TimingFor(Change::Membership).milliseconds == OpenMs);
    Check("timing-reorder", TimingFor(Change::Order).milliseconds == ReorderMs && TimingFor(Change::Order).curve.y1 == Photon.y1);
    Check("timing-settle", TimingFor(Change::Geometry).milliseconds == SettleMs);
    std::printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
