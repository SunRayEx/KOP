// KOPAW 插件 ABI（P2.1）：宿主（player/未来的 KOPMS）与 .so 节点包之间的契约。
//
// 插件 = 一个 .so，必须导出 kopaw_plugin_get_api()。它返回一个长期有效的
// KopawPlugin 表，表内携带 ABI 版本/能力位和节点枚举、描述、实例化回调。
// 宿主只读取 struct_size 覆盖的字段，并在调用任何节点回调前完成协商。
// 节点若声明 outputs > 0，必须提供 KopawNodeVTable::bind_output；宿主在图注册
// 后为每个端口绑定 KopawOutput，节点之后才能调用 kopaw_graph_emit()。
// 插件读取媒体内容时必须先检查 KopawFrame::memory_type：CPU 模式下
// dma_buf_handle 只能在当前进程内还原为地址别名，DMA-BUF 模式下应使用本地
// 平面 FD/驱动导入路径；绝不能把句柄数字跨进程复用。
// 兼容性入口 kopaw_plugin_abi_version/name/node_* 仍由宏导出，便于旧工具
// 诊断，但不是新加载器的必需符号。
// 插件编译：-fPIC -shared，链接（或弱引用）宿主导出的 kopaw_graph_* 符号。
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "kopaw_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KOPAW_PLUGIN_API_VERSION KOPAW_ABI_VERSION

/*
 * A plugin exposes one immutable API table.  struct_size lets a newer host
 * accept an older minor-version table without reading fields past its end.
 * The ABI info's capabilities field is a required-capability mask for the
 * plugin and a provided-capability mask for the host.
 */
typedef struct KopawPlugin {
    uint32_t struct_size;
    KopawAbiInfo abi;
    const char* (*name)(void);
    uint32_t (*node_count)(void);
    const KopawNodeDesc* (*node_desc)(uint32_t index);
    int32_t (*node_create)(uint32_t index, KopawGraph* g, KopawNodeDesc* out);
} KopawPlugin;

typedef const KopawPlugin* (*KopawPluginGetApiFn)(void);

#define KOPAW_PLUGIN_REQUIRED_CAPABILITIES KOPAW_ABI_CAPABILITIES

/* 便捷声明宏：插件实现侧使用（自带 extern "C" 包裹，防止 C++ 改名）。 */
#ifdef __cplusplus
#define KOPAW_PLUGIN_EXTERN_C extern "C"
#else
#define KOPAW_PLUGIN_EXTERN_C
#endif

#define KOPAW_PLUGIN_EXPORTS(unused_name, plugin_name_str, node_count_expr,            \
                             desc_fn, create_fn)                                       \
    KOPAW_PLUGIN_EXTERN_C {                                                            \
    uint32_t kopaw_plugin_abi_version(void) { return KOPAW_PLUGIN_API_VERSION; }       \
    const char* kopaw_plugin_name(void) { return plugin_name_str; }                    \
    uint32_t kopaw_plugin_node_count(void) { return (node_count_expr); }               \
    const KopawNodeDesc* kopaw_plugin_node_desc(uint32_t i) { return desc_fn(i); }     \
    int32_t kopaw_plugin_node_create(uint32_t i, KopawGraph* g, KopawNodeDesc* out) {  \
        (void)(unused_name);                                                           \
        return create_fn(i, g, out);                                                   \
    }                                                                                  \
    const KopawPlugin* kopaw_plugin_get_api(void) {                                    \
        static const KopawPlugin api = {                                               \
            sizeof(KopawPlugin),                                                       \
            {sizeof(KopawAbiInfo), KOPAW_ABI_MAJOR, KOPAW_ABI_MINOR,                  \
             KOPAW_PLUGIN_REQUIRED_CAPABILITIES},                                     \
            kopaw_plugin_name, kopaw_plugin_node_count, kopaw_plugin_node_desc,        \
            kopaw_plugin_node_create};                                                \
        return &api;                                                                   \
    }                                                                                  \
    }

#ifdef __cplusplus
}
#endif
