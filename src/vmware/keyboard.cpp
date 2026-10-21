#include "vmware/keyboard.h"

#include <algorithm>
#include <array>

namespace vmware {

namespace {

struct Key {
    std::uint16_t scancode;
    std::uint32_t normal;
    std::uint32_t shifted;  // 0: same as normal
};

// A US layout (X11 "us"). Letters and digits are their ASCII keysyms.
constexpr Key keys[] = {
    {0x01, 0xff1b, 0},      // Escape
    {0x02, '1', '!'},       {0x03, '2', '@'},      {0x04, '3', '#'},     {0x05, '4', '$'},
    {0x06, '5', '%'},       {0x07, '6', '^'},      {0x08, '7', '&'},     {0x09, '8', '*'},
    {0x0a, '9', '('},       {0x0b, '0', ')'},      {0x0c, '-', '_'},     {0x0d, '=', '+'},
    {0x0e, 0xff08, 0},      // BackSpace
    {0x0f, 0xff09, 0},      // Tab
    {0x10, 'q', 'Q'},       {0x11, 'w', 'W'},      {0x12, 'e', 'E'},     {0x13, 'r', 'R'},
    {0x14, 't', 'T'},       {0x15, 'y', 'Y'},      {0x16, 'u', 'U'},     {0x17, 'i', 'I'},
    {0x18, 'o', 'O'},       {0x19, 'p', 'P'},      {0x1a, '[', '{'},     {0x1b, ']', '}'},
    {0x1c, 0xff0d, 0},      // Return
    {0x1d, 0xffe3, 0},      // Control_L
    {0x1e, 'a', 'A'},       {0x1f, 's', 'S'},      {0x20, 'd', 'D'},     {0x21, 'f', 'F'},
    {0x22, 'g', 'G'},       {0x23, 'h', 'H'},      {0x24, 'j', 'J'},     {0x25, 'k', 'K'},
    {0x26, 'l', 'L'},       {0x27, ';', ':'},      {0x28, '\'', '"'},    {0x29, '`', '~'},
    {0x2a, 0xffe1, 0},      // Shift_L
    {0x2b, '\\', '|'},
    {0x2c, 'z', 'Z'},       {0x2d, 'x', 'X'},      {0x2e, 'c', 'C'},     {0x2f, 'v', 'V'},
    {0x30, 'b', 'B'},       {0x31, 'n', 'N'},      {0x32, 'm', 'M'},     {0x33, ',', '<'},
    {0x34, '.', '>'},       {0x35, '/', '?'},
    {0x36, 0xffe2, 0},      // Shift_R
    {0x37, 0xffaa, 0},      // KP_Multiply
    {0x38, 0xffe9, 0},      // Alt_L
    {0x39, ' ', 0},
    {0x3a, 0xffe5, 0},      // Caps_Lock
    {0x3b, 0xffbe, 0},      {0x3c, 0xffbf, 0},     {0x3d, 0xffc0, 0},    {0x3e, 0xffc1, 0},  // F1-F4
    {0x3f, 0xffc2, 0},      {0x40, 0xffc3, 0},     {0x41, 0xffc4, 0},    {0x42, 0xffc5, 0},  // F5-F8
    {0x43, 0xffc6, 0},      {0x44, 0xffc7, 0},                                               // F9-F10
    {0x45, 0xff7f, 0},      // Num_Lock
    {0x46, 0xff14, 0},      // Scroll_Lock
    {0x4a, 0xffad, 0},      // KP_Subtract
    {0x4e, 0xffab, 0},      // KP_Add
    {0x54, 0xff15, 0},      // Sys_Req (Alt+Print)
    {0x56, '<', '>'},       // the key left of Z on ISO keyboards
    {0x57, 0xffc8, 0},      {0x58, 0xffc9, 0},     // F11, F12
    {0x64, 0xffca, 0},      {0x65, 0xffcb, 0},     {0x66, 0xffcc, 0},    {0x67, 0xffcd, 0},  // F13-F16
    {0x68, 0xffce, 0},      {0x69, 0xffcf, 0},     {0x6a, 0xffd0, 0},    {0x6b, 0xffd1, 0},  // F17-F20
    {0x6c, 0xffd2, 0},      {0x6d, 0xffd3, 0},     {0x6e, 0xffd4, 0},    {0x76, 0xffd5, 0},  // F21-F24
    {0x70, 0xff27, 0},      // Hiragana_Katakana
    {0x73, '\\', '_'},      // JIS Ro
    {0x79, 0xff23, 0},      // Henkan
    {0x7b, 0xff22, 0},      // Muhenkan
    {0x7d, 0x00a5, '|'},    // JIS Yen
    {0x110, 0x1008ff16, 0}, // XF86AudioPrev
    {0x119, 0x1008ff17, 0}, // XF86AudioNext
    {0x11c, 0xff8d, 0},     // KP_Enter
    {0x11d, 0xffe4, 0},     // Control_R
    {0x120, 0x1008ff12, 0}, // XF86AudioMute
    {0x121, 0x1008ff1d, 0}, // XF86Calculator
    {0x122, 0x1008ff14, 0}, // XF86AudioPlay
    {0x124, 0x1008ff15, 0}, // XF86AudioStop
    {0x12e, 0x1008ff11, 0}, // XF86AudioLowerVolume
    {0x130, 0x1008ff13, 0}, // XF86AudioRaiseVolume
    {0x132, 0x1008ff18, 0}, // XF86HomePage
    {0x135, 0xffaf, 0},     // KP_Divide
    {0x137, 0xff61, 0},     // Print
    {0x138, 0xffea, 0},     // Alt_R
    {0x145, 0xff7f, 0},     // Num_Lock, as Windows reports it
    {0x146, 0xff6b, 0},     // Break (Ctrl+Pause)
    {0x147, 0xff50, 0},     // Home
    {0x148, 0xff52, 0},     // Up
    {0x149, 0xff55, 0},     // Prior
    {0x14b, 0xff51, 0},     // Left
    {0x14d, 0xff53, 0},     // Right
    {0x14f, 0xff57, 0},     // End
    {0x150, 0xff54, 0},     // Down
    {0x151, 0xff56, 0},     // Next
    {0x152, 0xff63, 0},     // Insert
    {0x153, 0xffff, 0},     // Delete
    {0x15b, 0xffeb, 0},     // Super_L
    {0x15c, 0xffec, 0},     // Super_R
    {0x15d, 0xff67, 0},     // Menu
    {0x15e, 0x1008ff2a, 0}, // XF86PowerOff
    {0x15f, 0x1008ff2f, 0}, // XF86Sleep
    {0x163, 0x1008ff2b, 0}, // XF86WakeUp
    {0x165, 0x1008ff1b, 0}, // XF86Search
    {0x166, 0x1008ff30, 0}, // XF86Favorites
    {0x167, 0x1008ff29, 0}, // XF86Refresh
    {0x168, 0x1008ff28, 0}, // XF86Stop
    {0x169, 0x1008ff27, 0}, // XF86Forward
    {0x16a, 0x1008ff26, 0}, // XF86Back
    {0x16b, 0x1008ff33, 0}, // XF86MyComputer
    {0x16c, 0x1008ff19, 0}, // XF86Mail
};

// The keypad's dual keys: with NumLock on, and off.
struct PadKey {
    std::uint16_t scancode;
    std::uint32_t num;
    std::uint32_t nav;
};

constexpr PadKey pad_keys[] = {
    {0x47, 0xffb7, 0xff95},  // KP_7 / KP_Home
    {0x48, 0xffb8, 0xff97},  // KP_8 / KP_Up
    {0x49, 0xffb9, 0xff9a},  // KP_9 / KP_Prior
    {0x4b, 0xffb4, 0xff96},  // KP_4 / KP_Left
    {0x4c, 0xffb5, 0xff9d},  // KP_5 / KP_Begin
    {0x4d, 0xffb6, 0xff98},  // KP_6 / KP_Right
    {0x4f, 0xffb1, 0xff9c},  // KP_1 / KP_End
    {0x50, 0xffb2, 0xff99},  // KP_2 / KP_Down
    {0x51, 0xffb3, 0xff9b},  // KP_3 / KP_Next
    {0x52, 0xffb0, 0xff9e},  // KP_0 / KP_Insert
    {0x53, 0xffae, 0xff9f},  // KP_Decimal / KP_Delete
};

constexpr std::uint32_t pause_scancode = 0x21d;
constexpr std::uint32_t pause_keysym = 0xff13;
constexpr std::uint32_t pause_xt = 0xc6;

}  // namespace

std::uint32_t us_keysym(std::uint32_t scancode, bool shift, bool num_lock) {
    for (const PadKey& k : pad_keys)
        if (k.scancode == scancode) return num_lock ? k.num : k.nav;
    for (const Key& k : keys)
        if (k.scancode == scancode) return shift && k.shifted ? k.shifted : k.normal;
    return 0;
}

std::uint32_t xt_keycode(std::uint32_t scancode) {
    // Windows reports NumLock with the E0 flag; on the wire it is plain.
    if (scancode == 0x145) return 0x45;
    if (scancode >= 0x01 && scancode <= 0x7f) return scancode;
    if (scancode >= 0x101 && scancode <= 0x17f) return 0x80 | (scancode & 0x7f);
    return 0;
}

std::vector<KeyOut> Keyboard::translate(std::uint32_t scancode, bool down) {
    // Pause arrives as 0x21D (E1 1D) followed by 0x45. Keep the first half
    // until the second arrives; anything else in between drops it.
    if (scancode == pause_scancode) {
        e1_pending_ = true;
        e1_down_ = down;
        return {};
    }
    // A release may also come as a lone 0x45 while Pause is held.
    bool pause = scancode == 0x45 && ((e1_pending_ && e1_down_ == down) || (!down && pause_down_));
    e1_pending_ = false;
    if (pause) {
        if (!down && !pause_down_) return {};
        pause_down_ = down;
        return {KeyOut{pause_keysym, pause_xt, down}};
    }

    if (!down) {
        auto it = pressed_.find(scancode);
        KeyOut out;
        if (it != pressed_.end()) {
            out = it->second;
            pressed_.erase(it);
        } else {
            out = KeyOut{us_keysym(scancode, shift_left_ || shift_right_, num_lock_), xt_keycode(scancode), false};
        }
        out.down = false;
        if (scancode == 0x2a) shift_left_ = false;
        if (scancode == 0x36) shift_right_ = false;
        if (out.keysym == 0 && out.xt == 0) return {};
        return {out};
    }

    // Auto-repeat: the same keysym as the first press.
    if (auto it = pressed_.find(scancode); it != pressed_.end()) return {it->second};

    KeyOut out{us_keysym(scancode, shift_left_ || shift_right_, num_lock_), xt_keycode(scancode), true};
    if (scancode == 0x2a) shift_left_ = true;
    if (scancode == 0x36) shift_right_ = true;
    if (scancode == 0x45 || scancode == 0x145) num_lock_ = !num_lock_;
    if (out.keysym == 0 && out.xt == 0) return {};
    pressed_[scancode] = out;
    return {out};
}

}  // namespace vmware
