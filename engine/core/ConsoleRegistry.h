#pragma once
#include <functional>
#include <istream>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

namespace afterlight {
using ConsoleValue = std::variant<bool, int, double, std::string>;
enum class CVarSource { Default, Config, CommandLine, Console };
enum CVarFlags : unsigned { CVarNone = 0, CVarArchive = 1, CVarReadOnly = 2, CVarRestart = 4 };
struct ConsoleResult {
    bool ok;
    std::string text;
};
struct CVarRange {
    double minimum, maximum;
};
struct CVar {
    std::string name, help;
    ConsoleValue value, active, defaults;
    unsigned flags = 0;
    CVarSource source = CVarSource::Default;
    std::optional<CVarRange> range;
    std::vector<std::string> choices;
    std::function<void(const ConsoleValue&)> changed;
};
// Registration, mutation and command callbacks belong to the owning engine thread.
// Render consumers receive concrete values in Frame, never pointers into this registry.
class ConsoleRegistry {
  public:
    using Command = std::function<std::string(const std::vector<std::string>&)>;
    using ContextCommand = std::function<std::string(const std::vector<std::string>&, CVarSource)>;
    ConsoleRegistry();
    ConsoleRegistry(const ConsoleRegistry&) = delete;
    ConsoleRegistry& operator=(const ConsoleRegistry&) = delete;
    CVar& variable(std::string name, ConsoleValue initial, std::string help, unsigned flags = CVarNone,
                   std::optional<CVarRange> range = {}, std::vector<std::string> choices = {});
    void command(std::string name, std::string help, Command callback);
    void command(std::string name, std::string help, ContextCommand callback);
    const CVar& at(const std::string& name) const;
    template <class T> T get(const std::string& name) const {
        return std::get<T>(at(name).active);
    }
    ConsoleResult set(const std::string& name, const std::string& value, CVarSource source);
    ConsoleResult execute(const std::string& line, CVarSource source = CVarSource::Console);
    bool contains(const std::string& name) const;
    // Case-insensitive, ordered whitespace-separated fragments; ranked by match quality.
    std::vector<std::string> complete(const std::string& query) const;
    std::vector<ConsoleResult> load(std::istream& input);
    void save(std::ostream& output) const;
    void finishStartup() {
        started_ = true;
    }
    std::string describe(const std::string& name) const;
    static std::string format(const ConsoleValue&);
    static std::vector<std::string> tokenize(const std::string&);

  private:
    struct Entry {
        std::string name, help;
        ContextCommand callback;
    };
    std::map<std::string, CVar> variables_;
    std::map<std::string, Entry> commands_;
    std::vector<std::string> search(const std::string& query, bool includeHelp) const;
    bool started_ = false;
};
} // namespace afterlight
