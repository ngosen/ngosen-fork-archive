// SPDX-License-Identifier: GPL-3.0-or-later
//
// An XIM client under XWayland sometimes hands a forwarded backspace back unprocessed. It must reach
// the client again and the commit must wait for it, or the new text lands before the deletion.
#include "lotus-engine.h"
#include "lotus-utils.h"
#include "test-input-context.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

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

    bool type(fcitx::LotusEngine& engine, const fcitx::InputMethodEntry& entry, TestInputContext& context, fcitx::KeySym symbol, bool accepted) {
        fcitx::KeyEvent event(&context, fcitx::Key(symbol), false);
        engine.keyEvent(entry, event);
        if (event.accepted() != accepted) {
            reportFailure("process key " + std::to_string(symbol), "accepted=" + std::to_string(accepted), "accepted=" + std::to_string(event.accepted()));
            return false;
        }
        return true;
    }

    std::string describeForwarded(const TestInputContext& context) {
        std::string out;
        for (const auto& event : context.forwarded())
            out += "[" + event.key().toString() + (event.isRelease() ? " up" : " down") + "]";
        return out.empty() ? "(none)" : out;
    }

} // namespace

int main() {
    const std::string socketNamespace = "test-" + std::to_string(getpid());
    setenv("LOTUS_SOCKET_NAMESPACE", socketNamespace.c_str(), 1);
    // XIM forwards only under XWayland.
    setenv("WAYLAND_DISPLAY", "wayland-test", 1);

    configureTestPaths("fcitx5-lotus-xim-forward-handback");
    TestInstance       testInstance;
    fcitx::LotusEngine engine(&testInstance.instance);
    fcitx::RawConfig   config;
    config.setValueByPath("Mode", "Uinput");
    config.setValueByPath("InputMethod", "Telex");
    engine.setConfig(config);

    // XIM clients declare no surrounding text, so the wait is timer-only.
    auto context = std::make_unique<TestInputContext>(&testInstance.instance, "test", "xim");
    context->focusIn();
    fcitx::InputMethodEntry  entry("lotus", "Lotus", "vi", "lotus");
    fcitx::InputContextEvent focus(context.get(), fcitx::EventType::InputContextFocusIn);
    engine.activate(entry, focus);

    for (const char key : std::string("tie")) {
        if (!type(engine, entry, *context, static_cast<fcitx::KeySym>(key), false))
            return 1;
    }
    if (!type(engine, entry, *context, FcitxKey_e, true))
        return 1;
    // The client is still waiting for the reply to that key; nothing may reach it before the reply.
    if (!context->forwarded().empty()) {
        reportFailure("no backspace forwarded inside the key event", "(none)", describeForwarded(*context));
        return 1;
    }
    // Run the loop until the forward fires; it is due before the commit, so nothing else can run first.
    for (int i = 0; i < 500 && context->forwarded().empty(); ++i)
        pumpEventLoop(testInstance.instance, 1);
    const auto& forwarded = context->forwarded();
    if (forwarded.size() != 2 || forwarded[0].key().sym() != FcitxKey_BackSpace || forwarded[0].isRelease() || forwarded[1].key().sym() != FcitxKey_BackSpace ||
        !forwarded[1].isRelease()) {
        reportFailure("forward the backspace for e -> ê after the reply", "[BackSpace down][BackSpace up]", describeForwarded(*context));
        return 1;
    }

    // The client hands the backspace back right away (measured 3-50 ms on XWayland GTK3); it must pass
    // through, not be swallowed, and push the commit at least 15 ms past it. Only lower bounds on time
    // are checked, so a slow machine can make the commit later but never fail the test.
    const uint64_t handedBackAt = fcitx::now(CLOCK_MONOTONIC);
    if (!type(engine, entry, *context, FcitxKey_BackSpace, false))
        return 1;
    if (!context->commits().empty()) {
        reportFailure("no commit when the backspace comes back", "commits=(none)", "commits=" + joinCommits(*context));
        return 1;
    }
    uint64_t committedAt = 0;
    for (int i = 0; i < 500 && context->commits().empty(); ++i) {
        pumpEventLoop(testInstance.instance, 1);
        committedAt = fcitx::now(CLOCK_MONOTONIC);
    }
    if (context->commits() != std::vector<std::string>{"ê"}) {
        reportFailure("commit after the handed-back backspace", "commits=['ê']", "commits=" + joinCommits(*context));
        return 1;
    }
    if (committedAt - handedBackAt < 15000) {
        reportFailure("commit at least 15 ms after the handed-back backspace", ">= 15000 us", std::to_string(committedAt - handedBackAt) + " us");
        return 1;
    }

    // Without a hand-back the commit still waits 15 ms, long enough for one that is on its way.
    if (!type(engine, entry, *context, FcitxKey_space, true) || !type(engine, entry, *context, FcitxKey_a, false))
        return 1;
    const size_t   commitsBefore = context->commits().size();
    const uint64_t replacedAt    = fcitx::now(CLOCK_MONOTONIC);
    if (!type(engine, entry, *context, FcitxKey_s, true))
        return 1;
    committedAt = 0;
    for (int i = 0; i < 500 && context->commits().size() == commitsBefore; ++i) {
        pumpEventLoop(testInstance.instance, 1);
        committedAt = fcitx::now(CLOCK_MONOTONIC);
    }
    if (context->commits().size() != commitsBefore + 1 || context->commits().back() != "á") {
        reportFailure("commit a -> á", "last commit 'á'", "commits=" + joinCommits(*context));
        return 1;
    }
    if (committedAt - replacedAt < 15000) {
        reportFailure("commit at least 15 ms after the forward", ">= 15000 us", std::to_string(committedAt - replacedAt) + " us");
        return 1;
    }
    return 0;
}
