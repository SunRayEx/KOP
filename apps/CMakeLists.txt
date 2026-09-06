# KOPAW_Test / KOPMS_Test —— 用户级全流程测试应用
add_executable(KOPAW_Test kopaw_test.cpp)
target_link_libraries(KOPAW_Test PRIVATE
    kop::app-sdk
    kopaw_core
    pthread dl m
)

add_executable(KOPMS_Test kopms_test.cpp)
target_link_libraries(KOPMS_Test PRIVATE kop::app-sdk pthread)

# 开发者演示：媒体 → SDK 帧处理 → KOPMS 零拷贝合成
add_executable(KOP_Demo kop_demo.cpp)
target_link_libraries(KOP_Demo PRIVATE kop::app-sdk pthread)
