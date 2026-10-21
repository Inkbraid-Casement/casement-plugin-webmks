// Scancodes to keysyms (US layout) and to QEMU's XT keycodes.

#include "check.h"

#include "vmware/keyboard.h"

using namespace vmware;

namespace {

KeyOut one(Keyboard& k, std::uint32_t scancode, bool down) {
    auto out = k.translate(scancode, down);
    if (out.size() != 1) {
        ::check::fail(__FILE__, __LINE__, "one key event", std::to_string(out.size()));
        return {};
    }
    return out[0];
}

}  // namespace

TEST(maps_us_keysyms) {
    CHECK_EQ(us_keysym(0x1e, false, true), 0x61u);  // a
    CHECK_EQ(us_keysym(0x1e, true, true), 0x41u);   // A
    CHECK_EQ(us_keysym(0x02, true, true), 0x21u);   // !
    CHECK_EQ(us_keysym(0x29, false, true), 0x60u);  // `
    CHECK_EQ(us_keysym(0x29, true, true), 0x7eu);   // ~
    CHECK_EQ(us_keysym(0x1c, false, true), 0xff0du);
    CHECK_EQ(us_keysym(0x11c, false, true), 0xff8du);   // KP_Enter
    CHECK_EQ(us_keysym(0x11d, false, true), 0xffe4u);   // Control_R
    CHECK_EQ(us_keysym(0x138, false, true), 0xffeau);   // Alt_R
    CHECK_EQ(us_keysym(0x148, false, true), 0xff52u);   // Up
    CHECK_EQ(us_keysym(0x153, false, true), 0xffffu);   // Delete
    CHECK_EQ(us_keysym(0x15b, false, true), 0xffebu);   // Super_L
    CHECK_EQ(us_keysym(0x47, false, true), 0xffb7u);    // KP_7
    CHECK_EQ(us_keysym(0x47, false, false), 0xff95u);   // KP_Home
    CHECK_EQ(us_keysym(0x53, false, true), 0xffaeu);    // KP_Decimal
    CHECK_EQ(us_keysym(0x3b, false, true), 0xffbeu);    // F1
    CHECK_EQ(us_keysym(0x58, false, true), 0xffc9u);    // F12
    CHECK_EQ(us_keysym(0x76, false, true), 0xffd5u);    // F24
    CHECK_EQ(us_keysym(0x7f, false, true), 0u);
    CHECK_EQ(us_keysym(0x200, false, true), 0u);
}

TEST(maps_xt_keycodes) {
    CHECK_EQ(xt_keycode(0x1e), 0x1eu);
    CHECK_EQ(xt_keycode(0x11d), 0x9du);  // right Ctrl, as the RFB spec's example
    CHECK_EQ(xt_keycode(0x148), 0xc8u);  // Up
    CHECK_EQ(xt_keycode(0x15b), 0xdbu);  // left Windows key
    CHECK_EQ(xt_keycode(0x145), 0x45u);  // NumLock with Windows' E0 flag
    CHECK_EQ(xt_keycode(0x00), 0u);
    CHECK_EQ(xt_keycode(0x21d), 0u);
}

TEST(follows_shift) {
    Keyboard k;
    CHECK_EQ(one(k, 0x1e, true).keysym, 0x61u);
    CHECK_EQ(one(k, 0x1e, false).keysym, 0x61u);
    KeyOut shift = one(k, 0x2a, true);
    CHECK_EQ(shift.keysym, 0xffe1u);
    CHECK_EQ(shift.xt, 0x2au);
    KeyOut a = one(k, 0x1e, true);
    CHECK_EQ(a.keysym, 0x41u);
    CHECK_EQ(a.xt, 0x1eu);
    CHECK(a.down);
    // Shift goes up first: 'A' is still released as 'A'.
    CHECK(!one(k, 0x2a, false).down);
    KeyOut up = one(k, 0x1e, false);
    CHECK_EQ(up.keysym, 0x41u);
    CHECK(!up.down);
    CHECK_EQ(one(k, 0x1e, true).keysym, 0x61u);
    CHECK_EQ(one(k, 0x1e, false).keysym, 0x61u);
    // Right Shift too, and auto-repeat keeps the first keysym.
    one(k, 0x36, true);
    CHECK_EQ(one(k, 0x03, true).keysym, 0x40u);  // @
    one(k, 0x36, false);
    CHECK_EQ(one(k, 0x03, true).keysym, 0x40u);
    CHECK_EQ(one(k, 0x03, false).keysym, 0x40u);
}

TEST(follows_num_lock) {
    Keyboard k;
    CHECK_EQ(one(k, 0x4f, true).keysym, 0xffb1u);  // KP_1
    one(k, 0x4f, false);
    one(k, 0x45, true);  // NumLock off
    one(k, 0x45, false);
    CHECK_EQ(one(k, 0x4f, true).keysym, 0xff9cu);  // KP_End
    one(k, 0x4f, false);
    KeyOut num = one(k, 0x145, true);  // and on again, as Windows sends it
    CHECK_EQ(num.keysym, 0xff7fu);
    CHECK_EQ(num.xt, 0x45u);
    one(k, 0x145, false);
    CHECK_EQ(one(k, 0x4f, true).keysym, 0xffb1u);
}

TEST(joins_pause) {
    Keyboard k;
    CHECK(k.translate(0x21d, true).empty());
    KeyOut down = one(k, 0x45, true);
    CHECK_EQ(down.keysym, 0xff13u);
    CHECK_EQ(down.xt, 0xc6u);
    CHECK(down.down);
    CHECK(k.translate(0x21d, false).empty());
    KeyOut up = one(k, 0x45, false);
    CHECK_EQ(up.xt, 0xc6u);
    CHECK(!up.down);
    // Released as a lone 0x45.
    k.translate(0x21d, true);
    one(k, 0x45, true);
    KeyOut lone = one(k, 0x45, false);
    CHECK_EQ(lone.keysym, 0xff13u);
    CHECK(!lone.down);
    k.translate(0x21d, false);
    // NumLock itself is still NumLock.
    KeyOut num = one(k, 0x45, true);
    CHECK_EQ(num.keysym, 0xff7fu);
    CHECK_EQ(num.xt, 0x45u);
}

TEST(drops_unknown_keys) {
    Keyboard k;
    CHECK(k.translate(0x2ff, true).empty());
    CHECK(k.translate(0x2ff, false).empty());
    // An E0 key without a keysym still has its XT code.
    auto out = k.translate(0x17e, true);
    REQUIRE(out.size() == 1);
    CHECK_EQ(out[0].keysym, 0u);
    CHECK_EQ(out[0].xt, 0xfeu);
}

TEST(ctrl_alt_del_is_three_keys) {
    Keyboard k;
    CHECK_EQ(one(k, 0x1d, true).keysym, 0xffe3u);
    CHECK_EQ(one(k, 0x38, true).keysym, 0xffe9u);
    KeyOut del = one(k, 0x153, true);
    CHECK_EQ(del.keysym, 0xffffu);
    CHECK_EQ(del.xt, 0xd3u);
}
