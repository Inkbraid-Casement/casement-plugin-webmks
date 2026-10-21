// The RFB client messages the plugin writes, and cursor conversion.

#include "check.h"

#include <vector>

#include "vmware/rfb_wire.h"

using namespace vmware::rfb;

TEST(writes_update_requests) {
    auto m = update_request(true, 1, 2, 1024, 768);
    CHECK(m == (std::array<std::uint8_t, 10>{3, 1, 0, 1, 0, 2, 0x04, 0x00, 0x03, 0x00}));
    CHECK_EQ(update_request(false, 0, 0, 1, 1)[1], 0);
}

TEST(writes_key_events) {
    CHECK(key_event(0xff0d, true) == (std::array<std::uint8_t, 8>{4, 1, 0, 0, 0x00, 0x00, 0xff, 0x0d}));
    CHECK(key_event(0x61, false) == (std::array<std::uint8_t, 8>{4, 0, 0, 0, 0, 0, 0, 0x61}));
    CHECK(qemu_key_event(0xffe4, 0x9d, true) ==
          (std::array<std::uint8_t, 12>{255, 0, 0, 1, 0, 0, 0xff, 0xe4, 0, 0, 0, 0x9d}));
    CHECK(qemu_key_event(0, 0xc6, false) == (std::array<std::uint8_t, 12>{255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xc6}));
}

TEST(writes_pointer_and_wheel) {
    CHECK(pointer_event(5, 300, 2) == (std::array<std::uint8_t, 6>{5, 5, 0x01, 0x2c, 0, 2}));
    auto up = wheel_events(1, 1, 10, 20);
    REQUIRE(up.size() == 2);
    CHECK(up[0] == pointer_event(1 | 8, 10, 20));
    CHECK(up[1] == pointer_event(1, 10, 20));
    auto down = wheel_events(-2, 0, 0, 0);
    REQUIRE(down.size() == 4);
    CHECK_EQ(down[0][1], 16);
    CHECK_EQ(down[1][1], 0);
    CHECK_EQ(down[2][1], 16);
    CHECK(wheel_events(0, 0, 0, 0).empty());
    CHECK_EQ(wheel_events(1000, 0, 0, 0).size(), 60u);
}

TEST(converts_cursors) {
    // Two pixels, BGRX: opaque red, and a transparent one.
    std::vector<std::uint8_t> bgrx{0x00, 0x00, 0xff, 0x00, 0x10, 0x20, 0x30, 0x00};
    std::vector<std::uint8_t> mask{1, 0};
    auto rgba = cursor_rgba(bgrx.data(), mask.data(), 2, 1);
    CHECK(rgba == (std::vector<std::uint8_t>{0xff, 0x00, 0x00, 0xff, 0, 0, 0, 0}));
}
