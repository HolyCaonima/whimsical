#include "core/CpuProfile.h"
#include "assets/Json.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace afterlight;
static void check(bool value, const char* why) {
    if (!value)
        throw std::runtime_error(why);
}
static void validate(const CpuThreadProfile& thread) {
    const auto& rows = thread.scopes;
    for (size_t i = 0; i < rows.size(); ++i) {
        check(std::isfinite(rows[i].milliseconds) && rows[i].milliseconds >= 0 &&
                  rows[i].selfMilliseconds >= -1e-8,
              "Finite, nonnegative CPU durations");
        double accounted = rows[i].selfMilliseconds;
        for (size_t j = i + 1; j < rows.size(); ++j)
            if (rows[j].parent == int(i))
                accounted += rows[j].milliseconds;
        check(std::abs(accounted - rows[i].milliseconds) < 1e-8,
              "Inclusive equals self plus direct children, without double-counting grandchildren");
        check(rows[i].parent < int(i), "Parents precede children");
    }
}
int main() {
    try {
        CpuProfiler disabled(false, "None", "Disabled");
        { CpuScope scope("Ignored"); }
        check(disabled.finish().scopes.empty(), "Disabled profiling produces no samples");
        CpuProfiler game(true, "Game", "Game Update");
        CpuThreadProfile renderResult;
        std::thread render([&] {
            CpuProfiler profiler(true, "Render", "Render Frame");
            {
                CpuScope scope("Wait / Fence");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            renderResult = profiler.finish();
        });
        {
            CpuScope scope("Tick");
            try {
                CpuScope child("Throwing \"child\"");
                throw std::runtime_error("Unwind");
            } catch (const std::runtime_error&) {
            }
            CpuScope sibling("Sibling");
            sibling.finish();
            sibling.finish(); // Explicit close followed by destruction must not end twice.
        }
        {
            CpuProfiler nested(true, "Nested", "Nested Root");
            CpuScope scope("Abandoned");
        } // An abandoned capture must restore the enclosing thread-local binding.
        { CpuScope scope("Restored"); }
        render.join();
        auto gameResult = game.finish();
        validate(gameResult);
        validate(renderResult);
        check(gameResult.scopes.size() == 5 && gameResult.scopes[2].parent == 1 &&
                  gameResult.scopes[3].parent == 1 && gameResult.scopes[4].parent == 0,
              "RAII unwinding and nested capture preserve the scope tree");
        check(renderResult.scopes.size() == 2 && renderResult.scopes[1].milliseconds >= .5,
              "Thread-local captures are isolated and waits measure elapsed time");
        CpuProfile report{4, 42, 12, {gameResult, renderResult}};
        auto json = Json::parse(report.json());
        check(json.at("threads").elements().size() == 2 &&
                  json.at("threads").at(0).at("scopes").at(2).at("name").string() == "Throwing \"child\"" &&
                  json.at("frame").number() == 42,
              "JSON preserves thread lanes, frame identity and escaped names");
        check(report.text().find("[Game]") != std::string::npos &&
                  report.text().find("[Render]") != std::string::npos &&
                  report.text().find("inclusive / self") != std::string::npos,
              "Console report labels threads and timing semantics");
        std::cout << "PASS: CPU scope nesting, waits, thread isolation, unwind, disable and reports\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
