#include "TabModel.h"
#include <iostream>

namespace {
unsigned failures{}, checks{};
void Check(const char* name, bool passed) {
    ++checks;
    if (!passed) ++failures;
    std::cout << (passed ? "PASS " : "FAIL ") << name << '\n';
}
}

int main() {
    using tabs::RequestAction;
    {
        tabs::SwitchModel model;
        Check("idle request for another tab navigates", model.Request(2, 1, 0) == RequestAction::Navigate);
        Check("requested tab is displayed before the view arrives", model.Displayed(1) == 2);
        Check("arrival without a newer request needs no follow-up", model.Arrived(2) == 0);
        Check("after arrival the committed tab is displayed", model.Displayed(2) == 2 && model.Requested() == 0);
        Check("request for the committed tab does nothing", model.Request(2, 2, 0) == RequestAction::None);
        Check("zero identity is ignored", model.Request(0, 2, 0) == RequestAction::None);
    }
    {
        // AE1: three quick switches collapse to the last one.
        tabs::SwitchModel model;
        model.Request(2, 1, 0);
        Check("request during navigation waits", model.Request(3, 1, 2) == RequestAction::Wait);
        Check("later request during navigation waits", model.Request(4, 1, 2) == RequestAction::Wait);
        Check("display follows the newest request immediately", model.Displayed(1) == 4);
        Check("arrival of a superseded tab returns the newest request", model.Arrived(2) == 4);
        Check("superseded follow-up keeps no stale request", model.Requested() == 0);
        Check("final arrival settles", model.Arrived(4) == 0 && model.Displayed(4) == 4);
    }
    {
        tabs::SwitchModel model;
        model.Request(2, 1, 0);
        model.Request(3, 1, 2);
        Check("asking again for the tab in flight cancels the retarget", model.Request(2, 1, 2) == RequestAction::Wait);
        Check("the tab in flight stays displayed after a cancelled retarget", model.Displayed(1) == 2);
        Check("cancelled retarget needs no follow-up", model.Arrived(2) == 0 && model.Requested() == 0);
    }
    {
        tabs::SwitchModel model;
        model.Request(2, 1, 0);
        model.Request(3, 1, 2);
        model.Enqueue({tabs::CommandKind::NewTab, 0});
        model.Abandon();
        Check("navigation error drops request and queue", model.Requested() == 0 && model.Queued() == 0 &&
                                                              model.Displayed(1) == 1);
    }
    {
        tabs::SwitchModel model;
        bool accepted = true;
        for (std::size_t i = 0; i < tabs::SwitchModel::MaxQueue; ++i)
            accepted = model.Enqueue({tabs::CommandKind::Close, i + 1}) && accepted;
        Check("queue accepts its capacity", accepted && model.Queued() == tabs::SwitchModel::MaxQueue);
        Check("queue overflow is rejected, not silently replacing", !model.Enqueue({tabs::CommandKind::Reopen, 0}));
        auto first = model.Next();
        Check("queued commands replay in order", first && first->kind == tabs::CommandKind::Close && first->tab == 1);
        std::size_t remaining = 0;
        while (model.Next()) ++remaining;
        Check("queue drains completely", remaining == tabs::SwitchModel::MaxQueue - 1 && !model.Next());
    }
    {
        tabs::SwitchModel model;
        model.Request(2, 1, 0);
        model.Request(3, 1, 2);
        model.Forget(3);
        Check("closing the requested tab forgets it", model.Requested() == 0 && model.Arrived(2) == 0);
        Check("a new tab opened while idle becomes the displayed tab", model.Request(9, 2, 0) == RequestAction::Navigate &&
                                                                      model.Displayed(2) == 9);
    }
    Check("busy indicator waits 300 ms", !tabs::SwitchModel::Busy(true, 299) && tabs::SwitchModel::Busy(true, 300));
    Check("busy indicator needs a navigation", !tabs::SwitchModel::Busy(false, 5000));
    std::cout << checks - failures << '/' << checks << " checks passed\n";
    return failures ? 1 : 0;
}
