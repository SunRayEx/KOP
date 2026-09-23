// KOPNET 传输内部工具：UDP socket 建立（connect / 会话化 listen）。
// 仅供 kopnet 内部使用（UDP 与 RTP 适配器共用）。
#pragma once

#include <cstdint>
#include <string>

namespace kopnet {

// 创建非阻塞 + CLOEXEC 的 UDP socket。
// bind_local=true 时绑定 host:port（host 为空或 "*" 表示任意地址），
// 否则只创建 socket（供调用方自行 connect）。
// 失败返回 -1 并填充 error。
int make_udp_socket(const std::string& host, uint16_t port, bool bind_local,
                    std::string* error);

// 把已创建的 UDP socket connect 到 host:port（固定 5 元组）。
// 成功返回 0，失败返回 -1 并填充 error。
int resolve_and_connect(int fd, const std::string& host, uint16_t port,
                        std::string* error);

}  // namespace kopnet
