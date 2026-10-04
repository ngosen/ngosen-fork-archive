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
    if (context->forwarded().size() != 2) {
        reportFailure("forward the backspace for e -> ê", "[BackSpace down][BackSpace up]", describeForwarded(*context));
        return 1;
    }

    // 10 ms later the client hands the backspace back: it must pass through, not be swallowed.
    pumpEventLoop(testInstance.instance, 10);
    if (!type(engine, entry, *context, FcitxKey_BackSpace, false))
        return 1;
    if (!context->commits().empty()) {
        reportFailure("no commit when the backspace comes back", "commits=(none)", "commits=" + joinCommits(*context));
        return 1;
    }
    // The original wait (15 ms from the forward) has passed; the commit now waits for the handed-back key.
    pumpEventLoop(testInstance.instance, 8);
    if (!context->commits().empty()) {
        reportFailure("no commit within 15 ms after the handed-back backspace", "commits=(none)", "commits=" + joinCommits(*context));
        return 1;
    }
    pumpEventLoop(testInstance.instance, 30);
    if (context->commits() != std::vector<std::string>{"ê"}) {
        reportFailure("commit after the handed-back backspace", "commits=['ê']", "commits=" + joinCommits(*context));
        return 1;
    }
    return 0;
}
