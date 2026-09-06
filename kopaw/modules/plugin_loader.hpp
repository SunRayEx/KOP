// 插件加载器（宿主侧）：dlopen .so → dlsym 解析 KopawPlugin → ABI 版本协商。
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "kopaw_plugin.h"

namespace kopaw {

struct LoadedPlugin {
    std::string path;
    std::string name;
    void* handle = nullptr;
    KopawPlugin api{};
    std::vector<std::string> node_names;
};

class PluginRegistry {
public:
    ~PluginRegistry();

    // 加载一个 .so；版本、能力、结构体或必需符号不合格时拒绝。
    bool load(const std::string& path, std::string* error);

    // 调用插件实例化函数，并再次校验插件返回的实例描述。
    bool create_node(const LoadedPlugin* plugin,
                     uint32_t index,
                     KopawGraph* graph,
                     KopawNodeDesc* out,
                     std::string* error) const;

    // 图节点注册后绑定实际输出句柄；每个输出端口调用一次插件回调。
    bool bind_node_outputs(const KopawNodeDesc* node,
                           KopawGraph* graph,
                           uint32_t node_id,
                           std::string* error) const;

    // 在 graph_free 完成后显式卸载所有插件。析构函数也会调用它。
    void unload_all();

    // 已加载插件列表
    const std::vector<std::unique_ptr<LoadedPlugin>>& plugins() const { return plugins_; }

    // 按节点名查找：返回 (插件指针, 节点种类下标)
    const LoadedPlugin* find_node(const std::string& node_name, uint32_t* index) const;

private:
    std::vector<std::unique_ptr<LoadedPlugin>> plugins_;
};

}  // namespace kopaw
