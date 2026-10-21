// The remote picture and its way into Casement's surfaces.
//
// Sessions draw into a private BGRX back buffer and mark what changed. The
// display copies the changed rectangles into the current surface and sends
// `frame` whenever no frame is in flight, so drawing never waits for
// Casement and Casement always gets the latest picture.

#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "casement/protocol.h"

namespace casement {

/// A surface Casement shared with the plugin: a file mapping of BGRX
/// pixels, `stride` bytes per row.
class Surface {
public:
    /// Maps the surface. Errors are logged and give an empty pointer.
    static std::unique_ptr<Surface> open(std::uint32_t id, std::uint64_t handle, std::uint32_t width,
                                         std::uint32_t height, std::uint32_t stride, std::uint64_t size);
    ~Surface();
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    std::uint32_t id = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::uint8_t* pixels = nullptr;

private:
    Surface() = default;
    HANDLE handle_ = nullptr;
};

class Display {
public:
    /// More changed rectangles than `max_rects` are merged into their
    /// bounding box.
    explicit Display(Output& out, std::size_t max_rects = 16) : out_(out), max_rects_(max_rects) {}

    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }

    /// Sets the remote screen's size, asking Casement for a matching
    /// surface when it changes. The back buffer is cleared to black.
    void set_size(std::uint32_t width, std::uint32_t height);

    /// The back buffer: BGRX, `stride()` bytes per row.
    std::uint8_t* buffer() { return back_.data(); }
    std::size_t stride() const { return std::size_t{width_} * 4; }

    /// Marks a rectangle as changed. It is clipped to the screen.
    void damage(std::int64_t x, std::int64_t y, std::int64_t w, std::int64_t h);
    void damage_all();

    /// Handles `surface` and `frame_ack`; other messages are ignored.
    void handle(const json& msg);

    /// Copies what changed into the surface and sends `frame`, if a surface
    /// is free.
    void flush();

    /// Whether a surface is mapped and no frame is in flight.
    bool idle() const { return surface_ && !in_flight_ && !requested_; }

private:
    Output& out_;
    std::size_t max_rects_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::vector<std::uint8_t> back_;
    std::unique_ptr<Surface> surface_;
    /// A `surface_request` is outstanding: surfaces arriving meanwhile may be
    /// for an older size, so nothing is drawn until the answer comes.
    bool requested_ = false;
    bool in_flight_ = false;
    bool full_ = false;
    std::vector<std::array<std::uint32_t, 4>> rects_;
};

/// The display with its lock held, from `Host::display()`.
class LockedDisplay {
public:
    LockedDisplay(std::mutex& m, Display& d) : lock_(m), display_(d) {}
    Display* operator->() { return &display_; }
    Display& operator*() { return display_; }

private:
    std::unique_lock<std::mutex> lock_;
    Display& display_;
};

}  // namespace casement
