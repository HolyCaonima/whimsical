#pragma once
#include "core/ConsoleRegistry.h"
#include "core/Types.h"
#include <deque>

namespace afterlight::ui {
// Text editing and input ownership live on the main thread, independent of the HUD painter.
class Console {
  public:
    explicit Console(ConsoleRegistry& registry);
    bool handle(Input& input); // Consumes game input even on the closing tick.
    bool isOpen() const {
        return open_;
    }
    void open(bool value) {
        open_ = value;
    }
    void log(std::string text, bool error = false);
    ConsoleResult execute(const std::string&, CVarSource source = CVarSource::Console);
    ConsoleView view() const;

  private:
    ConsoleRegistry& registry_;
    bool open_ = false;
    std::u32string input_, draft_;
    size_t cursor_ = 0, historyIndex_ = 0, scroll_ = 0, completionIndex_ = 0;
    std::vector<std::u32string> history_;
    std::deque<ConsoleLine> lines_;
    std::vector<std::string> completion_;
    void submit();
    bool complete(int direction = 1);
    std::vector<std::string> suggestions() const;
};
} // namespace afterlight::ui
