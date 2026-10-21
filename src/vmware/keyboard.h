// Casement's key events (PC set-1 scancodes, 0x100 added for E0 keys, Pause
// as 0x21D followed by 0x45) as RFB key events: an X11 keysym for a US
// layout, and the XT keycode QEMU extended key events carry.

#pragma once

#include <cstdint>
#include <map>
#include <vector>

namespace vmware {

struct KeyOut {
    /// The X11 keysym for a US layout and the current Shift and NumLock
    /// state; 0 for keys without one.
    std::uint32_t keysym = 0;
    /// The XT keycode as the RFB QEMU extended key event encodes it: plain
    /// codes as they are, E0 codes as 0x80 + code, Pause as 0xC6.
    std::uint32_t xt = 0;
    bool down = false;
};

/// The keysym of a scancode on a US layout, unshifted or shifted. 0 when
/// there is none.
std::uint32_t us_keysym(std::uint32_t scancode, bool shift, bool num_lock);

/// The XT keycode of a scancode, 0 if it has none.
std::uint32_t xt_keycode(std::uint32_t scancode);

/// Translates a stream of Casement key events, keeping the state that the
/// keysyms depend on. A key is released with the keysym it was pressed
/// with, even if Shift changed in between.
class Keyboard {
public:
    std::vector<KeyOut> translate(std::uint32_t scancode, bool down);

private:
    bool shift_left_ = false;
    bool shift_right_ = false;
    // Most guests start with NumLock on; the server's own state decides
    // anyway, this only picks KP_7 or KP_Home.
    bool num_lock_ = true;
    // The first half of Pause (0x21D) waiting for its 0x45.
    bool e1_pending_ = false;
    bool e1_down_ = false;
    bool pause_down_ = false;
    std::map<std::uint32_t, KeyOut> pressed_;
};

}  // namespace vmware
