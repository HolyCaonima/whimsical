#include "core/GpuProfile.h"
#include "assets/Json.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace afterlight;
static void check(bool value, const char* why) {
    if (!value)
        throw std::runtime_error(why);
}
int main() {
    try {
        check(std::abs(gpuTimestampMilliseconds(100, 300, 64, 2.5) - .0005) < 1e-12,
              "Convert ticks using the GPU timestamp period");
        check(std::abs(gpuTimestampMilliseconds(0xfffffff0, 0x10, 32, 1) - .000032) < 1e-12,
              "Handle timestampValidBits wraparound");
        check(std::abs(gpuTimestampMilliseconds(UINT64_MAX - 15, 16, 64, 1) - .000032) < 1e-12,
              "Handle 64-bit timestamp wraparound without shifting by 64");
        GpuProfile report{7,
                          123,
                          960,
                          600,
                          "Test GPU",
                          {},
                          {{"GPU Frame", -1, 10}, {"NRD", 0, 5}, {"Pass \"A\"", 1, 2}, {"Composite", 0, 1}}};
        const auto text = report.text();
        check(text.find("50.0%    NRD") != std::string::npos &&
                  text.find("20.0%      Pass") != std::string::npos,
              "Report must show inclusive hierarchy and frame percentages");
        auto json = Json::parse(report.json());
        check(json.at("frame").number() == 123 && json.at("scopes").elements().size() == 4 &&
                  json.at("scopes").at(2).at("name").string() == "Pass \"A\"",
              "Machine-readable profile preserves timing tree and escaped names");
        report.scopes.clear();
        report.error = "Timestamps unsupported";
        check(report.text().find(report.error) != std::string::npos &&
                  Json::parse(report.json()).at("error").string() == report.error,
              "Unsupported timing must report an error, not invented zero measurements");
        std::cout << "PASS: GPU timestamp conversion, wraparound, hierarchy and report serialization\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
