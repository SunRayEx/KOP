# KOPNET → KOPAW 网络节点（net-source / net-sink）
#
# 节点实现只依赖 KOPAW 的 C ABI 头与 frame.hpp（头文件），图引擎符号
# （kopaw_graph_*）由最终可执行文件链接 kopaw_core 提供，本库不直接链接。
add_library(kopnet-nodes STATIC net_nodes.cpp frame_envelope.cpp)
target_include_directories(kopnet-nodes PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/kopaw/modules
    ${CMAKE_SOURCE_DIR}/kopaw/abi/include
    ${CMAKE_SOURCE_DIR}/common/include)
target_link_libraries(kopnet-nodes PUBLIC kopnet kop-common)

if(BUILD_TESTING)
    add_executable(kopnet-node-test ${CMAKE_CURRENT_SOURCE_DIR}/../tests/net_node_test.cpp)
    target_include_directories(kopnet-node-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/kopaw/modules
        ${CMAKE_SOURCE_DIR}/kopaw/abi/include)
    target_link_libraries(kopnet-node-test PRIVATE
        kopnet-nodes
        kopaw_core
        pthread dl m)
    add_test(NAME kopnet-node COMMAND kopnet-node-test)
endif()
