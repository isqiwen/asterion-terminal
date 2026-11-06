#include <node_api.h>
#include <sys/mman.h>

[[gnu::noinline]] napi_value native_crash_site(napi_env, napi_callback_info)
{
    const auto memory = ::mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    *static_cast<volatile int*>(memory) = 1;
    return nullptr;
}

napi_value initialize(napi_env env, napi_value exports)
{
    napi_value function;
    napi_create_function(env, "crash", NAPI_AUTO_LENGTH, native_crash_site, nullptr, &function);
    napi_set_named_property(env, exports, "crash", function);
    return exports;
}
NAPI_MODULE(NODE_GYP_MODULE_NAME, initialize)
