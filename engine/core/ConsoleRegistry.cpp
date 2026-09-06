#include "ConsoleRegistry.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace afterlight {
namespace {
std::string key(std::string name) {
    for (char& c : name)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    return name;
}
using MatchRank = std::tuple<int, size_t, size_t, size_t>;
std::optional<MatchRank> match(const std::string& text, const std::vector<std::string>& words) {
    if (words.empty())
        return MatchRank{0, 0, 0, 0};
    size_t letters = 0;
    for (const auto& word : words)
        letters += word.size();
    std::optional<MatchRank> best;
    // Try each first fragment occurrence, so a later, tighter match can outrank a loose one.
    for (size_t start = text.find(words.front()); start != std::string::npos;
         start = text.find(words.front(), start + 1)) {
        size_t end = start;
        bool found = true;
        for (const auto& word : words) {
            auto at = text.find(word, end);
            if (at == std::string::npos) {
                found = false;
                break;
            }
            end = at + word.size();
        }
        if (!found)
            continue;
        const auto gaps = end - start - letters;
        const int quality = gaps ? 3 : start ? 2 : end == text.size() ? 0 : 1;
        MatchRank rank{quality, gaps, start, text.size()};
        if (!best || rank < *best)
            best = rank;
    }
    return best;
}
std::string quote(const std::string& value) {
    std::string result = "\"";
    for (char c : value) {
        switch (c) {
        case '\\':
            result += "\\\\";
            break;
        case '"':
            result += "\\\"";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            result += c;
        }
    }
    return result + '"';
}
ConsoleValue parse(const CVar& var, const std::string& text) {
    switch (var.defaults.index()) {
    case 0: {
        auto s = key(text);
        if (s == "true" || s == "1" || s == "on")
            return true;
        if (s == "false" || s == "0" || s == "off")
            return false;
        throw std::runtime_error("Expected bool: 0/1, false/true or off/on");
    }
    case 1: {
        int n = 0;
        auto result = std::from_chars(text.data(), text.data() + text.size(), n);
        if (result.ec != std::errc() || result.ptr != text.data() + text.size())
            throw std::runtime_error("Expected a 32-bit integer");
        return n;
    }
    case 2: {
        std::istringstream in(text);
        in.imbue(std::locale::classic());
        double n;
        if (!(in >> n) || !in.eof() || !std::isfinite(n))
            throw std::runtime_error("Expected a finite number");
        return n;
    }
    default:
        return text;
    }
}
void validate(const CVar& var, const ConsoleValue& value) {
    if (var.range) {
        double n =
            std::holds_alternative<int>(value) ? double(std::get<int>(value)) : std::get<double>(value);
        if (!std::isfinite(n) || n < var.range->minimum || n > var.range->maximum)
            throw std::runtime_error("Value outside range [" + ConsoleRegistry::format(var.range->minimum) +
                                     ", " + ConsoleRegistry::format(var.range->maximum) + "]");
    }
    if (!var.choices.empty() &&
        std::find(var.choices.begin(), var.choices.end(), std::get<std::string>(value)) == var.choices.end())
        throw std::runtime_error("Value is not one of the registered choices");
}
void arguments(const std::vector<std::string>& args, size_t minimum, size_t maximum, const char* usage) {
    if (args.size() < minimum || args.size() > maximum)
        throw std::runtime_error(usage);
}
} // namespace

ConsoleRegistry::ConsoleRegistry() {
    command("help", "help [name]: list entries or show detailed help", [this](const auto& a) {
        arguments(a, 0, 1, "Usage: help [name]");
        if (!a.empty())
            return describe(a[0]);
        std::string out = "CVar: name [value] or name=value. Tab completes; help name explains.\n";
        for (const auto& name : complete(""))
            out += name + "\n";
        return out;
    });
    command("find", "find words...: search names and help using ordered fragments", [this](const auto& a) {
        if (a.empty())
            throw std::runtime_error("Usage: find words...");
        std::string out, query;
        for (const auto& word : a)
            query += word + " ";
        for (const auto& name : search(query, true))
            out += describe(name) + "\n";
        return out.empty() ? "No matches." : out;
    });
    command("reset", "reset name: restore the registered default", [this](const auto& a, CVarSource source) {
        arguments(a, 1, 1, "Usage: reset name");
        const auto& v = at(a[0]);
        auto result = set(v.name,
                          std::holds_alternative<std::string>(v.defaults) ? std::get<std::string>(v.defaults)
                                                                          : format(v.defaults),
                          source);
        if (!result.ok)
            throw std::runtime_error(result.text);
        return result.text;
    });
}
CVar& ConsoleRegistry::variable(std::string name, ConsoleValue initial, std::string help, unsigned flags,
                                std::optional<CVarRange> range, std::vector<std::string> choices) {
    auto id = key(name);
    if (id.empty() || id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789._") != std::string::npos ||
        variables_.count(id) || commands_.count(id))
        throw std::runtime_error("Invalid or duplicate console name: " + name);
    if (range &&
        ((!std::holds_alternative<int>(initial) && !std::holds_alternative<double>(initial)) ||
         range->minimum > range->maximum || !std::isfinite(range->minimum) || !std::isfinite(range->maximum)))
        throw std::runtime_error("Invalid numeric CVar range: " + name);
    if (!choices.empty() && !std::holds_alternative<std::string>(initial))
        throw std::runtime_error("Choices require a string CVar");
    if (std::holds_alternative<double>(initial) && !std::isfinite(std::get<double>(initial)))
        throw std::runtime_error("Non-finite CVar default");
    CVar v{std::move(name), std::move(help),    initial, initial, initial, flags, CVarSource::Default,
           range,           std::move(choices), {}};
    validate(v, initial);
    return variables_.emplace(id, std::move(v)).first->second;
}
void ConsoleRegistry::command(std::string name, std::string help, Command callback) {
    if (!callback)
        throw std::runtime_error("Missing command callback: " + name);
    command(std::move(name), std::move(help),
            [callback = std::move(callback)](const auto& args, CVarSource) { return callback(args); });
}
void ConsoleRegistry::command(std::string name, std::string help, ContextCommand callback) {
    auto id = key(name);
    if (id.empty() || id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789._") != std::string::npos ||
        variables_.count(id) || commands_.count(id) || !callback)
        throw std::runtime_error("Invalid or duplicate command: " + name);
    commands_.emplace(id, Entry{std::move(name), std::move(help), std::move(callback)});
}
const CVar& ConsoleRegistry::at(const std::string& name) const {
    auto it = variables_.find(key(name));
    if (it == variables_.end())
        throw std::runtime_error("Unknown CVar: " + name);
    return it->second;
}
std::string ConsoleRegistry::format(const ConsoleValue& value) {
    if (auto p = std::get_if<bool>(&value))
        return *p ? "true" : "false";
    if (auto p = std::get_if<int>(&value))
        return std::to_string(*p);
    if (auto p = std::get_if<std::string>(&value))
        return quote(*p);
    char buffer[128];
    auto result = std::to_chars(buffer, buffer + sizeof(buffer), std::get<double>(value));
    return std::string(buffer, result.ptr);
}
ConsoleResult ConsoleRegistry::set(const std::string& name, const std::string& text, CVarSource source) {
    try {
        auto& var = const_cast<CVar&>(at(name));
        if (var.flags & CVarReadOnly)
            throw std::runtime_error("Read-only CVar: " + var.name);
        if (source < var.source)
            throw std::runtime_error("Ignored lower-priority assignment: " + var.name);
        auto value = parse(var, text);
        validate(var, value);
        const bool apply = !started_ || !(var.flags & CVarRestart);
        if (apply && var.active != value && var.changed)
            var.changed(value);
        var.value = value;
        var.source = source;
        if (apply)
            var.active = value;
        return {true, describe(name)};
    } catch (const std::exception& error) {
        return {false, error.what()};
    }
}
std::string ConsoleRegistry::describe(const std::string& name) const {
    auto cmd = commands_.find(key(name));
    if (cmd != commands_.end())
        return cmd->second.name + " -- " + cmd->second.help;
    const auto& v = at(name);
    static const char* types[] = {"bool", "int", "float", "string"};
    static const char* sources[] = {"default", "config", "command line", "console"};
    std::string out = v.name + " = " + format(v.value) + " [" + types[v.value.index()] + ", " +
                      sources[int(v.source)] + "]";
    if (v.flags & CVarReadOnly)
        out += " [read-only]";
    if (v.flags & CVarArchive)
        out += " [archive]";
    if (v.flags & CVarRestart)
        out += " [restart required]";
    if (v.value != v.active)
        out += " (active: " + format(v.active) + ")";
    out += "\n  " + v.help + "; default=" + format(v.defaults);
    if (v.range)
        out += "; range=" + format(v.range->minimum) + ".." + format(v.range->maximum);
    if (!v.choices.empty()) {
        out += "; choices=";
        for (const auto& s : v.choices)
            out += s + " ";
    }
    return out;
}
std::vector<std::string> ConsoleRegistry::tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::string token;
    bool quoted = false, started = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') {
            quoted = !quoted;
            started = true;
        } else if (quoted && c == '\\' && i + 1 < line.size()) {
            char next = line[++i];
            switch (next) {
            case 'n':
                token += '\n';
                break;
            case 'r':
                token += '\r';
                break;
            case 't':
                token += '\t';
                break;
            case '\\':
            case '"':
                token += next;
                break;
            default:
                token += '\\';
                token += next;
            }
            started = true;
        } else if (!quoted && (c == ' ' || c == '\t' || c == '=' || c == '\r')) {
            if (started) {
                tokens.push_back(token);
                token.clear();
                started = false;
            }
        } else {
            token += c;
            started = true;
        }
    }
    if (quoted)
        throw std::runtime_error("Unclosed quoted string");
    if (started)
        tokens.push_back(token);
    return tokens;
}
ConsoleResult ConsoleRegistry::execute(const std::string& line, CVarSource source) {
    try {
        auto args = tokenize(line);
        if (args.empty())
            return {true, ""};
        auto name = args.front();
        args.erase(args.begin());
        auto cmd = commands_.find(key(name));
        if (cmd != commands_.end())
            return {true, cmd->second.callback(args, source)};
        at(name);
        if (args.empty() || (args.size() == 1 && args[0] == "?"))
            return {true, describe(name)};
        arguments(args, 1, 1, "Usage: name value (quote strings containing spaces)");
        return set(name, args[0], source);
    } catch (const std::exception& error) {
        return {false, error.what()};
    }
}
bool ConsoleRegistry::contains(const std::string& name) const {
    const auto id = key(name);
    return variables_.count(id) || commands_.count(id);
}
std::vector<std::string> ConsoleRegistry::complete(const std::string& query) const {
    return search(query, false);
}
std::vector<std::string> ConsoleRegistry::search(const std::string& query, bool includeHelp) const {
    std::istringstream input(key(query));
    input.imbue(std::locale::classic());
    std::vector<std::string> words;
    for (std::string word; input >> word;)
        words.push_back(std::move(word));
    struct Candidate {
        bool helpOnly;
        MatchRank rank;
        std::string id, name;
    };
    std::vector<Candidate> candidates;
    auto add = [&](const auto& item) {
        auto rank = match(item.first, words);
        bool helpOnly = !rank;
        if (!rank && includeHelp)
            rank = match(item.first + " " + key(item.second.help), words);
        if (rank)
            candidates.push_back({helpOnly, *rank, item.first, item.second.name});
    };
    for (const auto& v : variables_)
        add(v);
    for (const auto& c : commands_)
        add(c);
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return std::tie(a.helpOnly, a.rank, a.id) < std::tie(b.helpOnly, b.rank, b.id);
    });
    std::vector<std::string> out;
    for (const auto& candidate : candidates)
        out.push_back(candidate.name);
    return out;
}
std::vector<ConsoleResult> ConsoleRegistry::load(std::istream& input) {
    std::vector<ConsoleResult> results;
    std::string line;
    int number = 0;
    while (std::getline(input, line)) {
        ++number;
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#' || line[first] == ';')
            continue;
        ConsoleResult result;
        try {
            auto args = tokenize(line);
            arguments(args, 2, 2, "Config expects: CVar=value");
            result = set(args[0], args[1], CVarSource::Config);
        } catch (const std::exception& error) {
            result = {false, error.what()};
        }
        result.text = "Line " + std::to_string(number) + ": " + result.text;
        results.push_back(std::move(result));
    }
    return results;
}
void ConsoleRegistry::save(std::ostream& out) const {
    out << "# Afterlight console variables; explicit save, archive variables only.\n";
    for (const auto& item : variables_) {
        const auto& v = item.second;
        if ((v.flags & CVarArchive) && !(v.flags & CVarReadOnly))
            out << v.name << "=" << format(v.value) << "\n";
    }
    if (!out)
        throw std::runtime_error("Could not write console configuration");
}
} // namespace afterlight
