#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <emmintrin.h>

namespace afterlight {
// GPU readback of unfiltered linear signals; no tone mapping or history filtering is applied here.
struct RenderAudit {
    inline static constexpr uint32_t bindings[] = {26, 7, 23, 19, 20, 21, 22};
    inline static constexpr const char* names[] = {
        "direct", "albedo", "display", "raw-diffuse", "raw-specular", "denoised-diffuse", "denoised-specular"};
    static constexpr size_t signalCount = sizeof(bindings) / sizeof(bindings[0]);
    struct Pixel {
        std::array<float, 4> mean{}; // RGB and luminance, updated together with x64 baseline SSE2.
        float m2 = 0, previousL = 0, delta2 = 0;
    };
    std::array<std::vector<Pixel>, signalCount> signals;
    uint32_t samples = 0;
    uint64_t nonFinite = 0;
    static float half(uint16_t h) {
        // binary16 -> binary32 is exact. Avoid millions of CRT ldexp calls per sample.
        const uint32_t sign = uint32_t(h & 0x8000) << 16;
        const uint32_t exponent = (h >> 10) & 31;
        const uint32_t mantissa = h & 1023;
        if (!exponent) {
            const float value = float(mantissa) * (1.f / 16777216.f);
            return sign ? -value : value;
        }
        const uint32_t bits = sign | ((exponent == 31 ? 255 : exponent + 112) << 23) | (mantissa << 13);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    void add(const uint16_t* values, size_t pixels) {
        ++samples;
        const auto sampleCount = _mm_set1_ps(float(samples));
        for (size_t s = 0; s < signals.size(); ++s) {
            signals[s].resize(pixels);
            for (size_t i = 0; i < pixels; ++i) {
                auto& p = signals[s][i];
                std::array<float, 3> rgb;
                for (int c = 0; c < 3; ++c) {
                    const auto packed = values[(s * pixels + i) * 4 + c];
                    if ((packed & 0x7c00) == 0x7c00) {
                        ++nonFinite;
                        rgb[c] = 0;
                    } else
                        rgb[c] = half(packed);
                }
                float l = rgb[0] * .2126f + rgb[1] * .7152f + rgb[2] * .0722f;
                float d = l - p.mean[3];
                const auto mean = _mm_loadu_ps(p.mean.data());
                const auto delta = _mm_sub_ps(_mm_set_ps(l, rgb[2], rgb[1], rgb[0]), mean);
                _mm_storeu_ps(p.mean.data(), _mm_add_ps(mean, _mm_div_ps(delta, sampleCount)));
                p.m2 += d * (l - p.mean[3]);
                if (samples > 1)
                    p.delta2 += (l - p.previousL) * (l - p.previousL);
                p.previousL = l;
            }
        }
    }
    void save(const std::filesystem::path& dir, uint32_t width, uint32_t height) const {
        std::filesystem::create_directories(dir);
        std::ofstream report(dir / "audit.json");
        report.exceptions(std::ios::failbit | std::ios::badbit);
        report << "{\n\"width\":" << width << ",\"height\":" << height << ",\"samples\":" << samples
               << ",\"nonFinite\":" << nonFinite << ",\"signals\":[\n";
        for (size_t s = 0; s < signals.size(); ++s) {
            std::ofstream file(dir / (std::string(names[s]) + ".f32"), std::ios::binary);
            file.exceptions(std::ios::failbit | std::ios::badbit);
            double energy = 0, variance = 0, delta2 = 0;
            std::vector<std::array<float, 5>> output;
            output.reserve(signals[s].size());
            for (const auto& p : signals[s]) {
                float v = samples > 1 ? p.m2 / float(samples - 1) : 0;
                float d = samples > 1 ? p.delta2 / float(samples - 1) : 0;
                output.push_back({p.mean[0], p.mean[1], p.mean[2], v, d});
                energy += p.mean[3];
                variance += v;
                delta2 += d;
            }
            file.write(reinterpret_cast<const char*>(output.data()), std::streamsize(output.size() * sizeof(output[0])));
            file.close();
            double count = double(width) * height;
            report << (s ? ",\n" : "") << "{\"name\":\"" << names[s]
                   << "\",\"meanLuminance\":" << energy / count
                   << ",\"temporalRms\":" << std::sqrt(variance / count)
                   << ",\"frameDeltaRms\":" << std::sqrt(delta2 / count) << "}";
        }
        report << "\n]}\n";
        report.close();
    }
};
} // namespace afterlight
