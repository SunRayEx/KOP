// GLFW 键码 ↔ Linux evdev 键码/字符 的共享映射表。
// 合成器输入管线（input_state.cpp）与输入反馈客户端（input_client.cpp）
// 必须使用同一张表：soak 测试按注入字符核对客户端回读字符。
#pragma once

#include <cstdint>

namespace kopms {

struct KeyPair {
    int glfw;
    uint32_t evdev;
};
constexpr KeyPair kKeyTable[] = {
    {32, 57},   // SPACE         KEY_SPACE
    {257, 28},  // ENTER         KEY_ENTER
    {258, 15},  // TAB           KEY_TAB
    {259, 14},  // BACKSPACE     KEY_BACKSPACE
    {260, 1},   // ESC           KEY_ESC
    {44, 51},   // COMMA         KEY_COMMA
    {46, 52},   // PERIOD        KEY_DOT
    {45, 12},   // MINUS         KEY_MINUS
    {61, 13},   // EQUAL         KEY_EQUAL
    {59, 39},   // SEMICOLON     KEY_SEMICOLON
    {39, 40},   // APOSTROPHE    KEY_APOSTROPHE
    {91, 26},   // LEFT_BRACKET  KEY_LEFTBRACE
    {93, 27},   // RIGHT_BRACKET KEY_RIGHTBRACE
    {92, 43},   // BACKSLASH     KEY_BACKSLASH
    {53, 53},   // SLASH         KEY_SLASH
    {96, 41},   // GRAVE         KEY_GRAVE
    {340, 42},  // LEFT_SHIFT    KEY_LEFTSHIFT
    {344, 54},  // RIGHT_SHIFT   KEY_RIGHTSHIFT
    {341, 29},  // LEFT_CONTROL  KEY_LEFTCTRL
    {345, 97},  // RIGHT_CONTROL KEY_RIGHTCTRL
    {342, 56},  // LEFT_ALT      KEY_LEFTALT
    {263, 111}, // DELETE        KEY_DELETE
};
// evdev 字母键码按 QWERTY 物理布局排布（Q=16..P=25, A=30..L=38, Z=44..M=50），
// 绝不是字母表序——必须显式成表。
constexpr uint32_t kEvdevKey1 = 2;   // KEY_1
constexpr uint32_t kEvdevKey0 = 11;  // KEY_0
constexpr uint32_t kEvdevLeftShift = 42;
constexpr uint32_t kEvdevRightShift = 54;

// GLFW_KEY_A..Z（65..90）→ evdev 键码（QWERTY 布局）
constexpr uint32_t kLetterEvdev[26] = {
    30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
    49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,
};

inline uint32_t glfw_key_to_evdev(int glfw_key) {
    if (glfw_key >= 65 && glfw_key <= 90) {
        return kLetterEvdev[glfw_key - 65];
    }
    if (glfw_key >= 48 && glfw_key <= 57) {
        const char digit = static_cast<char>(glfw_key);
        return digit == '0'
                   ? kEvdevKey0
                   : kEvdevKey1 + static_cast<uint32_t>(glfw_key - 49);
    }
    for (const KeyPair& pair : kKeyTable) {
        if (pair.glfw == glfw_key) return pair.evdev;
    }
    return 0;
}

inline char evdev_to_char(uint32_t evdev, bool shift) {
    // 与 kLetterEvdev 互逆（覆盖 soak 键入字符集）。
    for (int i = 0; i < 26; ++i) {
        if (kLetterEvdev[i] == evdev) {
            const char lower = static_cast<char>('a' + i);
            return shift ? static_cast<char>(lower - 'a' + 'A') : lower;
        }
    }
    if (evdev == 12) return shift ? '_' : '-';
    if (evdev >= kEvdevKey1 && evdev <= kEvdevKey1 + 8) {
        return static_cast<char>('1' + (evdev - kEvdevKey1));
    }
    if (evdev == kEvdevKey0) return '0';
    if (evdev == 57) return ' ';
    return 0;
}

}  // namespace kopms
