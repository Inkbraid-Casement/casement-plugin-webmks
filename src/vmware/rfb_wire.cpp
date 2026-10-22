#include "vmware/rfb_wire.h"

#include <cstdlib>

namespace vmware::rfb {

namespace {

void put16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}

void put32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

// More notches than this in one wheel message are a runaway input device.
constexpr int max_notches = 30;

}  // namespace

std::array<std::uint8_t, 10> update_request(bool incremental, std::uint16_t x, std::uint16_t y, std::uint16_t w,
                                            std::uint16_t h) {
    std::array<std::uint8_t, 10> m{3, static_cast<std::uint8_t>(incremental ? 1 : 0)};
    put16(&m[2], x);
    put16(&m[4], y);
    put16(&m[6], w);
    put16(&m[8], h);
    return m;
}

std::array<std::uint8_t, 8> key_event(std::uint32_t keysym, bool down) {
    std::array<std::uint8_t, 8> m{4, static_cast<std::uint8_t>(down ? 1 : 0)};
    put32(&m[4], keysym);
    return m;
}

std::array<std::uint8_t, 12> qemu_key_event(std::uint32_t keysym, std::uint32_t xt, bool down) {
    std::array<std::uint8_t, 12> m{255, 0};
    put16(&m[2], static_cast<std::uint16_t>(down ? 1 : 0));
    put32(&m[4], keysym);
    put32(&m[8], xt);
    return m;
}

std::array<std::uint8_t, 6> pointer_event(std::uint8_t buttons, std::uint16_t x, std::uint16_t y) {
    std::array<std::uint8_t, 6> m{5, buttons};
    put16(&m[2], x);
    put16(&m[4], y);
    return m;
}

std::vector<std::array<std::uint8_t, 6>> wheel_events(int delta, std::uint8_t buttons, std::uint16_t x,
                                                      std::uint16_t y) {
    std::vector<std::array<std::uint8_t, 6>> out;
    int notches = std::abs(delta);
    if (notches > max_notches) notches = max_notches;
    auto wheel = static_cast<std::uint8_t>(delta > 0 ? 8 : 16);
    for (int i = 0; i < notches; ++i) {
        out.push_back(pointer_event(static_cast<std::uint8_t>(buttons | wheel), x, y));
        out.push_back(pointer_event(buttons, x, y));
    }
    return out;
}

std::vector<std::uint8_t> cursor_rgba(const std::uint8_t* bgrx, const std::uint8_t* mask, int width, int height) {
    std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<std::uint8_t> rgba(n * 4);
    for (std::size_t i = 0; i < n; ++i) {
        bool opaque = mask[i] != 0;
        rgba[i * 4] = opaque ? bgrx[i * 4 + 2] : 0;
        rgba[i * 4 + 1] = opaque ? bgrx[i * 4 + 1] : 0;
        rgba[i * 4 + 2] = opaque ? bgrx[i * 4] : 0;
        rgba[i * 4 + 3] = opaque ? 255 : 0;
    }
    return rgba;
}

}  // namespace vmware::rfb
