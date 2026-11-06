// Platform boundary for the system pasteboard, like collectors.hpp for metrics. Each supported
// OS implements these under core/src/platform/<os>/.
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace hm {

// What the pasteboard holds: UTF-8 text or image bytes. Images are stored as TIFF, because every
// macOS app accepts TIFF when pasting, and it needs no re-encode on restore.
struct ClipboardContent {
    enum class Kind { text, image };

    Kind kind = Kind::text;
    std::string text;                 // UTF-8, valid when kind is text
    std::vector<std::uint8_t> bytes;  // TIFF, valid when kind is image

    std::size_t size() const { return kind == Kind::text ? text.size() : bytes.size(); }

    // Same payload, byte for byte.
    bool operator==(const ClipboardContent &) const = default;
};

template <typename T> using Collected = std::expected<T, std::string>;

// The system-wide pasteboard change counter. It increases whenever any process, including this
// one, changes the pasteboard. Implementations must be cheap, because the watcher polls it.
std::uint64_t pasteboard_change_count();

// Reads the current pasteboard. Text is preferred when both text and an image are present. Empty
// or unsupported content is an error, which the caller treats as "nothing to record" rather than
// a failure to report.
Collected<ClipboardContent> read_pasteboard();

// Replaces the pasteboard with `content`, so the user can paste it with Cmd+V.
Collected<void> write_pasteboard(const ClipboardContent &content);

} // namespace hm
