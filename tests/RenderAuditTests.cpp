#include "render/RenderAuditWorker.h"
#include <chrono>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <algorithm>
using namespace afterlight;
static void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
static std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
static void addScalarReference(RenderAudit& audit, const uint16_t* values, size_t pixels) {
    ++audit.samples;
    for (size_t s = 0; s < RenderAudit::signalCount; ++s) {
        audit.signals[s].resize(pixels);
        for (size_t i = 0; i < pixels; ++i) {
            auto& p = audit.signals[s][i];
            std::array<float, 3> rgb;
            for (int c = 0; c < 3; ++c) {
                rgb[c] = RenderAudit::half(values[(s * pixels + i) * 4 + c]);
                if (!std::isfinite(rgb[c])) {
                    ++audit.nonFinite;
                    rgb[c] = 0;
                }
                p.mean[c] += (rgb[c] - p.mean[c]) / float(audit.samples);
            }
            float l = rgb[0] * .2126f + rgb[1] * .7152f + rgb[2] * .0722f;
            float d = l - p.mean[3];
            p.mean[3] += d / float(audit.samples);
            p.m2 += d * (l - p.mean[3]);
            if (audit.samples > 1)
                p.delta2 += (l - p.previousL) * (l - p.previousL);
            p.previousL = l;
        }
    }
}
int main() {
    try {
        // Exhaust the binary16 domain, including signed zero, subnormals, infinities and NaNs.
        for (uint32_t bits = 0; bits <= 65535; ++bits) {
            const auto value = RenderAudit::half(uint16_t(bits));
            const int exponent = (bits >> 10) & 31;
            const auto mantissa = bits & 1023;
            if (exponent == 31) {
                check(mantissa ? std::isnan(value) : std::isinf(value), "Half special value classification");
            } else {
                float expected = exponent ? std::ldexp(float(1024 + mantissa), exponent - 25)
                                          : std::ldexp(float(mantissa), -24);
                if (bits & 32768)
                    expected = -expected;
                check(value == expected && std::signbit(value) == std::signbit(expected), "Exact half conversion");
            }
        }
        constexpr uint32_t width = 7, height = 3;
        const size_t pixels = width * height;
        std::vector<uint16_t> frame(pixels * 4 * RenderAudit::signalCount);
        RenderAudit expected;
        RenderAuditWorker worker(width, height);
        for (uint32_t sample = 0; sample < 160; ++sample) {
            for (size_t i = 0; i < frame.size(); ++i)
                frame[i] = uint16_t((i * 7919 + sample * 997) & 65535);
            addScalarReference(expected, frame.data(), pixels);
            worker.submit(frame.data());
            std::fill(frame.begin(), frame.end(), uint16_t(0)); // Producer immediately reuses GPU staging.
        }
        const auto root = std::filesystem::temp_directory_path() /
            ("afterlight-audit-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const auto& actual = worker.finish(root / "async");
        check(actual.samples == 160 && actual.nonFinite == expected.nonFinite, "Drain preserves every sample and nonfinite count");
        for (size_t s = 0; s < RenderAudit::signalCount; ++s) {
            for (size_t i = 0; i < pixels; ++i) {
                const auto& a = actual.signals[s][i];
                const auto& b = expected.signals[s][i];
                check(a.mean == b.mean && a.m2 == b.m2 && a.previousL == b.previousL &&
                          a.delta2 == b.delta2, "Async statistics must match serial sample order exactly");
            }
        }
        expected.save(root / "serial", width, height);
        for (const auto* name : RenderAudit::names) {
            const std::string file = std::string(name) + ".f32";
            check(read(root / "async" / file) == read(root / "serial" / file), "Async f32 output matches serial bytes");
            std::filesystem::remove(root / "async" / file);
            std::filesystem::remove(root / "serial" / file);
        }
        check(read(root / "async/audit.json") == read(root / "serial/audit.json"), "Async JSON matches serial report");
        RenderAuditWorker failure(width, height);
        failure.submit(frame.data());
        bool failed = false;
        try { failure.finish(root / "async/audit.json/child"); }
        catch (const std::exception&) { failed = true; }
        check(failed, "Worker file errors must reach the caller");
        for (const auto* name : {"async", "serial"}) {
            std::filesystem::remove(root / name / "audit.json");
            std::filesystem::remove(root / name);
        }
        std::filesystem::remove(root);
        { RenderAuditWorker empty(width, height); empty.finish(); }
        { RenderAuditWorker shutdown(width, height); shutdown.submit(frame.data()); }
        std::cout << "PASS: all binary16 values, lossless ordered audit, staging ownership, drain, file output and worker errors\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
