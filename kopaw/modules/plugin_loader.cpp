#include "plugin_loader.hpp"

#include <dlfcn.h>

#include <cstddef>
#include <sstream>
#include <unordered_set>

#include "kop/log.h"

namespace kopaw {

static const char* kTag = "plugin";

namespace {

constexpr uint32_t kMaxPluginNodes = 4096;

void set_error(std::string* error, const std::string& message) {
    if (error) *error = message;
}

std::string dl_error() {
    const char* message = dlerror();
    return message ? message : "未知动态链接器错误";
}

bool has_plugin_size(const KopawPlugin* api) {
    return api && api->struct_size >=
                      offsetof(KopawPlugin, node_create) + sizeof(api->node_create);
}

bool has_abi_size(const KopawAbiInfo& abi) {
    return abi.struct_size >=
           offsetof(KopawAbiInfo, capabilities) + sizeof(abi.capabilities);
}

bool has_node_vtable_base(const KopawNodeVTable* vt) {
    return vt && vt->struct_size >=
                     offsetof(KopawNodeVTable, destroy) + sizeof(vt->destroy);
}

bool has_node_output_binding(const KopawNodeVTable* vt) {
    return vt && vt->struct_size >=
                     offsetof(KopawNodeVTable, bind_output) + sizeof(vt->bind_output);
}

bool validate_node_desc(const KopawNodeDesc* desc, uint32_t index, std::string* error) {
    if (!desc) {
        set_error(error, "节点描述为空: index=" + std::to_string(index));
        return false;
    }
    if (desc->struct_size < sizeof(KopawNodeDesc)) {
        set_error(error, "节点描述结构体过小: index=" + std::to_string(index) +
                             " size=" + std::to_string(desc->struct_size) +
                             " required=" + std::to_string(sizeof(KopawNodeDesc)));
        return false;
    }
    if (!desc->name || desc->name[0] == '\0') {
        set_error(error, "节点描述缺少名称: index=" + std::to_string(index));
        return false;
    }
    if (!desc->vtable) {
        set_error(error, "节点描述缺少 vtable: " + std::string(desc->name));
        return false;
    }
    const KopawNodeVTable* vt = desc->vtable;
    if (!has_node_vtable_base(vt)) {
        set_error(error, "节点 vtable 结构体过小: " + std::string(desc->name));
        return false;
    }
    if (!vt->destroy) {
        set_error(error, "节点缺少 destroy 回调: " + std::string(desc->name));
        return false;
    }
    if ((desc->self_driven || desc->inputs == 0) ? !vt->run : !vt->send) {
        set_error(error, "节点缺少必需的 run/send 回调: " + std::string(desc->name));
        return false;
    }
    if (desc->outputs > 0 &&
        (!has_node_output_binding(vt) || !vt->bind_output)) {
        set_error(error, "有输出端口的节点缺少 bind_output 回调: " +
                             std::string(desc->name));
        return false;
    }
    if (desc->queue_capacity == 0) {
        set_error(error, "节点 queue_capacity 必须大于零: " + std::string(desc->name));
        return false;
    }
    return true;
}

}  // namespace

PluginRegistry::~PluginRegistry() {
    unload_all();
}

void PluginRegistry::unload_all() {
    for (auto& p : plugins_) {
        if (p->handle) dlclose(p->handle);
        p->handle = nullptr;
    }
    plugins_.clear();
}

bool PluginRegistry::load(const std::string& path, std::string* error) {
    if (error) error->clear();
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        set_error(error, "dlopen 失败: " + dl_error());
        return false;
    }
    auto reject = [&](const std::string& message) {
        set_error(error, message);
        dlclose(h);
        return false;
    };

    dlerror();
    auto get_api = reinterpret_cast<KopawPluginGetApiFn>(
        dlsym(h, "kopaw_plugin_get_api"));
    if (!get_api) return reject("缺少 kopaw_plugin_get_api: " + dl_error());

    const KopawPlugin* api = get_api();
    if (!api) return reject("kopaw_plugin_get_api 返回空指针");
    if (!has_plugin_size(api)) {
        return reject("插件 API 结构体过小: size=" + std::to_string(api->struct_size) +
                      " required=" +
                      std::to_string(offsetof(KopawPlugin, node_create) +
                                     sizeof(api->node_create)));
    }
    if (!has_abi_size(api->abi)) {
        return reject("插件 ABI 信息结构体过小: size=" +
                      std::to_string(api->abi.struct_size));
    }

    const KopawAbiInfo host = kopaw_abi_info();
    if (!has_abi_size(host)) return reject("宿主 ABI 信息结构体无效");
    if (api->abi.major != host.major) {
        return reject("ABI 主版本不匹配: 插件 " + std::to_string(api->abi.major) +
                      " vs 宿主 " + std::to_string(host.major));
    }
    if (api->abi.minor > host.minor) {
        return reject("ABI 次版本过新: 插件 " + std::to_string(api->abi.minor) +
                      " vs 宿主 " + std::to_string(host.minor));
    }
    if ((api->abi.capabilities & ~host.capabilities) != 0) {
        std::ostringstream os;
        os << "插件需要宿主未提供的能力位: 0x" << std::hex
           << (api->abi.capabilities & ~host.capabilities);
        return reject(os.str());
    }
    if (!api->name || !api->node_count || !api->node_desc || !api->node_create) {
        return reject("插件 API 缺少必需函数指针");
    }

    auto p = std::make_unique<LoadedPlugin>();
    p->path = path;
    p->handle = h;
    p->api = *api;
    const char* plugin_name = p->api.name();
    if (!plugin_name || plugin_name[0] == '\0') {
        return reject("插件名称为空");
    }
    p->name = plugin_name;
    uint32_t n = p->api.node_count();
    if (n > kMaxPluginNodes) {
        return reject("插件节点种类数过大: " + std::to_string(n));
    }
    std::unordered_set<std::string> names;
    for (uint32_t i = 0; i < n; ++i) {
        const KopawNodeDesc* d = p->api.node_desc(i);
        std::string node_error;
        if (!validate_node_desc(d, i, &node_error)) return reject(node_error);
        if (!names.insert(d->name).second) {
            return reject("插件节点名称重复: " + std::string(d->name));
        }
        p->node_names.emplace_back(d->name);
    }
    KOP_LOG_INFO(kTag, "已加载 %s（%s，%u 节点）", path.c_str(), p->name.c_str(), n);
    plugins_.push_back(std::move(p));
    return true;
}

bool PluginRegistry::create_node(const LoadedPlugin* plugin,
                                 uint32_t index,
                                 KopawGraph* graph,
                                 KopawNodeDesc* out,
                                 std::string* error) const {
    if (error) error->clear();
    if (!plugin || !out || index >= plugin->node_names.size()) {
        set_error(error, "插件节点实例化参数无效");
        return false;
    }
    *out = KopawNodeDesc{};
    const int32_t rc = plugin->api.node_create(index, graph, out);
    if (rc != KOPAW_OK) {
        set_error(error, "插件 node_create 失败: rc=" + std::to_string(rc));
        return false;
    }
    std::string node_error;
    if (!validate_node_desc(out, index, &node_error)) {
        // A plugin that returned OK transferred a possible instance to the
        // host. Give it one best-effort destroy call before rejecting it.
        if (out->struct_size >=
                offsetof(KopawNodeDesc, vtable) + sizeof(out->vtable) &&
            out->vtable && has_node_vtable_base(out->vtable) &&
            out->vtable->destroy) {
            out->vtable->destroy(out->user_data);
        }
        set_error(error, node_error);
        return false;
    }
    return true;
}

bool PluginRegistry::bind_node_outputs(const KopawNodeDesc* node,
                                       KopawGraph* graph,
                                       uint32_t node_id,
                                       std::string* error) const {
    if (error) error->clear();
    if (!node || !graph || node_id == 0) {
        set_error(error, "插件输出绑定参数无效");
        return false;
    }
    if (node->outputs == 0) return true;

    const KopawNodeVTable* vt = node->vtable;
    if (!has_node_output_binding(vt) || !vt->bind_output) {
        set_error(error, "插件节点缺少 bind_output 回调: " +
                             std::string(node->name ? node->name : "?"));
        return false;
    }
    for (uint32_t port = 0; port < node->outputs; ++port) {
        const KopawOutput output = kopaw_graph_node_output(graph, node_id, port);
        if (output.id == 0) {
            set_error(error, "插件输出端口句柄无效: " +
                                 std::string(node->name ? node->name : "?") +
                                 " port=" + std::to_string(port));
            return false;
        }
        vt->bind_output(node->user_data, port, output);
    }
    return true;
}

const LoadedPlugin* PluginRegistry::find_node(const std::string& node_name,
                                              uint32_t* index) const {
    for (const auto& p : plugins_) {
        for (uint32_t i = 0; i < p->node_names.size(); ++i) {
            if (p->node_names[i] == node_name) {
                if (index) *index = i;
                return p.get();
            }
        }
    }
    return nullptr;
}

}  // namespace kopaw
