#include "UiBindings.h"
#include "uiCore/UiCore.h"
#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <unordered_map>

namespace afterlight {
struct UiBindings::Impl {
    duk_context* js;
    ui::UiCore& ui;
    std::function<std::string(const std::string&)> resolve;
    std::function<void(const std::string&)> log;
    std::function<int(int)> invoke;
    uint32_t nextNode = 0, nextListener = 0;
    std::unordered_map<uint32_t, Rml::ObserverPtr<Rml::Element>> nodes;
    struct Document {
        Rml::ObserverPtr<Rml::Element> element;
        bool visible = false, modal = false;
    };
    std::vector<Document> documents;
    bool visible = true;
    void setVisible(bool value) {
        if (visible == value)
            return;
        visible = value;
        for (auto& d : documents)
            if (d.element) {
                auto& doc = document(*d.element.get());
                if (visible && d.visible)
                    doc.Show(d.modal ? Rml::ModalFlag::Modal : Rml::ModalFlag::None, Rml::FocusFlag::None);
                else
                    doc.Hide();
            }
    }
    struct Listener : Rml::EventListener, std::enable_shared_from_this<Listener> {
        Impl& owner;
        uint32_t id;
        Rml::ObserverPtr<Rml::Element> element;
        std::string type;
        bool capture;
        Listener(Impl& o, uint32_t i, Rml::Element& e, std::string t, bool c)
            : owner(o), id(i), element(e.GetObserverPtr()), type(std::move(t)), capture(c) {}
        void ProcessEvent(Rml::Event& event) override {
            auto keepAlive = shared_from_this(); // A callback may close its own document or unsubscribe.
            auto* c = owner.js;
            const auto top = duk_get_top(c);
            duk_push_heap_stash(c);
            duk_get_prop_string(c, -1, "uiCallbacks");
            duk_get_prop_index(c, -1, id);
            duk_push_object(c);
            duk_push_string(c, event.GetType().c_str());
            duk_put_prop_string(c, -2, "type");
            duk_push_uint(c, owner.handle(event.GetTargetElement()));
            duk_put_prop_string(c, -2, "target");
            duk_push_uint(c, owner.handle(event.GetCurrentElement()));
            duk_put_prop_string(c, -2, "currentTarget");
            duk_push_object(c);
            for (const auto& entry : event.GetParameters()) {
                const auto& v = entry.second;
                switch (v.GetType()) {
                case Rml::Variant::BOOL:
                    duk_push_boolean(c, v.Get<bool>());
                    break;
                case Rml::Variant::INT:
                case Rml::Variant::INT64:
                case Rml::Variant::FLOAT:
                case Rml::Variant::DOUBLE:
                    duk_push_number(c, v.Get<double>());
                    break;
                default:
                    duk_push_string(c, v.Get<Rml::String>().c_str());
                    break;
                }
                duk_put_prop_string(c, -2, entry.first.c_str());
            }
            duk_put_prop_string(c, -2, "parameters");
            if (owner.invoke(1) != 0)
                owner.log(std::string("UI event: ") + duk_safe_to_stacktrace(c, -1));
            else if (duk_get_boolean(c, -1))
                event.StopPropagation();
            duk_set_top(c, top);
        }
        void OnDetach(Rml::Element*) override {
            auto keepAlive = shared_from_this();
            element.reset();
            duk_push_heap_stash(owner.js);
            duk_get_prop_string(owner.js, -1, "uiCallbacks");
            duk_del_prop_index(owner.js, -1, id);
            duk_pop_2(owner.js);
            owner.listeners.erase(id);
        }
    };
    std::unordered_map<uint32_t, std::shared_ptr<Listener>> listeners;
    uint32_t handle(Rml::Element* element) {
        if (!element)
            return 0;
        for (auto it = nodes.begin(); it != nodes.end();) {
            if (!it->second)
                it = nodes.erase(it);
            else {
                if (it->second.get() == element)
                    return it->first;
                ++it;
            }
        }
        nodes[++nextNode] = element->GetObserverPtr();
        return nextNode;
    }
    Rml::Element& node(uint32_t id) {
        auto it = nodes.find(id);
        if (it == nodes.end() || !it->second)
            throw std::runtime_error("UI element is no longer alive");
        return *it->second.get();
    }
    Rml::ElementDocument& document(Rml::Element& element) {
        auto* result = dynamic_cast<Rml::ElementDocument*>(&element);
        if (!result)
            throw std::invalid_argument("UI operation requires a document");
        return *result;
    }
    static duk_ret_t call(duk_context* c) {
        duk_push_heap_stash(c);
        duk_get_prop_string(c, -1, "uiBindings");
        auto& self = *static_cast<Impl*>(duk_get_pointer(c, -1));
        duk_pop_2(c);
        try {
            return self.dispatch(c);
        } catch (const std::exception& e) {
            duk_push_error_object(c, DUK_ERR_ERROR, "%s", e.what());
        }
        duk_throw_raw(c);
    }
    duk_ret_t dispatch(duk_context* c) {
        const std::string op = duk_require_string(c, 0);
        auto string = [&](int i) { return std::string(duk_require_string(c, i)); };
        if (op == "load" || op == "create") {
            auto* doc = op == "load"
                            ? ui.loadDocument(resolve(string(2)))
                            : ui.createDocument(string(2),
                                                resolve(duk_get_string_default(c, 3, "/Game/UI/inline.rml")));
            documents.erase(std::remove_if(documents.begin(), documents.end(),
                                           [](const auto& item) { return !item.element; }),
                            documents.end());
            documents.push_back({doc->GetObserverPtr()});
            duk_push_uint(c, handle(doc));
            return 1;
        }
        if (op == "font") {
            duk_push_boolean(c, ui.loadFont(resolve(string(2)), duk_get_boolean(c, 3)));
            return 1;
        }
        if (op == "off") {
            auto found = listeners.find(duk_require_uint(c, 2));
            if (found != listeners.end()) {
                auto listener = found->second;
                listener->element->RemoveEventListener(listener->type, listener.get(), listener->capture);
            }
            return 0;
        }
        auto& e = node(duk_require_uint(c, 1));
        if (op == "query" || op == "id") {
            duk_push_uint(c,
                          handle(op == "query" ? e.QuerySelector(string(2)) : e.GetElementById(string(2))));
            return 1;
        }
        if (op == "queryAll") {
            Rml::ElementList found;
            e.QuerySelectorAll(found, string(2));
            duk_push_array(c);
            for (size_t i = 0; i < found.size(); ++i) {
                duk_push_uint(c, handle(found[i]));
                duk_put_prop_index(c, -2, duk_uarridx_t(i));
            }
            return 1;
        }
        if (op == "text")
            e.SetInnerRML(ui::UiCore::escape(string(2)));
        else if (op == "rml")
            e.SetInnerRML(string(2));
        else if (op == "getRml") {
            duk_push_string(c, e.GetInnerRML().c_str());
            return 1;
        } else if (op == "style") {
            if (!e.SetProperty(string(2), string(3)))
                throw std::invalid_argument("Invalid RCSS property");
        } else if (op == "getStyle") {
            auto* p = e.GetProperty(string(2));
            if (p)
                duk_push_string(c, p->ToString().c_str());
            else
                duk_push_null(c);
            return 1;
        } else if (op == "attr")
            e.SetAttribute(string(2), string(3));
        else if (op == "getAttr") {
            auto* a = e.GetAttribute(string(2));
            if (a)
                duk_push_string(c, a->Get<Rml::String>().c_str());
            else
                duk_push_null(c);
            return 1;
        } else if (op == "removeAttr")
            e.RemoveAttribute(string(2));
        else if (op == "class")
            e.SetClass(string(2), duk_require_boolean(c, 3) != 0);
        else if (op == "hasClass") {
            duk_push_boolean(c, e.IsClassSet(string(2)));
            return 1;
        } else if (op == "value" || op == "getValue") {
            auto* control = dynamic_cast<Rml::ElementFormControl*>(&e);
            if (!control)
                throw std::invalid_argument("UI value requires a form control");
            if (op == "value")
                control->SetValue(string(2));
            else {
                duk_push_string(c, control->GetValue().c_str());
                return 1;
            }
        } else if (op == "append") {
            auto child = e.GetOwnerDocument()->CreateElement(string(2));
            auto* raw = child.get();
            e.AppendChild(std::move(child));
            duk_push_uint(c, handle(raw));
            return 1;
        } else if (op == "remove") {
            if (&e == e.GetOwnerDocument())
                document(e).Close();
            else if (auto* parent = e.GetParentNode())
                parent->RemoveChild(&e);
        } else if (op == "show" || op == "hide") {
            auto& doc = document(e);
            bool show = op == "show", modal = duk_get_boolean(c, 2) != 0;
            for (auto& d : documents)
                if (d.element.get() == &doc) {
                    d.visible = show;
                    d.modal = modal;
                }
            if (show && visible)
                doc.Show(modal ? Rml::ModalFlag::Modal : Rml::ModalFlag::None);
            else
                doc.Hide();
        } else if (op == "close")
            document(e).Close();
        else if (op == "focus")
            e.Focus();
        else if (op == "blur")
            e.Blur();
        else if (op == "bounds") {
            ui.context().Update();
            auto offset = e.GetAbsoluteOffset(Rml::BoxArea::Border);
            auto size = e.GetBox().GetSize(Rml::BoxArea::Border);
            duk_push_object(c);
            for (auto p : {std::pair<const char*, float>{"x", offset.x},
                           {"y", offset.y},
                           {"width", size.x},
                           {"height", size.y}}) {
                duk_push_number(c, p.second);
                duk_put_prop_string(c, -2, p.first);
            }
            return 1;
        } else if (op == "on") {
            duk_require_function(c, 3);
            auto listener =
                std::make_shared<Listener>(*this, ++nextListener, e, string(2), duk_get_boolean(c, 4) != 0);
            duk_push_heap_stash(c);
            duk_get_prop_string(c, -1, "uiCallbacks");
            duk_dup(c, 3);
            duk_put_prop_index(c, -2, listener->id);
            duk_pop_2(c);
            listeners[listener->id] = listener;
            e.AddEventListener(listener->type, listener.get(), listener->capture);
            duk_push_uint(c, listener->id);
            return 1;
        } else
            throw std::invalid_argument("Unknown UI operation: " + op);
        return 0;
    }
    Impl(duk_context* c, ui::UiCore& u, std::function<void(const std::string&)> sink,
         std::function<std::string(const std::string&)> resolver, std::function<int(int)> caller)
        : js(c), ui(u), resolve(std::move(resolver)), log(std::move(sink)), invoke(std::move(caller)) {
        duk_push_heap_stash(c);
        duk_push_pointer(c, this);
        duk_put_prop_string(c, -2, "uiBindings");
        duk_push_object(c);
        duk_put_prop_string(c, -2, "uiCallbacks");
        duk_pop(c);
        duk_push_c_function(c, call, DUK_VARARGS);
        duk_put_global_string(c, "__uiNative");
        const char* source = R"JS(
(function(native) {
    function element(id) { return id ? new Element(id) : null; }
    function Element(id) { this._id = id; }
    function command(name, op) {
        Element.prototype[name] = function(a,b) { native(op,this._id,a,b); return this; };
    }
    function getter(name, op) {
        Element.prototype[name] = function(a) { return native(op,this._id,a); };
    }
    command('setText','text'); command('setInnerRML','rml'); command('setProperty','style');
    command('setAttribute','attr'); command('removeAttribute','removeAttr'); command('setClass','class');
    command('setValue','value'); command('show','show'); command('hide','hide'); command('close','close');
    command('remove','remove'); command('focus','focus'); command('blur','blur');
    getter('getInnerRML','getRml'); getter('getProperty','getStyle'); getter('getAttribute','getAttr');
    getter('hasClass','hasClass'); getter('getValue','getValue'); getter('getBounds','bounds');
    Element.prototype.querySelector = function(s) { return element(native('query',this._id,s)); };
    Element.prototype.getElementById = function(s) { return element(native('id',this._id,s)); };
    Element.prototype.querySelectorAll = function(s) { return native('queryAll',this._id,s).map(element); };
    Element.prototype.appendChild = function(tag) { return element(native('append',this._id,tag)); };
    Element.prototype.on = function(type,callback,capture) {
        return native('on',this._id,type,function(event) {
            event.target=element(event.target); event.currentTarget=element(event.currentTarget);
            event.stopPropagation=function() { this._stop=true; };
            var result=callback(event); return result===false || event._stop===true;
        },!!capture);
    };
    Element.prototype.off = function(token) { native('off',0,token); return this; };
    Engine.ui = {
        loadDocument:function(path) { return element(native('load',0,path)); },
        createDocument:function(rml,source) { return element(native('create',0,rml,source)); },
        loadFont:function(path,fallback) { return native('font',0,path,!!fallback); },
        off:function(token) { native('off',0,token); }
    };
})(__uiNative);
delete this.__uiNative;
)JS";
        if (duk_peval_string(c, source) != 0) {
            std::string error = duk_safe_to_stacktrace(c, -1);
            duk_pop(c);
            throw std::runtime_error(error);
        }
        duk_pop(c);
    }
    ~Impl() {
        while (!listeners.empty()) {
            auto listener = listeners.begin()->second;
            listener->element->RemoveEventListener(listener->type, listener.get(), listener->capture);
        }
        for (auto& doc : documents)
            if (doc.element)
                static_cast<Rml::ElementDocument*>(doc.element.get())->Close();
        ui.context().Update();
    }
};
UiBindings::UiBindings(duk_context* c, ui::UiCore& u, std::function<void(const std::string&)> log,
                       std::function<std::string(const std::string&)> resolve, std::function<int(int)> invoke)
    : impl_(std::make_unique<Impl>(c, u, std::move(log), std::move(resolve), std::move(invoke))) {}
UiBindings::~UiBindings() = default;
void UiBindings::setVisible(bool value) {
    impl_->setVisible(value);
}
} // namespace afterlight
