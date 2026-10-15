#ifndef LUPE_ZSTD_DECODE_HPP_
#define LUPE_ZSTD_DECODE_HPP_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lupe::detail {

// True when the buffer opens with a Zstandard frame magic (an ordinary
// frame, or a skippable frame from 0x184D2A50..0x184D2A5F).
bool looks_like_zstd(std::string_view data);

struct ZstdDecodeResult {
  std::string text;
  std::size_t complete_frames{0};
  // True when an incomplete final frame was discarded (torn append);
  // text then holds the last checksummed frame boundary.
  bool dropped_torn_tail{false};
  // Empty on success. On failure text still holds the verified prefix
  // decoded before the error.
  std::string error;
};

// Decompresses a buffer of one or more concatenated Zstandard frames
// (deepseek-harness writes one checksummed frame per durable append
// batch). A frame's output is committed only once the frame is fully
// decoded, so its checksum has been verified; a torn final frame is
// dropped rather than surfaced as corruption.
ZstdDecodeResult
decompress_zstd_frames(std::string_view compressed, std::uint64_t max_output);

} // namespace lupe::detail

#endif // LUPE_ZSTD_DECODE_HPP_
