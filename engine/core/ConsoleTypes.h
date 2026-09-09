#pragma once
#include <string>
#include <vector>
namespace whimsical {
struct ConsoleLine {
    std::string text;
    bool error = false;
};
struct ConsoleView {
    bool open = false;
    std::string input, beforeCursor;
    std::vector<ConsoleLine> lines;
    std::vector<std::string> suggestions;
    int selectedSuggestion = -1; // Index in the visible candidate window.
    unsigned scroll = 0;
};
} // namespace whimsical
