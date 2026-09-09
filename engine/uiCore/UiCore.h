#pragma once
#include "UiFrame.h"
#include "assets/ContentMounts.h"
#include <filesystem>
#include <functional>
#include <string>
namespace Rml {
class Context;
class Element;
class ElementDocument;
} // namespace Rml

namespace whimsical {
struct Input;
namespace ui {
// One context per host. All DOM access and callbacks run on the simulation thread.
class UiCore {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit UiCore(std::shared_ptr<const ContentMounts>);
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
    // Text is a value update. Existing text nodes keep their identity; markup
    // replacement remains a separate operation on the DOM.
    static void setText(Rml::Element&, const std::string&);
    static std::string escape(const std::string&);
};
} // namespace ui
} // namespace whimsical
