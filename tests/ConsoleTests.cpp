#include "core/ConsoleRegistry.h"
#include "core/EngineSettings.h"
#include "ui/Console.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace whimsical;
void check(bool value, const char* why) {
    if (!value)
        throw std::runtime_error(why);
}
void fuzzySearchTests() {
    ConsoleRegistry registry;
    registry.variable("r.xxx.xx.animaxxx", 0, "Nested animation setting");
    registry.variable("r.anim", 0, "Animation");
    registry.variable("r.animation", 0, "Animation");
    registry.variable("r.x.anim", 0, "Animation");
    registry.variable("r.other", 0, "Controls animation playback rate");
    registry.variable("anim.r.reverse", 0, "Reversed fragments");
    registry.command("r.animReload", "Reload animation", [](const auto&) { return "reloaded"; });
    const std::vector<std::string> expected{"r.anim", "r.animation", "r.animReload", "r.x.anim",
                                            "r.xxx.xx.animaxxx"};
    check(registry.complete("  R.  \tANIM  ") == expected,
          "Ordered fragments cross hierarchy levels, ignore case/whitespace, and rank tight names first");
    check(registry.complete("xxx anim") == std::vector<std::string>{"r.xxx.xx.animaxxx"},
          "Fragments can start anywhere inside the name");
    check(registry.complete("r.xxx missing").empty() && registry.complete("r.xxx r.xxx").empty(),
          "Every fragment must match in order without reusing characters");
    check(registry.complete(" \t ") == registry.complete(""), "Empty query lists all entries");
    auto result = registry.execute("find r.  anim");
    check(result.ok && result.text.find("r.xxx.xx.animaxxx") != std::string::npos &&
              result.text.find("r.other") != std::string::npos &&
              result.text.find("r.other") > result.text.find("r.xxx.xx.animaxxx") &&
              result.text == registry.execute("find \"r.  anim\"").text,
          "Find shares ordered matching, supports quoted/unquoted queries, and ranks names before help");
    check(registry.execute("find playback rate").text.find("r.other") != std::string::npos &&
              registry.execute("find impossible fragment").text == "No matches.",
          "Multiword help search and no-match output");
    check(!registry.execute("r. anim 2").ok && registry.get<int>("r.anim") == 0,
          "Fuzzy queries never execute a guessed name");

    ui::Console console(registry);
    console.open(true);
    Input input;
    input.text = U"r.  anim";
    console.handle(input);
    check(console.view().suggestions == expected, "Live suggestions include multi-fragment matches");
    input.text = U"\t";
    console.handle(input);
    check(console.view().input == expected[0] && console.view().suggestions == expected,
          "Tab uses the displayed fuzzy candidates and preserves the cycle");
    for (size_t i = 1; i <= expected.size(); ++i) {
        input.text = U"\t";
        console.handle(input);
        check(console.view().input == expected[i % expected.size()], "Tab cycles the original fuzzy query");
    }
    for (const auto& line : {U"r.anim 2", U"R.ANIM ", U"r.anim=2", U"find r. anim", U"help r.anim",
                             U"r.other \"hello world\""}) {
        input.text = std::u32string(1, 21) + line;
        console.handle(input);
        const auto before = console.view().input;
        input.text = U"\t";
        console.handle(input);
        check(console.view().input == before && console.view().suggestions.empty(),
              "Tab must preserve command arguments and quoted values");
    }
    input.text = U"\x15r.xxx anim\t 3\r";
    console.handle(input);
    check(registry.get<int>("r.xxx.xx.animaxxx") == 3,
          "Editing clears old candidates; fuzzy completion then assignment executes the selected name");
}
void candidateNavigationTests() {
    ConsoleRegistry registry;
    int executions = 0;
    for (int i = 0; i < 7; ++i)
        registry.command("r.item" + std::to_string(i), "Candidate", [&](const auto&) {
            ++executions;
            return "executed";
        });
    ui::Console console(registry);
    console.open(true);
    Input input;
    auto type = [&](const std::u32string& text) {
        input.text = text;
        console.handle(input);
    };
    auto arrow = [&](unsigned key) {
        input.pressed[key] = true;
        console.handle(input);
        check(!input.pressed[key], "Candidate navigation consumes gameplay arrows");
    };
    type(U"r.item0\r");
    arrow(0x26);
    check(console.view().input == "r.item0", "Empty input recalls history");
    arrow(0x28);
    check(console.view().input.empty(), "History navigation ignores candidates of recalled names");
    type(U"r. item");
    check(console.view().selectedSuggestion == -1, "Typing shows unselected predictions");
    for (int i = 0; i < 7; ++i) {
        arrow(0x28);
        const auto view = console.view();
        check(view.input == "r.item" + std::to_string(i) && view.selectedSuggestion >= 0 &&
                  view.suggestions.at(view.selectedSuggestion) == view.input && view.suggestions.size() <= 5,
              "Down fills candidates in rank order and scrolls the visible highlight");
    }
    arrow(0x28);
    check(console.view().input == "r.item0", "Down wraps to first candidate");
    arrow(0x26);
    check(console.view().input == "r.item6" && executions == 1,
          "Up wraps backward; selecting a command never executes it");
    type(U"\r");
    check(executions == 2 && console.view().input.empty() && console.view().selectedSuggestion == -1,
          "Enter executes the selected command and clears selection");
    type(U"r. item");
    arrow(0x26);
    check(console.view().input == "r.item6", "Initial Up selects last candidate");
    type(U"\x15r. item0");
    arrow(0x28);
    check(console.view().input == "r.item0", "Editing rebuilds arrow candidates");
    type(U"\x15r. item");
    arrow(0x28);
    type(U"\t");
    check(console.view().input == "r.item1", "Tab and arrows share selection state");
    arrow(0x26);
    check(console.view().input == "r.item0", "Arrows continue from Tab selection");
}
int main() {
    try {
        fuzzySearchTests();
        candidateNavigationTests();
        ConsoleRegistry vars;
        auto& n = vars.variable("test.Count", 2, "A bounded count", CVarArchive, CVarRange{0, 10});
        vars.variable("test.Name", std::string("default"), "Quoted text", CVarArchive);
        vars.variable("test.Flag", true, "Boolean", CVarArchive);
        vars.variable("test.Float", 1.5, "Float", CVarArchive, CVarRange{0, 8});
        vars.variable("test.Fixed", 42, "Read only", CVarReadOnly);
        vars.variable("test.Mode", std::string("a"), "Startup setting", CVarArchive | CVarRestart, {},
                      {"a", "b"});
        int changes = 0;
        n.changed = [&](const ConsoleValue& value) { changes += std::get<int>(value); };
        check(vars.execute("TEST.COUNT=4", CVarSource::Config).ok && changes == 4,
              "Case insensitive set and callback");
        check(!vars.execute("test.Count 4.1").ok && !vars.execute("test.Count 11").ok &&
                  vars.get<int>("test.Count") == 4,
              "Invalid numeric writes must be atomic");
        check(!vars.execute("test.Count 999999999999999").ok && !vars.execute("test.Float nan").ok &&
                  !vars.execute("test.Float 1abc").ok,
              "Reject overflow/NaN/trailing garbage");
        check(!vars.execute("test.Fixed 3").ok && !vars.execute("test.Mode invalid").ok,
              "Read-only and enum constraints");
        check(vars.execute("test.Count 5", CVarSource::CommandLine).ok, "Command line override");
        check(!vars.execute("test.Count 3", CVarSource::Config).ok && changes == 9,
              "Lower priority must not fire callback");
        check(vars.execute("test.Mode b", CVarSource::Config).ok && vars.get<std::string>("test.Mode") == "b",
              "Startup apply");
        vars.finishStartup();
        check(vars.execute("reset test.Count", CVarSource::CommandLine).ok &&
                  vars.at("test.Count").source == CVarSource::CommandLine,
              "Built-in commands must retain their invocation source");
        check(vars.execute("test.Mode a").ok && vars.get<std::string>("test.Mode") == "b" &&
                  std::get<std::string>(vars.at("test.Mode").value) == "a",
              "Restart setting must expose pending and active values separately");
        check(vars.execute("test.Name \"hello 世界 \\\"quote\\\" \\\\ path\\nsecond\"").ok,
              "UTF-8 and quoted escapes");
        const auto name = vars.get<std::string>("test.Name");
        check(!vars.execute("test.Name \"unterminated").ok && vars.get<std::string>("test.Name") == name,
              "Malformed command must not change value");
        check(vars.execute("test.Flag off").ok && !vars.get<bool>("test.Flag"), "Boolean spellings");
        check(vars.execute("reset test.Count").ok && vars.get<int>("test.Count") == 2,
              "Reset restores default");
        check(vars.execute("find bounded").text.find("test.Count") != std::string::npos,
              "Help substring search");
        check(vars.complete("TEST.").size() == 6 && !vars.execute("missing").ok,
              "Completion and unknown entries");
        bool duplicate = false;
        try {
            vars.command("TEST.COUNT", "duplicate", [](const auto&) { return ""; });
        } catch (const std::exception&) {
            duplicate = true;
        }
        check(duplicate, "Commands and variables share one name namespace");
        std::ostringstream saved;
        vars.save(saved);
        ConsoleRegistry restored;
        restored.variable("test.Count", 0, "", CVarArchive);
        restored.variable("test.Name", std::string(""), "", CVarArchive);
        restored.variable("test.Flag", true, "", CVarArchive);
        restored.variable("test.Float", 0.0, "", CVarArchive);
        restored.variable("test.Mode", std::string("b"), "", CVarArchive | CVarRestart);
        std::istringstream input(saved.str());
        for (const auto& r : restored.load(input))
            check(r.ok, "Config roundtrip");
        check(restored.get<std::string>("test.Name") == name &&
                  saved.str().find("test.Fixed") == std::string::npos,
              "Persist escaped strings; exclude read-only values");
        std::istringstream bad("test.Count=bad\nquit ignored\ntest.Count=3\n");
        const auto results = restored.load(bad);
        check(results.size() == 3 && !results[0].ok && !results[1].ok && results[2].ok,
              "Config errors must identify lines and never dispatch commands");

        ConsoleRegistry engine;
        EngineSettings settings(engine, false);
        ui::Console console(engine);
        Input keys;
        keys.pressed[0xC0] = true;
        keys.keys['W'] = true;
        keys.leftPressed = true;
        check(console.handle(keys) && console.isOpen() && !keys.keys['W'] && !keys.leftPressed,
              "Opening console captures keyboard and mouse");
        keys.text = U"r.Expo\t";
        console.handle(keys);
        check(console.view().input == "r.Exposure", "Tab completes registered names");
        keys.text = U" 2\r";
        console.handle(keys);
        check(engine.get<double>("r.Exposure") == 2 && console.view().input.empty(),
              "Console submits actual registered command");
        keys.text = U"draft";
        console.handle(keys);
        keys.pressed[0x26] = true;
        console.handle(keys);
        check(console.view().input == "r.Exposure 2", "Up recalls history");
        keys.pressed[0x28] = true;
        console.handle(keys);
        check(console.view().input == "draft", "Down restores unfinished draft");
        keys.text = U"\x15世界";
        console.handle(keys);
        keys.pressed[0x25] = true;
        console.handle(keys);
        keys.text = U"!";
        console.handle(keys);
        check(console.view().input == "世!界", "Cursor edits Unicode codepoints");
        keys.text = U"\x15r.Hud false\r";
        console.handle(keys);
        Frame old;
        settings.decorate(old);
        old.console = console.view();
        engine.execute("r.Exposure 3");
        Frame newer;
        settings.decorate(newer);
        check(old.exposure == 2 && newer.exposure == 3 && !old.hudEnabled && old.console.open,
              "Frame values remain immutable; console independent of HUD");
        keys.pressed[0x1B] = true;
        keys.keys['W'] = true;
        check(console.handle(keys) && !console.isOpen() && !keys.pressed[0x1B] && !keys.keys['W'],
              "Closing tick must not leak game inputs");
        for (int i = 0; i < 400; ++i)
            console.log("line " + std::to_string(i));
        console.open(true);
        check(console.view().lines.size() <= 24 && console.view().lines.back().text == "line 399",
              "Bounded visible scrollback");
        std::cout << "Console registry, persistence, ownership and input tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
