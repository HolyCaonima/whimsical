#include "UiCore.h"
#include "core/Input.h"
#include <RmlUi/Core.h>
#include <RmlUi/Core/StyleSheetContainer.h>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <unordered_map>
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace whimsical::ui {
namespace {
uint64_t resourceId = 0, contextId = 0;
// RmlUi owns global interfaces, while each context supplies its own mount table.
struct Files : Rml::FileInterface {
    std::vector<std::weak_ptr<const ContentMounts>> providers;
    void add(std::shared_ptr<const ContentMounts> mounts) {
        for (auto it = providers.begin(); it != providers.end();) {
            if (auto existing = it->lock()) {
                if (existing == mounts)
                    return;
                ++it;
            } else
                it = providers.erase(it);
        }
        providers.push_back(mounts);
    }
    std::shared_ptr<const ContentMounts> provider(const std::string& uri) const {
        for (const auto& weak : providers)
            if (auto mounts = weak.lock()) {
                try {
                    (void)mounts->fromUri(uri);
                    return mounts;
                } catch (const std::invalid_argument&) {
                }
            }
        throw std::invalid_argument("Unknown or unmounted UI resource: " + uri);
    }
    ContentFile resolve(const std::string& uri) const {
        return provider(uri)->fromUri(uri);
    }
    Rml::FileHandle Open(const Rml::String& uri) override {
        try {
            return reinterpret_cast<Rml::FileHandle>(_wfopen(resolve(uri).physical().c_str(), L"rb"));
        } catch (const std::exception& e) {
            Rml::Log::Message(Rml::Log::LT_ERROR, "%s", e.what());
            return 0;
        }
    }
    void Close(Rml::FileHandle file) override {
        fclose(reinterpret_cast<FILE*>(file));
    }
    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override {
        return fread(buffer, 1, size, reinterpret_cast<FILE*>(file));
    }
    bool Seek(Rml::FileHandle file, long offset, int origin) override {
        return fseek(reinterpret_cast<FILE*>(file), offset, origin) == 0;
    }
    size_t Tell(Rml::FileHandle file) override {
        return ftell(reinterpret_cast<FILE*>(file));
    }
};
struct System : Rml::SystemInterface {
    Files& files;
    explicit System(Files& f) : files(f) {}
    void JoinPath(Rml::String& translated, const Rml::String& document, const Rml::String& path) override {
        try {
            auto mounts = files.provider(document);
            translated = mounts->relative(mounts->fromUri(document), path).uri();
        } catch (const std::exception& e) {
            translated = "/__invalid_content_resource";
            Rml::Log::Message(Rml::Log::LT_ERROR, "%s", e.what());
        }
    }

    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    double GetElapsedTime() override {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
    bool LogMessage(Rml::Log::Type, const Rml::String& message) override {
        std::cerr << "[RmlUi] " << message << '\n';
        return true;
    }
    void SetClipboardText(const Rml::String& text) override {
        int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        auto memory = GlobalAlloc(GMEM_MOVEABLE, size_t(size) * sizeof(wchar_t));
        if (!memory)
            return;
        auto* wide = static_cast<wchar_t*>(GlobalLock(memory));
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide, size);
        GlobalUnlock(memory);
        if (OpenClipboard(nullptr)) {
            EmptyClipboard();
            if (SetClipboardData(CF_UNICODETEXT, memory))
                memory = nullptr;
            CloseClipboard();
        }
        if (memory)
            GlobalFree(memory);
    }
    void GetClipboardText(Rml::String& text) override {
        text.clear();
        if (!OpenClipboard(nullptr))
            return;
        auto memory = GetClipboardData(CF_UNICODETEXT);
        if (memory) {
            auto* wide = static_cast<const wchar_t*>(GlobalLock(memory));
            if (wide) {
                int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
                text.resize(size_t(size));
                WideCharToMultiByte(CP_UTF8, 0, wide, -1, text.data(), size, nullptr, nullptr);
                text.resize(size_t(size - 1));
                GlobalUnlock(memory);
            }
        }
        CloseClipboard();
    }
};
struct Services {
    Files files;
    System system{files};
    bool com = false;
    explicit Services(std::shared_ptr<const ContentMounts> mounts) {
        files.add(mounts);
        com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        Rml::SetSystemInterface(&system);
        Rml::SetFileInterface(&files);
        if (!Rml::Initialise())
            throw std::runtime_error("RmlUi initialization failed");
    }
    ~Services() {
        Rml::Shutdown();
        if (com)
            CoUninitialize();
    }
};
std::shared_ptr<Services> services(std::shared_ptr<const ContentMounts> mounts) {
    static std::weak_ptr<Services> instance;
    auto result = instance.lock();
    if (!result)
        instance = result = std::make_shared<Services>(mounts);
    else
        result->files.add(mounts);
    return result;
}
const std::array<float, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
struct Recorder : Rml::RenderInterface {
    Files& files;
    explicit Recorder(Files& f) : files(f) {}
    std::unordered_map<uint64_t, std::shared_ptr<const Geometry>> geometries;
    std::unordered_map<uint64_t, std::shared_ptr<const Texture>> textures;
    UiFrame frame;
    bool clipped = false;
    std::array<int, 4> scissor{};
    std::array<float, 16> transform = identity;
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override {
        auto result = std::make_shared<Geometry>();
        result->id = ++resourceId;
        result->vertices.reserve(vertices.size());
        for (const auto& v : vertices) {
            uint32_t color;
            std::memcpy(&color, &v.colour, sizeof(color));
            result->vertices.push_back({v.position.x, v.position.y, color, v.tex_coord.x, v.tex_coord.y});
        }
        result->indices.assign(indices.begin(), indices.end());
        geometries[result->id] = result;
        return result->id;
    }
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                        Rml::TextureHandle texture) override {
        frame.draws.push_back({geometries.at(geometry), texture ? textures.at(texture) : nullptr, transform,
                               translation.x, translation.y,
                               clipped ? scissor : std::array<int, 4>{0, 0, frame.width, frame.height}});
    }
    void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override {
        geometries.erase(handle);
    }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i size) override {
        auto result = std::make_shared<Texture>();
        result->id = ++resourceId;
        result->width = size.x;
        result->height = size.y;
        result->pixels.assign(source.begin(), source.end());
        textures[result->id] = result;
        return result->id;
    }
    Rml::TextureHandle LoadTexture(Rml::Vector2i& size, const Rml::String& source) override {
        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> image;
        ComPtr<IWICFormatConverter> converter;
        std::filesystem::path path;
        try {
            path = files.resolve(source).physical();
        } catch (const std::exception& e) {
            Rml::Log::Message(Rml::Log::LT_ERROR, "%s", e.what());
            return 0;
        }
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory))) ||
            FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnLoad, &decoder)) ||
            FAILED(decoder->GetFrame(0, &image)) || FAILED(factory->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(image.Get(), GUID_WICPixelFormat32bppPRGBA, WICBitmapDitherTypeNone,
                                         nullptr, 0, WICBitmapPaletteTypeCustom))) {
            Rml::Log::Message(Rml::Log::LT_ERROR, "Cannot load UI texture: %s", source.c_str());
            return 0;
        }
        UINT width, height;
        converter->GetSize(&width, &height);
        std::vector<uint8_t> pixels(size_t(width) * height * 4);
        if (FAILED(converter->CopyPixels(nullptr, width * 4, UINT(pixels.size()), pixels.data())))
            return 0;
        size = {int(width), int(height)};
        return GenerateTexture({pixels.data(), pixels.size()}, size);
    }
    void ReleaseTexture(Rml::TextureHandle handle) override {
        textures.erase(handle);
    }
    void EnableScissorRegion(bool value) override {
        clipped = value;
    }
    void SetScissorRegion(Rml::Rectanglei value) override {
        scissor = {value.Left(), value.Top(), value.Width(), value.Height()};
    }
    void SetTransform(const Rml::Matrix4f* value) override {
        if (value)
            std::memcpy(transform.data(), value->data(), sizeof(transform));
        else
            transform = identity;
    }
};
Rml::Input::KeyIdentifier key(int vk) {
    using namespace Rml::Input;
    if (vk >= 'A' && vk <= 'Z')
        return KeyIdentifier(KI_A + vk - 'A');
    if (vk >= '0' && vk <= '9')
        return KeyIdentifier(KI_0 + vk - '0');
    if (vk >= VK_F1 && vk <= VK_F12)
        return KeyIdentifier(KI_F1 + vk - VK_F1);
    switch (vk) {
    case VK_BACK:
        return KI_BACK;
    case VK_TAB:
        return KI_TAB;
    case VK_RETURN:
        return KI_RETURN;
    case VK_ESCAPE:
        return KI_ESCAPE;
    case VK_SPACE:
        return KI_SPACE;
    case VK_LEFT:
        return KI_LEFT;
    case VK_RIGHT:
        return KI_RIGHT;
    case VK_UP:
        return KI_UP;
    case VK_DOWN:
        return KI_DOWN;
    case VK_HOME:
        return KI_HOME;
    case VK_END:
        return KI_END;
    case VK_DELETE:
        return KI_DELETE;
    case VK_INSERT:
        return KI_INSERT;
    case VK_PRIOR:
        return KI_PRIOR;
    case VK_NEXT:
        return KI_NEXT;
    case VK_SHIFT:
        return KI_LSHIFT;
    case VK_CONTROL:
        return KI_LCONTROL;
    case VK_MENU:
        return KI_LMENU;
    default:
        return KI_UNKNOWN;
    }
}
} // namespace
struct UiCore::Impl {
    std::shared_ptr<Services> service;
    Recorder recorder;
    Rml::SharedPtr<Rml::StyleSheetContainer> base;
    Rml::Context* context;
    std::shared_ptr<const ContentMounts> mounts;
    Input previous;
    bool pointerOwned = false;
    explicit Impl(std::shared_ptr<const ContentMounts> roots)
        : service(services(roots)), recorder(service->files), mounts(std::move(roots)) {
        if (mounts->contains("/Engine")) {
            base = Rml::Factory::InstanceStyleSheetFile(mounts->locate("/Engine/UI/base.rcss").uri());
            if (!base)
                throw std::runtime_error("RmlUi base stylesheet could not be loaded");
            for (const char* name : {"LatoLatin-Regular.ttf", "LatoLatin-Bold.ttf"})
                if (!Rml::LoadFontFace(mounts->locate(std::string("/Engine/Fonts/") + name).uri()))
                    throw std::runtime_error("RmlUi default font could not be loaded");
        }
        if (mounts->contains("/SystemFonts"))
            Rml::LoadFontFace(mounts->locate("/SystemFonts/msyh.ttc").uri(), true);
        context = Rml::CreateContext("ui-" + std::to_string(++contextId), {1280, 800}, &recorder);
        if (!context)
            throw std::runtime_error("RmlUi context creation failed");
    }
    ~Impl() {
        Rml::RemoveContext(context->GetName());
        Rml::ReleaseRenderManagers();
    }
    void style(Rml::ElementDocument* doc) {
        if (!base)
            return;
        auto* sheet = doc->GetStyleSheetContainer();
        doc->SetStyleSheetContainer(sheet ? base->CombineStyleSheetContainer(*sheet) : base);
    }
};
UiCore::UiCore(std::shared_ptr<const ContentMounts> mounts)
    : impl_(std::make_unique<Impl>(std::move(mounts))) {}
UiCore::~UiCore() = default;
Rml::Context& UiCore::context() {
    return *impl_->context;
}
std::string UiCore::resolve(const std::string& path) const {
    return (ContentMounts::isUri(path) ? impl_->mounts->fromUri(path) : impl_->mounts->locate(path)).uri();
}
Rml::ElementDocument* UiCore::loadDocument(const std::string& path) {
    auto* doc = context().LoadDocument(resolve(path));
    if (!doc)
        throw std::runtime_error("Cannot load RML document: " + path);
    impl_->style(doc);
    return doc;
}
Rml::ElementDocument* UiCore::createDocument(const std::string& rml, const std::string& source) {
    auto url = resolve(source);
    auto* doc = context().LoadDocumentFromMemory(rml, url);
    if (!doc)
        throw std::runtime_error("Cannot create RML document: " + source);
    impl_->style(doc);
    return doc;
}
bool UiCore::loadFont(const std::string& path, bool fallback) {
    return Rml::LoadFontFace(resolve(path), fallback);
}
void UiCore::resize(int width, int height) {
    if (width > 0 && height > 0)
        context().SetDimensions({width, height});
}
void UiCore::processInput(Input& input) {
    auto& p = *impl_;
    auto& c = context();
    resize(input.width, input.height);
    c.Update();
    int modifiers = (input.keys[VK_SHIFT] ? Rml::Input::KM_SHIFT : 0) |
                    (input.keys[VK_CONTROL] ? Rml::Input::KM_CTRL : 0) |
                    (input.keys[VK_MENU] ? Rml::Input::KM_ALT : 0);
    Input raw = input;
    const bool blocked = input.pointerCaptured && input.keyboardCaptured;
    const bool ordered = !input.uiEvents.empty();
    if (!input.focused || blocked) {
        c.ProcessMouseLeave();
        if (auto* focus = c.GetFocusElement())
            focus->Blur();
        const bool held[] = {p.previous.left, p.previous.right, p.previous.middle};
        for (int i = 0; i < 3; ++i)
            if (held[i])
                c.ProcessMouseButtonUp(i, 0);
        for (int i = 0; i < 256; ++i)
            if (p.previous.keys[i])
                c.ProcessKeyUp(key(i), 0);
        p.pointerOwned = false;
        p.previous = raw;
        input.pointerCaptured = input.keyboardCaptured = blocked;
        return;
    } else
        c.ProcessMouseMove(int(input.mouseX), int(input.mouseY), modifiers);
    auto* hover = c.GetHoverElement();
    bool pointer = blocked || (input.focused && hover && hover != c.GetRootElement());
    bool modal = false;
    for (int i = 0; i < c.GetNumDocuments(); ++i)
        modal = modal || (c.GetDocument(i)->IsVisible() && c.GetDocument(i)->IsModal());
    pointer = pointer || (input.focused && modal);
    bool wasOwned = p.pointerOwned;
    const bool buttons[] = {input.left, input.right, input.middle};
    const bool previous[] = {p.previous.left, p.previous.right, p.previous.middle};
    const bool pressed[] = {input.leftPressed, input.rightPressed, input.middle && !p.previous.middle};
    if (!ordered)
        for (int i = 0; i < 3; ++i) {
            if (input.focused && !blocked && (pressed[i] || (buttons[i] && !previous[i]))) {
                c.ProcessMouseButtonDown(i, modifiers);
                p.pointerOwned = p.pointerOwned || pointer;
            }
            if ((previous[i] && !buttons[i]) || (pressed[i] && !buttons[i]))
                c.ProcessMouseButtonUp(i, modifiers);
        }
    pointer = pointer || wasOwned || p.pointerOwned;
    if (!ordered && !blocked && input.focused && input.wheel)
        c.ProcessMouseWheel(-input.wheel, modifiers);
    auto* focus = c.GetFocusElement();
    bool keyboard =
        blocked || (input.focused &&
                    (modal || (focus && focus != c.GetRootElement() && focus != focus->GetOwnerDocument())));
    if (!ordered)
        for (int i = 0; i < 256; ++i) {
            if (!blocked && input.focused && (input.pressed[i] || (input.keys[i] && !p.previous.keys[i])))
                c.ProcessKeyDown(key(i), modifiers);
            if (p.previous.keys[i] && !input.keys[i])
                c.ProcessKeyUp(key(i), modifiers);
        }
    if (!ordered && !blocked && input.focused)
        for (auto character : input.text)
            if (character >= 32 && character != 127)
                c.ProcessTextInput(Rml::Character(character));
    if (!blocked && input.focused)
        for (const auto& event : input.uiEvents) {
            using Type = InputEvent::Type;
            const int mods = ((event.modifiers & 1) ? Rml::Input::KM_SHIFT : 0) |
                             ((event.modifiers & 2) ? Rml::Input::KM_CTRL : 0) |
                             ((event.modifiers & 4) ? Rml::Input::KM_ALT : 0);
            switch (event.type) {
            case Type::KeyDown:
                c.ProcessKeyDown(key(event.code), mods);
                break;
            case Type::KeyUp:
                c.ProcessKeyUp(key(event.code), mods);
                break;
            case Type::Text:
                c.ProcessTextInput(Rml::Character(event.code));
                break;
            case Type::MouseMove:
                c.ProcessMouseMove(int(event.x), int(event.y), mods);
                break;
            case Type::MouseDown: {
                c.ProcessMouseMove(int(event.x), int(event.y), mods);
                auto* hit = c.GetHoverElement();
                p.pointerOwned = p.pointerOwned || modal || (hit && hit != c.GetRootElement());
                c.ProcessMouseButtonDown(event.code, mods);
                break;
            }
            case Type::MouseUp:
                c.ProcessMouseMove(int(event.x), int(event.y), mods);
                c.ProcessMouseButtonUp(event.code, mods);
                break;
            case Type::Wheel:
                c.ProcessMouseWheel(-event.x, mods);
                break;
            case Type::FocusLost:
                c.ProcessMouseLeave();
                if (auto* focused = c.GetFocusElement())
                    focused->Blur();
                break;
            }
            auto* focused = c.GetFocusElement();
            keyboard = keyboard ||
                       (focused && focused != c.GetRootElement() && focused != focused->GetOwnerDocument());
            pointer = pointer || p.pointerOwned;
        }
    if (!input.left && !input.right && !input.middle)
        p.pointerOwned = false;
    p.previous = std::move(raw);
    input.pointerCaptured = pointer;
    input.keyboardCaptured = keyboard;
    if (pointer) {
        input.left = input.right = input.middle = input.leftPressed = input.rightPressed = false;
        input.deltaX = input.deltaY = input.wheel = 0;
    }
    if (keyboard) {
        input.keys.fill(false);
        input.pressed.fill(false);
        input.text.clear();
    }
    if (pointer || keyboard)
        input.uiEvents.clear();
}
std::shared_ptr<const UiFrame> UiCore::snapshot() {
    auto& recorder = impl_->recorder;
    auto size = context().GetDimensions();
    recorder.frame = {size.x, size.y, {}};
    context().Update();
    context().Render();
    return std::make_shared<const UiFrame>(std::move(recorder.frame));
}
std::string UiCore::escape(const std::string& text) {
    std::string result;
    for (char c : text) {
        switch (c) {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '"':
            result += "&quot;";
            break;
        case '\'':
            result += "&#39;";
            break;
        default:
            result += c;
        }
    }
    return result;
}
} // namespace whimsical::ui
