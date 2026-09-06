#pragma once
#include "UiFrame.h"
#include <filesystem>
#include <functional>
#include <string>
namespace Rml {
class Context;
class ElementDocument;
} // namespace Rml

namespace afterlight {
struct Input;
namespace ui {
// One context per host. All DOM access and callbacks run on the simulation thread.
class UiCore {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit UiCore(std::filesystem::path content);
    ~UiCore();
    UiCore(const UiCore&) = delete;
    Rml::Context& context();
    std::string resolve(const std::string& path) const;
    Rml::ElementDocument* loadDocument(const std::string& path);
    Rml::ElementDocument* createDocument(const std::string& rml,
                                         const std::string& source = "/Game/UI/inline.rml");
    bool loadFont(const std::string& path, bool fallback = false);
    void processInput(Input&);
    void resize(int width, int height);
    std::shared_ptr<const UiFrame> snapshot();
    static std::string escape(const std::string&);
};
} // namespace ui
} // namespace afterlight
