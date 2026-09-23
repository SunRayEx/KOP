# KOPNET：透明网络架构（远程控制支撑层）
#
# 分层（从下到上）：
#   transport/  Transport 实现（TCP/UDP/AF_UNIX/子进程管道）
#   tunnel/     隧道线协议 + TunnelSession（多路复用/分帧/credit 回压）
#   protocol/   协议解析与适配器注册表（SSH/RTC/RTP/RDP/relay）
#   nodes/      KOPAW 网络节点（option KOPNET_BUILD_NODES）
#   relay/      kopnet-relay 中继 CLI
#   remote/     KOPMS 远程会话
option(KOPNET_BUILD_NODES "构建 KOPAW 网络节点" ON)

add_library(kopnet STATIC
    endpoint.cpp
    bounded_queue.cpp
    transport/transport_factory.cpp
    transport/tcp_transport.cpp
    transport/udp_transport.cpp
    transport/unix_transport.cpp
    transport/pipe_transport.cpp
    tunnel/tunnel_protocol.cpp
    tunnel/tunnel.cpp
    resilient/resilient_session.cpp
    protocol/rtp_framing.cpp
    protocol/rtp_transport.cpp
    protocol/adapters.cpp
)
target_include_directories(kopnet PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/common/include)
target_include_directories(kopnet PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kopnet PUBLIC kop-common)

if(KOPNET_BUILD_NODES)
    add_subdirectory(nodes)
endif()

add_executable(kopnet-relay relay/kopnet_relay.cpp)
target_link_libraries(kopnet-relay PRIVATE kopnet pthread)

# KOPMS 远程会话：把 BUS2LAYER 线协议搬到隧道通道上（依赖 kopms-wire 的
# 编解码与帧校验；kopnet 核心保持协议无关）。
add_library(kopnet-remote STATIC remote/remote_session.cpp)
target_link_libraries(kopnet-remote PUBLIC kopnet kopms-wire)

if(BUILD_TESTING)
    add_executable(kopnet-endpoint-test tests/endpoint_test.cpp)
    target_link_libraries(kopnet-endpoint-test PRIVATE kopnet)
    add_test(NAME kopnet-endpoint COMMAND kopnet-endpoint-test)

    add_executable(kopnet-transport-test tests/transport_test.cpp)
    target_link_libraries(kopnet-transport-test PRIVATE kopnet)
    add_test(NAME kopnet-transport COMMAND kopnet-transport-test)

    add_executable(kopnet-tunnel-test tests/tunnel_test.cpp)
    target_link_libraries(kopnet-tunnel-test PRIVATE kopnet)
    add_test(NAME kopnet-tunnel COMMAND kopnet-tunnel-test)

    add_executable(kopnet-queue-test tests/bounded_queue_test.cpp)
    target_link_libraries(kopnet-queue-test PRIVATE kopnet pthread)
    add_test(NAME kopnet-queue COMMAND kopnet-queue-test)

    add_executable(kopnet-remote-test tests/remote_session_test.cpp)
    target_link_libraries(kopnet-remote-test PRIVATE kopnet-remote pthread)
    add_test(NAME kopnet-remote COMMAND kopnet-remote-test)

    add_executable(kopnet-resilient-test tests/resilient_session_test.cpp)
    target_link_libraries(kopnet-resilient-test PRIVATE kopnet pthread)
    add_test(NAME kopnet-resilient COMMAND kopnet-resilient-test)

    add_executable(kopnet-rtp-test tests/rtp_test.cpp)
    target_link_libraries(kopnet-rtp-test PRIVATE kopnet pthread)
    add_test(NAME kopnet-rtp COMMAND kopnet-rtp-test)
endif()
