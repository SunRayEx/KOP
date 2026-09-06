# 着色器编译与嵌入：GLSL --glslang--> SPIR-V --hex--> shaders_embed.cpp
# 由 add_custom_command 以 -P 脚本模式调用，输入以 -D 变量传入。
#
# 生成符号命名约定（vulkan_backend.cpp 引用）：
#   kopaw_vert_spv / kopaw_vert_spv_len / kopaw_frag_spv / kopaw_frag_spv_len

set(TMP_DIR "${OUT_FILE}.tmp")
file(MAKE_DIRECTORY "${TMP_DIR}")

execute_process(
    COMMAND "${GLSLANG}" -V "${SHADER_DIR}/quad.vert" -o "${TMP_DIR}/vert.spv"
    RESULT_VARIABLE rc
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(
    COMMAND "${GLSLANG}" -V "${SHADER_DIR}/quad.frag" -o "${TMP_DIR}/frag.spv"
    RESULT_VARIABLE rc
    COMMAND_ERROR_IS_FATAL ANY)

file(READ "${TMP_DIR}/vert.spv" vert_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," vert_hex "${vert_hex}")
file(READ "${TMP_DIR}/frag.spv" frag_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," frag_hex "${frag_hex}")

file(WRITE "${OUT_FILE}"
"// 自动生成：着色器 SPIR-V 嵌入（勿手改）
#include <stdint.h>
#ifdef __cplusplus
extern \"C\" {
#endif
// extern 不可省：C++ 中 const 数组默认内部链接
extern const uint8_t kopaw_vert_spv[] = {${vert_hex}};
extern const unsigned int kopaw_vert_spv_len = sizeof(kopaw_vert_spv);
extern const uint8_t kopaw_frag_spv[] = {${frag_hex}};
extern const unsigned int kopaw_frag_spv_len = sizeof(kopaw_frag_spv);
#ifdef __cplusplus
}
#endif
")

file(REMOVE_RECURSE "${TMP_DIR}")
