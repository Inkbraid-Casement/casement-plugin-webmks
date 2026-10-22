// Text helpers: UTF-8 and UTF-16 (Windows APIs), base64, hex.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace casement {

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view utf16);

std::string base64_encode(std::span<const std::uint8_t> data);
std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view text);

/// Lower-case hex, optionally with a separator between bytes ("ab:cd").
std::string hex(std::span<const std::uint8_t> data, std::string_view separator = "");

/// Parses hex digits, ignoring `:` and spaces. Empty optional when the text
/// has anything else or an odd number of digits.
std::optional<std::vector<std::uint8_t>> parse_hex(std::string_view text);

/// The message of a Windows error code, without the trailing line break.
std::string system_message(unsigned long code);

}  // namespace casement
