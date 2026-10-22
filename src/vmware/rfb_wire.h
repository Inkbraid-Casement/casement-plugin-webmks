// The RFB client messages the plugin writes itself (input and update
// requests), and the conversion of RFB cursor shapes to Casement's RGBA.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace vmware::rfb {

/// FramebufferUpdateRequest (type 3).
std::array<std::uint8_t, 10> update_request(bool incremental, std::uint16_t x, std::uint16_t y, std::uint16_t w,
                                            std::uint16_t h);

/// KeyEvent (type 4).
std::array<std::uint8_t, 8> key_event(std::uint32_t keysym, bool down);

/// QEMU extended key event (type 255, subtype 0).
std::array<std::uint8_t, 12> qemu_key_event(std::uint32_t keysym, std::uint32_t xt, bool down);

/// PointerEvent (type 5). RFB's button bits match Casement's: left 1,
/// middle 2, right 4; the wheel is buttons 4 (8, up) and 5 (16, down).
std::array<std::uint8_t, 6> pointer_event(std::uint8_t buttons, std::uint16_t x, std::uint16_t y);

/// The pointer events for wheel notches at (x, y) with `buttons` held: a
/// press and release of button 4 (away from the user) or 5 per notch.
std::vector<std::array<std::uint8_t, 6>> wheel_events(int delta, std::uint8_t buttons, std::uint16_t x,
                                                      std::uint16_t y);

/// A cursor shape from LibVNCClient (pixels as 32-bit little-endian BGRX,
/// one mask byte per pixel, non-zero where the cursor is opaque) as RGBA
/// with straight alpha.
std::vector<std::uint8_t> cursor_rgba(const std::uint8_t* bgrx, const std::uint8_t* mask, int width, int height);

}  // namespace vmware::rfb
