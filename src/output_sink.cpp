#include "output_sink.h"

#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace panorama_npu {
namespace {

std::mutex &registry_mutex()
{
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, SinkFactory> &registry()
{
    static std::unordered_map<std::string, SinkFactory> r;
    return r;
}

}  // namespace

std::unique_ptr<IOutputSink> create_sink(const std::string &name)
{
    std::lock_guard<std::mutex> lock(registry_mutex());
    auto it = registry().find(name);
    if (it == registry().end()) {
        std::fprintf(stderr, "output_sink: unknown sink '%s' (registered: %s)\n",
                     name.c_str(), list_registered_sinks().c_str());
        return nullptr;
    }
    return it->second();
}

int register_sink_factory(const char *name, SinkFactory factory)
{
    if (!name || !name[0] || !factory)
        return -1;
    std::lock_guard<std::mutex> lock(registry_mutex());
    if (registry().count(name) != 0) {
        std::fprintf(stderr, "output_sink: duplicate sink name '%s'\n", name);
        return -1;
    }
    registry().emplace(name, factory);
    return 0;
}

std::string list_registered_sinks()
{
    std::lock_guard<std::mutex> lock(registry_mutex());
    std::string out;
    for (const auto &kv : registry()) {
        if (!out.empty())
            out += ",";
        out += kv.first;
    }
    return out.empty() ? "(none)" : out;
}

}  // namespace panorama_npu
