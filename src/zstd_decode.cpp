#include "zstd_decode.hpp"

#include <zstd.h>

#include <memory>
#include <utility>
#include <vector>

namespace lupe::detail {
namespace {

constexpr std::uint32_t kZstdMagic = 0xFD2FB528U;
constexpr std::uint32_t kSkippableMagicStart = 0x184D2A50U;
constexpr std::uint32_t kSkippableMagicMask = 0xFFFFFFF0U;

std::uint32_t load_le32(const char *bytes) {
  const auto byte_at = [](const char *pointer, std::size_t index) {
    return static_cast<std::uint32_t>(
        static_cast<unsigned char>(pointer[index]));
  };
  return byte_at(bytes, 0)
       | (byte_at(bytes, 1) << 8U)
       | (byte_at(bytes, 2) << 16U)
       | (byte_at(bytes, 3) << 24U);
}

struct FreeDStream {
  void operator()(ZSTD_DStream *stream) const noexcept {
    static_cast<void>(ZSTD_freeDStream(stream));
  }
};

} // namespace

bool looks_like_zstd(std::string_view data) {
  if (data.size() < 4) {
    return false;
  }
  const std::uint32_t magic = load_le32(data.data());
  return magic == kZstdMagic
      || (magic & kSkippableMagicMask) == kSkippableMagicStart;
}

ZstdDecodeResult
decompress_zstd_frames(std::string_view compressed, std::uint64_t max_output) {
  ZstdDecodeResult result;
  if (!looks_like_zstd(compressed)) {
    result.error = "input does not begin with a zstd frame";
    return result;
  }

  std::unique_ptr<ZSTD_DStream, FreeDStream> stream{ZSTD_createDStream()};
  if (!stream) {
    result.error = "failed to allocate the zstd decoder";
    return result;
  }
  if (const std::size_t init = ZSTD_initDStream(stream.get());
      ZSTD_isError(init)) {
    result.error =
        std::string{"zstd decoder init failed: "} + ZSTD_getErrorName(init);
    return result;
  }

  ZSTD_inBuffer input{
      .src = compressed.data(),
      .size = compressed.size(),
      .pos = 0,
  };
  std::vector<char> scratch(ZSTD_DStreamOutSize());

  // Not trusted until the frame completes: a checksummed frame can emit
  // plaintext long before its trailing checksum is verified.
  std::string pending_frame;

  for (;;) {
    ZSTD_outBuffer output{
        .dst = scratch.data(),
        .size = scratch.size(),
        .pos = 0,
    };
    const std::size_t input_before = input.pos;
    const std::size_t ret =
        ZSTD_decompressStream(stream.get(), &output, &input);

    if (ZSTD_isError(ret)) {
      // Corruption, checksum mismatch, or a dictionary error: a hard
      // failure, not a torn tail.
      result.error =
          std::string{"zstd decompression failed: "} + ZSTD_getErrorName(ret);
      return result;
    }

    const std::uint64_t committed = result.text.size();
    const auto produced = static_cast<std::uint64_t>(output.pos);
    if (committed + pending_frame.size() + produced > max_output) {
      result.error = "decompressed log exceeds the size limit";
      return result;
    }
    pending_frame.append(scratch.data(), output.pos);

    if (ret == 0) {
      // Frame fully decoded and flushed: checksum verified.
      result.text.append(pending_frame);
      pending_frame.clear();
      ++result.complete_frames;

      if (input.pos == input.size) {
        return result;
      }
      // The same DStream automatically starts the next concatenated frame.
      continue;
    }

    if (input.pos == input.size && output.pos < output.size) {
      // ret > 0 with exhausted input and nothing left to flush: the final
      // frame is structurally incomplete.
      if (result.complete_frames == 0) {
        result.error = "zstd input has no complete frame";
        return result;
      }
      result.dropped_torn_tail = true;
      return result;
    }

    if (input.pos == input_before && output.pos == 0) {
      result.error = "zstd decoder made no progress";
      return result;
    }
  }
}

} // namespace lupe::detail
