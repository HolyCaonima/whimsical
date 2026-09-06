#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace afterlight {
// GPU readback of unfiltered linear signals; no tone mapping or history filtering is applied here.
struct RenderAudit {
    inline static constexpr uint32_t bindings[] = {26, 7, 23, 19, 20, 21, 22};
    inline static constexpr const char* names[] = {
        "direct", "albedo", "display", "raw-diffuse", "raw-specular", "denoised-diffuse", "denoised-specular"};
    static constexpr size_t signalCount = sizeof(bindings) / sizeof(bindings[0]);
    struct Pixel {
        std::array<float, 3> mean{};
        float meanL = 0, m2 = 0, previousL = 0, delta2 = 0;
    };
    std::array<std::vector<Pixel>, signalCount> signals;
    uint32_t samples = 0;
    uint64_t nonFinite = 0;
    static float half(uint16_t h) {
        int exponent = (h >> 10) & 31;
        float f = exponent == 0    ? std::ldexp(float(h & 1023), -24)
                  : exponent == 31 ? INFINITY
                                   : std::ldexp(float(1024 + (h & 1023)), exponent - 25);
        return (h & 32768) ? -f : f;
    }
    void add(const uint16_t* values, size_t pixels) {
        ++samples;
        for (size_t s = 0; s < signals.size(); ++s) {
            signals[s].resize(pixels);
            for (size_t i = 0; i < pixels; ++i) {
                auto& p = signals[s][i];
                std::array<float, 3> rgb;
                for (int c = 0; c < 3; ++c) {
                    rgb[c] = half(values[(s * pixels + i) * 4 + c]);
                    if (!std::isfinite(rgb[c])) {
                        ++nonFinite;
                        rgb[c] = 0;
                    }
                    p.mean[c] += (rgb[c] - p.mean[c]) / float(samples);
                }
                float l = rgb[0] * .2126f + rgb[1] * .7152f + rgb[2] * .0722f;
                float d = l - p.meanL;
                p.meanL += d / float(samples);
                p.m2 += d * (l - p.meanL);
                if (samples > 1)
                    p.delta2 += (l - p.previousL) * (l - p.previousL);
                p.previousL = l;
            }
        }
    }
    void save(const std::filesystem::path& dir, uint32_t width, uint32_t height) const {
        std::filesystem::create_directories(dir);
        std::ofstream report(dir / "audit.json");
        report << "{\n\"width\":" << width << ",\"height\":" << height << ",\"samples\":" << samples
               << ",\"nonFinite\":" << nonFinite << ",\"signals\":[\n";
        for (size_t s = 0; s < signals.size(); ++s) {
            std::ofstream file(dir / (std::string(names[s]) + ".f32"), std::ios::binary);
            double energy = 0, variance = 0, delta2 = 0;
            for (const auto& p : signals[s]) {
                float v = samples > 1 ? p.m2 / float(samples - 1) : 0;
                float d = samples > 1 ? p.delta2 / float(samples - 1) : 0;
                float data[] = {p.mean[0], p.mean[1], p.mean[2], v, d};
                file.write(reinterpret_cast<const char*>(data), sizeof(data));
                energy += p.meanL;
                variance += v;
                delta2 += d;
            }
            double count = double(width) * height;
            report << (s ? ",\n" : "") << "{\"name\":\"" << names[s]
                   << "\",\"meanLuminance\":" << energy / count
                   << ",\"temporalRms\":" << std::sqrt(variance / count)
                   << ",\"frameDeltaRms\":" << std::sqrt(delta2 / count) << "}";
        }
        report << "\n]}\n";
    }
};
} // namespace afterlight
