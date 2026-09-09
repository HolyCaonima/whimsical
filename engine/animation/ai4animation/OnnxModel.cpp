#include "OnnxModel.h"
#include <onnxruntime_cxx_api.h>
#include <stdexcept>

namespace whimsical::animation::ai4animation {
static Ort::Env& environment() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "Whimsical.Animation");
    return env;
}
static size_t elements(const std::vector<int64_t>& shape) {
    size_t n = 1;
    for (auto dim : shape) {
        if (dim <= 0)
            throw std::runtime_error("Animation ONNX requires fixed positive tensor shapes");
        n *= size_t(dim);
    }
    return n;
}
struct OnnxModel::Impl {
    Ort::Session session{nullptr};
    std::string inputName, outputName;
    std::vector<int64_t> inputShape, outputShape;
    size_t inputs, outputs;
    explicit Impl(const std::string& bytes) {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(1); // Avoid a thread pool and spinning per character asset.
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        session = Ort::Session(environment(), bytes.data(), bytes.size(), options);
        if (session.GetInputCount() != 1 || session.GetOutputCount() != 1)
            throw std::runtime_error("Animation ONNX expects one feature input and one output");
        Ort::AllocatorWithDefaultOptions allocator;
        inputName = session.GetInputNameAllocated(0, allocator).get();
        outputName = session.GetOutputNameAllocated(0, allocator).get();
        auto inputType = session.GetInputTypeInfo(0);
        auto outputType = session.GetOutputTypeInfo(0);
        auto in = inputType.GetTensorTypeAndShapeInfo(), out = outputType.GetTensorTypeAndShapeInfo();
        if (in.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            out.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
            throw std::runtime_error("Animation ONNX expects float32");
        inputShape = in.GetShape();
        outputShape = out.GetShape();
        inputs = elements(inputShape);
        outputs = elements(outputShape);
    }
};
OnnxModel::OnnxModel(const std::string& bytes) : impl_(std::make_unique<Impl>(bytes)) {}
OnnxModel::~OnnxModel() = default;
size_t OnnxModel::inputSize() const {
    return impl_->inputs;
}
size_t OnnxModel::outputSize() const {
    return impl_->outputs;
}
const std::vector<int64_t>& OnnxModel::outputShape() const {
    return impl_->outputShape;
}
std::vector<float> OnnxModel::run(const std::vector<float>& features) const {
    if (features.size() != inputSize())
        throw std::invalid_argument("ONNX feature dimension mismatch");
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto tensor =
        Ort::Value::CreateTensor<float>(memory, const_cast<float*>(features.data()), features.size(),
                                        impl_->inputShape.data(), impl_->inputShape.size());
    const char* inputs[] = {impl_->inputName.c_str()};
    const char* outputs[] = {impl_->outputName.c_str()};
    auto result = impl_->session.Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
    const float* data = result[0].GetTensorData<float>();
    return {data, data + outputSize()};
}
} // namespace whimsical::animation::ai4animation
