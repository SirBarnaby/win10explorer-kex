#pragma once
// Pure tab-switching policy for one Explorer frame. A Windows 10 frame owns a
// single Shell view, so a switch is a navigation that completes later. This
// model lets the strip show the requested tab at once, collapses rapid
// requests to the last one, and holds other commands until the view settles.
// No Win32 or COM types: unit-tested by TabModelTests.cpp.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace tabs {
enum class CommandKind { NewTab, Duplicate, Close, Reopen, Back, Forward };
struct Command {
    CommandKind kind{CommandKind::NewTab};
    std::uint64_t tab{}; // Duplicate/Close: the tab identity at request time.
    bool operator==(const Command&) const = default;
};
enum class RequestAction { None, Navigate, Wait };

class SwitchModel {
    std::uint64_t requested_{};
    std::deque<Command> queue_;

public:
    static constexpr std::size_t MaxQueue = 8;
    static constexpr unsigned long long BusyDelayMs = 300;

    // The user asked for `id`. `committed` is the tab the Shell view shows,
    // `navigating` the target of the navigation in flight (0 when idle).
    RequestAction Request(std::uint64_t id, std::uint64_t committed, std::uint64_t navigating) {
        if (!id) return RequestAction::None;
        if (!navigating) {
            requested_ = id == committed ? 0 : id;
            return requested_ ? RequestAction::Navigate : RequestAction::None;
        }
        // While a navigation runs, the last request wins. Asking for the tab
        // already in flight cancels an intermediate retarget; that tab stays
        // the displayed one and needs no follow-up on arrival.
        requested_ = id;
        return RequestAction::Wait;
    }
    // The in-flight navigation arrived at `arrived`. Returns the tab to
    // navigate to next when a later request superseded it, otherwise 0.
    std::uint64_t Arrived(std::uint64_t arrived) {
        const auto next = requested_ && requested_ != arrived ? requested_ : 0;
        requested_ = 0;
        return next;
    }
    // Navigation error, timeout, disable or teardown: nothing replays.
    void Abandon() { requested_ = 0; queue_.clear(); }
    // A tab left the model; it can no longer be the requested selection.
    void Forget(std::uint64_t id) { if (requested_ == id) requested_ = 0; }
    bool Enqueue(const Command& command) {
        if (queue_.size() >= MaxQueue) return false;
        queue_.push_back(command);
        return true;
    }
    std::optional<Command> Next() {
        if (queue_.empty()) return std::nullopt;
        auto command = queue_.front();
        queue_.pop_front();
        return command;
    }
    std::uint64_t Requested() const { return requested_; }
    std::uint64_t Displayed(std::uint64_t committed) const { return requested_ ? requested_ : committed; }
    std::size_t Queued() const { return queue_.size(); }
    static bool Busy(bool navigating, unsigned long long elapsedMs) { return navigating && elapsedMs >= BusyDelayMs; }
};
} // namespace tabs
