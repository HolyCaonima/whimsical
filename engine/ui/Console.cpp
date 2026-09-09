#include "Console.h"
#include <algorithm>
#include <sstream>
#include <iostream>
#include <stdexcept>

namespace whimsical::ui {
namespace {
std::string utf8(const std::u32string& s) {
    // Window supplies Unicode scalar values, so editing never splits a UTF-8 sequence.
    std::string result;
    for (char32_t c : s) {
        if (c <= 0x7F)
            result += char(c);
        else {
            if (c <= 0x7FF)
                result += char(0xC0 | (c >> 6));
            else {
                if (c <= 0xFFFF)
                    result += char(0xE0 | (c >> 12));
                else {
                    result += char(0xF0 | (c >> 18));
                    result += char(0x80 | ((c >> 12) & 63));
                }
                result += char(0x80 | ((c >> 6) & 63));
            }
            result += char(0x80 | (c & 63));
        }
    }
    return result;
}
} // namespace
Console::Console(ConsoleRegistry& registry) : registry_(registry) {
    registry_.command("clear", "Clear console output", [this](const auto& args) {
        if (!args.empty())
            throw std::runtime_error("Usage: clear");
        lines_.clear();
        scroll_ = 0;
        return "Console cleared.";
    });
    log("WHIMSICAL CONSOLE | ~ / F10 open | Esc close | Up/Down select/history | Enter execute | Tab "
        "complete");
    log("Type help or find render. Values persist only after cvar.save.");
}
void Console::log(std::string text, bool error) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        // Bound individual lines as well as the scrollback, including external script output.
        if (line.size() > 4096)
            line.resize(4096);
        lines_.push_back({std::move(line), error});
        if (lines_.size() > 256)
            lines_.pop_front();
    }
    scroll_ = std::min(scroll_, lines_.size());
}
ConsoleResult Console::execute(const std::string& line, CVarSource source) {
    log("> " + line);
    auto result = registry_.execute(line, source);
    std::cout << "[Console] " << line << "\n" << (result.ok ? "" : "Error: ") << result.text << "\n";
    if (!result.text.empty())
        log(result.text, !result.ok);
    scroll_ = 0;
    return result;
}
void Console::submit() {
    if (input_.empty())
        return;
    if (history_.empty() || history_.back() != input_)
        history_.push_back(input_);
    if (history_.size() > 64)
        history_.erase(history_.begin());
    historyIndex_ = history_.size();
    execute(utf8(input_));
    input_.clear();
    draft_.clear();
    cursor_ = 0;
    completion_.clear();
}
std::vector<std::string> Console::suggestions() const {
    const auto query = utf8(input_);
    if (query.find_first_of("=\"") != std::string::npos)
        return {};
    const auto start = query.find_first_not_of(" \t");
    if (start != std::string::npos) {
        const auto end = query.find_first_of(" \t", start);
        // A complete identifier followed by whitespace starts argument editing.
        if (end != std::string::npos && registry_.contains(query.substr(start, end - start)))
            return {};
    }
    return registry_.complete(query);
}
bool Console::complete(int direction) {
    if (completion_.empty()) {
        completion_ = suggestions();
        completionIndex_ = direction > 0 || completion_.empty() ? 0 : completion_.size() - 1;
    } else
        completionIndex_ = direction > 0 ? (completionIndex_ + 1) % completion_.size()
                                         : (completionIndex_ + completion_.size() - 1) % completion_.size();
    if (completion_.empty())
        return false;
    const auto& name = completion_[completionIndex_];
    input_.assign(name.begin(), name.end()); // Registered identifiers are ASCII.
    cursor_ = input_.size();
    historyIndex_ = history_.size();
    return true;
}
bool Console::handle(Input& in) {
    // Virtual-key values stay at the input boundary; this controller does not depend on Win32.
    if (in.pressed[0xC0] || in.pressed[0x79]) {
        open_ = !open_;
        completion_.clear();
    } else if (!open_)
        return false;
    else if (in.pressed[0x1B])
        open_ = false;
    else {
        const bool arrow = in.pressed[0x26] || in.pressed[0x28];
        const bool selected = arrow && historyIndex_ == history_.size() &&
                              input_.find_first_not_of(U" \t") != std::u32string::npos &&
                              complete(in.pressed[0x28] ? 1 : -1);
        if (!selected && in.pressed[0x26] && historyIndex_ > 0) {
            if (historyIndex_ == history_.size())
                draft_ = input_;
            input_ = history_[--historyIndex_];
            cursor_ = input_.size();
            completion_.clear();
        }
        if (!selected && in.pressed[0x28] && historyIndex_ < history_.size()) {
            ++historyIndex_;
            input_ = historyIndex_ == history_.size() ? draft_ : history_[historyIndex_];
            cursor_ = input_.size();
            completion_.clear();
        }
        if (in.pressed[0x25] && cursor_)
            --cursor_;
        if (in.pressed[0x27] && cursor_ < input_.size())
            ++cursor_;
        if (in.pressed[0x24])
            cursor_ = 0;
        if (in.pressed[0x23])
            cursor_ = input_.size();
        if (in.pressed[0x2E] && cursor_ < input_.size()) {
            input_.erase(cursor_, 1);
            completion_.clear();
            historyIndex_ = history_.size();
        }
        if (in.pressed[0x21])
            scroll_ = std::min(lines_.size(), scroll_ + 8);
        if (in.pressed[0x22])
            scroll_ = scroll_ > 8 ? scroll_ - 8 : 0;
        if (in.wheel > 0)
            scroll_ = std::min(lines_.size(), scroll_ + 3);
        if (in.wheel < 0)
            scroll_ = scroll_ > 3 ? scroll_ - 3 : 0;
        for (char32_t c : in.text) {
            if (c == U'\t') {
                complete();
                continue;
            }
            completion_.clear();
            historyIndex_ = history_.size();
            if (c == U'\r')
                submit();
            else if (c == U'\b') {
                if (cursor_)
                    input_.erase(--cursor_, 1);
            } else if (c == 21) {
                input_.clear();
                cursor_ = 0;
            } // Ctrl+U
            else if (c >= 32 && c != 127 && input_.size() < 1024)
                input_.insert(cursor_++, 1, c);
        }
    }
    in.keys.fill(false);
    in.uiEvents.clear();
    in.pointerCaptured = in.keyboardCaptured = true;
    in.pressed.fill(false);
    in.text.clear();
    in.left = in.right = in.middle = in.leftPressed = in.rightPressed = false;
    in.deltaX = in.deltaY = in.wheel = 0;
    return true;
}
ConsoleView Console::view() const {
    ConsoleView v;
    v.open = open_;
    if (!open_)
        return v;
    v.input = utf8(input_);
    v.beforeCursor = utf8(input_.substr(0, cursor_));
    v.scroll = unsigned(scroll_);
    const auto end = lines_.size() - std::min(scroll_, lines_.size());
    const auto start = end > 24 ? end - 24 : 0;
    v.lines.assign(lines_.begin() + start, lines_.begin() + end);
    if (!input_.empty()) {
        v.suggestions = completion_.empty() ? suggestions() : completion_;
        if (!completion_.empty()) {
            const auto first = completionIndex_ >= 5 ? completionIndex_ - 4 : 0;
            v.suggestions.erase(v.suggestions.begin(), v.suggestions.begin() + first);
            v.selectedSuggestion = int(completionIndex_ - first);
        }
        if (v.suggestions.size() > 5)
            v.suggestions.resize(5);
    }
    return v;
}
} // namespace whimsical::ui
