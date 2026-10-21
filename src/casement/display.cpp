#include "casement/display.h"

#include <algorithm>
#include <cstring>

#include "casement/log.h"
#include "casement/text.h"

namespace casement {

std::unique_ptr<Surface> Surface::open(std::uint32_t id, std::uint64_t handle, std::uint32_t width,
                                       std::uint32_t height, std::uint32_t stride, std::uint64_t size) {
    HANDLE h = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handle));
    if (stride < std::uint64_t{width} * 4 || size < std::uint64_t{stride} * height) {
        log::error("surface {}: {}x{} doesn't fit stride {} and size {}", id, width, height, stride, size);
        CloseHandle(h);
        return nullptr;
    }
    void* view = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, static_cast<SIZE_T>(size));
    if (!view) {
        log::error("can't map surface {}: {}", id, system_message(GetLastError()));
        CloseHandle(h);
        return nullptr;
    }
    std::unique_ptr<Surface> s(new Surface());
    s->id = id;
    s->width = width;
    s->height = height;
    s->stride = stride;
    s->pixels = static_cast<std::uint8_t*>(view);
    s->handle_ = h;
    return s;
}

Surface::~Surface() {
    if (pixels) UnmapViewOfFile(pixels);
    if (handle_) CloseHandle(handle_);
}

void Display::set_size(std::uint32_t width, std::uint32_t height) {
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    back_.assign(std::size_t{width} * height * 4, 0);
    full_ = true;
    rects_.clear();
    requested_ = true;
    out_.send(surface_request(width, height));
}

void Display::damage(std::int64_t x, std::int64_t y, std::int64_t w, std::int64_t h) {
    std::int64_t x0 = std::clamp<std::int64_t>(x, 0, width_);
    std::int64_t y0 = std::clamp<std::int64_t>(y, 0, height_);
    std::int64_t x1 = std::clamp<std::int64_t>(x + w, 0, width_);
    std::int64_t y1 = std::clamp<std::int64_t>(y + h, 0, height_);
    if (full_ || x1 <= x0 || y1 <= y0) return;
    if (x0 == 0 && y0 == 0 && x1 == width_ && y1 == height_) {
        damage_all();
        return;
    }
    rects_.push_back({static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0),
                      static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)});
    if (rects_.size() > max_rects_) {
        auto b = rects_[0];
        for (const auto& r : rects_) {
            std::uint32_t bx0 = std::min(b[0], r[0]), by0 = std::min(b[1], r[1]);
            std::uint32_t bx1 = std::max(b[0] + b[2], r[0] + r[2]), by1 = std::max(b[1] + b[3], r[1] + r[3]);
            b = {bx0, by0, bx1 - bx0, by1 - by0};
        }
        rects_.assign(1, b);
    }
}

void Display::damage_all() {
    full_ = true;
    rects_.clear();
}

void Display::handle(const json& msg) {
    std::string type = get_string(msg, "type");
    if (type == "surface") {
        // Unmap and close the old surface first.
        surface_.reset();
        in_flight_ = false;
        auto id = static_cast<std::uint32_t>(get_int(msg, "id"));
        auto width = static_cast<std::uint32_t>(get_int(msg, "width"));
        auto height = static_cast<std::uint32_t>(get_int(msg, "height"));
        surface_ = Surface::open(id, static_cast<std::uint64_t>(get_int(msg, "handle")), width, height,
                                 static_cast<std::uint32_t>(get_int(msg, "stride")),
                                 static_cast<std::uint64_t>(get_int(msg, "size")));
        if (!surface_) return;
        log::debug("surface {}: {}x{}", id, width, height);
        if (width != width_ || height != height_)
            log::warn("surface {} is {}x{}, the screen {}x{}", id, width, height, width_, height_);
        requested_ = false;
        damage_all();
        flush();
    } else if (type == "frame_ack") {
        if (surface_ && static_cast<std::uint32_t>(get_int(msg, "surface")) == surface_->id) {
            in_flight_ = false;
            flush();
        }
    }
}

void Display::flush() {
    if (in_flight_ || requested_ || !surface_ || (!full_ && rects_.empty())) return;
    std::uint32_t w = std::min(width_, surface_->width);
    std::uint32_t h = std::min(height_, surface_->height);
    std::vector<std::array<std::uint32_t, 4>> rects;
    if (full_) rects.push_back({0, 0, w, h});
    else rects.swap(rects_);
    json sent = json::array();
    for (auto [x, y, rw, rh] : rects) {
        if (x >= w || y >= h) continue;
        rw = std::min(rw, w - x);
        rh = std::min(rh, h - y);
        std::size_t bytes = std::size_t{rw} * 4;
        for (std::uint32_t row = y; row < y + rh; ++row) {
            std::memcpy(surface_->pixels + std::size_t{row} * surface_->stride + std::size_t{x} * 4,
                        back_.data() + std::size_t{row} * stride() + std::size_t{x} * 4, bytes);
        }
        sent.push_back({x, y, rw, rh});
    }
    bool whole = full_;
    full_ = false;
    rects_.clear();
    if (sent.empty()) return;
    in_flight_ = true;
    out_.send(json{{"type", "frame"}, {"surface", surface_->id}, {"rects", whole ? json::array() : sent}});
}

}  // namespace casement
