#pragma once
#include <filesystem>
#include <memory>
#include <vector>

namespace afterlight::animation::ai4animation {
// Immutable ONNX session shared by characters. Runtime tensors remain per invocation.
// Exported graphs include normalization, CxM iterations and the FiLM motion decoder.
class OnnxModel {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit OnnxModel(const std::filesystem::path&);
    ~OnnxModel();
    size_t inputSize() const;
    size_t outputSize() const;
    const std::vector<int64_t>& outputShape() const;
    std::vector<float> run(const std::vector<float>&) const;
};
} // namespace afterlight::animation::ai4animation
