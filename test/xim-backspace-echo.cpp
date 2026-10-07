// SPDX-License-Identifier: GPL-3.0-or-later
//
// An XIM client sends back the release of each forwarded backspace once it has applied it. A slow
// client (VS Code) takes longer than the fixed wait for several backspaces, so the commit must wait
// for the last release instead, or it lands before the deletion and loses a letter.
#include "lotus-engine.h"
#include "lotus-utils.h"
#include "test-input-context.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

    void reportFailure(const std::string& step, const std::string& expected, const std::string& actual) {
        std::cerr << "Step: " << step << "\nExpected: " << expected << "\nActual: " << actual << '\n';
    }

    std::string joinCommits(const TestInputContext& context) {
        std::string out;
        for (const auto& commit : context.commits())
            out += "['" + commit + "']";
        return out.empty() ? "(none)" : out;
    }

    uint64_t nowUs() {
        return fcitx::now(CLOCK_MONOTONIC);
    }

    // libuv timers count whole milliseconds from the loop's cached time.
    constexpr uint64_t ClockSlackUs = 3000;
    // The first release comes back inside the fixed wait, the second after it.
    constexpr uint64_t FirstEchoUs  = 8000;
    constexpr uint64_t SecondEchoUs = 22000;

    enum class Outcome {
        Pass,
        Fail,
        Stalled
    };

    struct Harness {
        fcitx::Instance&               instance;
        fcitx::LotusEngine&            engine;
        const fcitx::InputMethodEntry& entry;
        TestInputContext&              context;

        bool                           key(fcitx::KeySym symbol, bool release, bool accepted) {
            fcitx::KeyEvent event(&context, fcitx::Key(symbol), release);
            engine.keyEvent(entry, event);
            if (event.accepted() != accepted) {
                reportFailure("process key " + std::to_string(symbol) + (release ? " up" : " down"), "accepted=" + std::to_string(accepted),
                              "accepted=" + std::to_string(event.accepted()));
                return false;
            }
            return true;
        }
        template <typename Done>
        uint64_t pumpUntil(Done done, uint64_t deadlineUs) {
            while (!done() && nowUs() < deadlineUs)
                pumpEventLoop(instance, 1);
            return nowUs();
        }
    };

    // Types "an" + "s" (2 backspaces) and sends the releases back at FirstEchoUs and SecondEchoUs.
    Outcome echoOnce(Harness& h, bool firstWord) {
        if (!firstWord && !h.key(FcitxKey_space, false, true))
            return Outcome::Fail;
        if (!h.key(FcitxKey_a, false, false) || !h.key(FcitxKey_n, false, false))
            return Outcome::Fail;
        const size_t commitsBefore   = h.context.commits().size();
        const size_t forwardedBefore = h.context.forwarded().size();
        const auto   committed       = [&] { return h.context.commits().size() > commitsBefore; };
        // libuv dates a new timer from the loop's last run, and a real key arrives inside one.
        pumpEventLoop(h.instance, 1);
        const uint64_t replacedAt = nowUs();
        if (!h.key(FcitxKey_s, false, true))
            return Outcome::Fail;
        h.pumpUntil([&] { return h.context.forwarded().size() >= forwardedBefore + 4; }, replacedAt + 500000);
        if (h.context.forwarded().size() != forwardedBefore + 4) {
            reportFailure("forward 2 backspaces for an -> án", "4 forwarded events", std::to_string(h.context.forwarded().size() - forwardedBefore));
            return Outcome::Fail;
        }

        h.pumpUntil(committed, replacedAt + FirstEchoUs);
        if (committed())
            return Outcome::Stalled;
        if (!h.key(FcitxKey_BackSpace, true, false))
            return Outcome::Fail;

        const uint64_t stoppedAt = h.pumpUntil(committed, replacedAt + SecondEchoUs);
        if (committed()) {
            if (stoppedAt - replacedAt + ClockSlackUs >= SecondEchoUs)
                return Outcome::Stalled;
            reportFailure("no commit before the second backspace is applied", "no commit before " + std::to_string(SecondEchoUs) + " us",
                          "commit at " + std::to_string(stoppedAt - replacedAt) + " us");
            return Outcome::Fail;
        }
        const uint64_t echoedAt = nowUs();
        if (!h.key(FcitxKey_BackSpace, true, false))
            return Outcome::Fail;
        // The last release ends the wait at once.
        const uint64_t committedAt = h.pumpUntil(committed, echoedAt + 500000);
        if (!committed() || h.context.commits().back() != "án") {
            reportFailure("commit after the last release", "last commit 'án'", "commits=" + joinCommits(h.context));
            return Outcome::Fail;
        }
        if (committedAt - echoedAt > 10000) {
            reportFailure("commit soon after the last release", "< 10000 us", std::to_string(committedAt - echoedAt) + " us");
            return Outcome::Fail;
        }
        return Outcome::Pass;
    }

} // namespace

int main() {
    unsetenv("WAYLAND_DISPLAY");

    configureTestPaths("fcitx5-ngosen-xim-backspace-echo");
    TestInstance       testInstance;
    fcitx::LotusEngine engine(&testInstance.instance);
    fcitx::RawConfig   config;
    config.setValueByPath("Mode", "Sen");
    config.setValueByPath("InputMethod", "Telex");
    engine.setConfig(config);

    auto context = std::make_unique<TestInputContext>(&testInstance.instance, "test", "xim");
    context->focusIn();
    fcitx::InputMethodEntry  entry("lotus", "Lotus", "vi", "lotus");
    fcitx::InputContextEvent focus(context.get(), fcitx::EventType::InputContextFocusIn);
    engine.activate(entry, focus);

    Harness h{testInstance.instance, engine, entry, *context};
    // A machine that stalls past an echo deadline proves nothing; try another word.
    Outcome outcome = Outcome::Stalled;
    for (int attempt = 0; attempt < 5 && outcome == Outcome::Stalled; ++attempt)
        outcome = echoOnce(h, attempt == 0);
    if (outcome == Outcome::Stalled) {
        reportFailure("send both releases back before the commit", "one of 5 attempts", "machine stalled every time");
        return 1;
    }
    return outcome == Outcome::Pass ? 0 : 1;
}
