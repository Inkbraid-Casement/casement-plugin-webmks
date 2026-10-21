#include "casement/text.h"

#include <windows.h>

namespace casement {

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view utf16) {
    if (utf16.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

namespace {
constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}
}  // namespace

std::string base64_encode(std::span<const std::uint8_t> data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        std::uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += alphabet[v & 63];
    }
    if (std::size_t rest = data.size() - i; rest > 0) {
        std::uint32_t v = data[i] << 16;
        if (rest == 2) v |= data[i + 1] << 8;
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += rest == 2 ? alphabet[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view text) {
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
        int v = value(c);
        if (v < 0) return std::nullopt;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

std::string hex(std::span<const std::uint8_t> data, std::string_view separator) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (i > 0) out += separator;
        out += digits[data[i] >> 4];
        out += digits[data[i] & 15];
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> parse_hex(std::string_view text) {
    std::vector<std::uint8_t> out;
    int high = -1;
    for (char c : text) {
        if (c == ':' || c == ' ') continue;
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return std::nullopt;
        if (high < 0) {
            high = v;
        } else {
            out.push_back(static_cast<std::uint8_t>(high << 4 | v));
            high = -1;
        }
    }
    if (high >= 0) return std::nullopt;
    return out;
}

std::string system_message(unsigned long code) {
    wchar_t* buffer = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string text;
    if (n && buffer) {
        text = narrow(std::wstring_view(buffer, n));
        LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == '.' ||
                             text.back() == ' '))
        text.pop_back();
    if (text.empty()) text = "error " + std::to_string(code);
    return text;
}

}  // namespace casement
