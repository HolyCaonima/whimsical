#include "physics/dynamics/adapter/GpuService.h"
#include "physics/dynamics/adapter/ScriptBindings.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include <chrono>
using namespace whimsical;
int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            std::cerr << "Usage: dynamics_run script.js\nThe script defines update(); return false to finish.\n";
            return 1;
        }
        std::ifstream file(argv[1], std::ios::binary);
        if (!file)
            throw std::runtime_error("Cannot open script");
        std::string source((std::istreambuf_iterator<char>(file)), {});
        rc::RenderCore core({true, false, nullptr});
        auto mailbox = std::make_shared<dynamics::Mailbox>();
        dynamics::GpuService service(core, mailbox);
        std::unique_ptr<duk_context, decltype(&duk_destroy_heap)> heap(duk_create_heap_default(),
                                                                       duk_destroy_heap);
        auto c = heap.get();
        duk_push_object(c);
        duk_push_c_function(
            c,
            [](duk_context* c) -> duk_ret_t {
                std::cout << duk_safe_to_string(c, 0) << '\n';
                return 0;
            },
            1);
        duk_put_prop_string(c, -2, "log");
        duk_put_global_string(c, "Engine");
        dynamics::ScriptBindings bindings(c, mailbox);
        if (duk_peval_lstring(c, source.data(), source.size()) != 0)
            throw std::runtime_error(duk_safe_to_stacktrace(c, -1));
        duk_pop(c);
        for (;;) {
            service.drain();
            duk_get_global_string(c, "update");
            if (duk_pcall(c, 0) != 0)
                throw std::runtime_error(duk_safe_to_stacktrace(c, -1));
            bool more = duk_get_boolean_default(c, -1, true) != 0;
            duk_pop(c);
            if (!more)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (core.errors())
            throw std::runtime_error("Vulkan validation errors");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
