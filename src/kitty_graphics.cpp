#include "lupe/kitty_graphics.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <stb_image.h>

namespace lupe {
namespace {

constexpr std::size_t kMaxImageBytes = 32 * 1024 * 1024;
constexpr std::uint64_t kMaxImagePixels = 16 * 1024 * 1024;
constexpr std::size_t kChunkSize = 4096;

int base64_value(char character) {
  if (character >= 'A' && character <= 'Z') {
    return character - 'A';
  }
  if (character >= 'a' && character <= 'z') {
    return character - 'a' + 26;
  }
  if (character >= '0' && character <= '9') {
    return character - '0' + 52;
  }
  if (character == '+' || character == '-') {
    return 62;
  }
  if (character == '/' || character == '_') {
    return 63;
  }
  return -1;
}

std::optional<std::vector<unsigned char>>
decode_base64(std::string_view input) {
  std::vector<unsigned char> bytes;
  bytes.reserve(input.size() / 4 * 3);
  unsigned int buffer = 0;
  int bits = 0;
  int padding = 0;
  std::size_t digits = 0;
  for (const char character : input) {
    if (std::isspace(static_cast<unsigned char>(character)) != 0) {
      continue;
    }
    if (character == '=') {
      if (++padding > 2) {
        return std::nullopt;
      }
      continue;
    }
    const int value = base64_value(character);
    if (value < 0 || padding != 0) {
      return std::nullopt;
    }
    ++digits;
    buffer = (buffer << 6) | static_cast<unsigned int>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      bytes.push_back(static_cast<unsigned char>(buffer >> bits));
    }
  }
  const auto remainder = digits % 4;
  if (remainder == 1
      || (buffer & ((1U << bits) - 1U)) != 0
      || (padding != 0
          && (remainder == 0
              || remainder + static_cast<std::size_t>(padding) != 4))) {
    return std::nullopt;
  }
  return bytes;
}

std::string encode_base64(std::span<const unsigned char> bytes) {
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  result.reserve((bytes.size() + 2) / 3 * 4);
  for (std::size_t offset = 0; offset < bytes.size(); offset += 3) {
    const unsigned int first = bytes[offset];
    const unsigned int second =
        offset + 1 < bytes.size() ? bytes[offset + 1] : 0;
    const unsigned int third =
        offset + 2 < bytes.size() ? bytes[offset + 2] : 0;
    result.push_back(alphabet[first >> 2]);
    result.push_back(alphabet[((first & 3U) << 4) | (second >> 4)]);
    result.push_back(offset + 1 < bytes.size()
                         ? alphabet[((second & 15U) << 2) | (third >> 6)]
                         : '=');
    result.push_back(offset + 2 < bytes.size() ? alphabet[third & 63U] : '=');
  }
  return result;
}

int hexadecimal_value(char character) {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return character - 'a' + 10;
  }
  if (character >= 'A' && character <= 'F') {
    return character - 'A' + 10;
  }
  return -1;
}

std::optional<std::string> decode_file_url(std::string_view url) {
  url.remove_prefix(std::string_view{"file://"}.size());
  if (url.starts_with("localhost/")) {
    url.remove_prefix(std::string_view{"localhost"}.size());
  } else if (url.empty() || url.front() != '/') {
    return std::nullopt;
  }
  std::string decoded;
  for (std::size_t index = 0; index < url.size(); ++index) {
    if (url[index] != '%') {
      decoded.push_back(url[index]);
      continue;
    }
    if (index + 2 >= url.size()) {
      return std::nullopt;
    }
    const int high = hexadecimal_value(url[index + 1]);
    const int low = hexadecimal_value(url[index + 2]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    decoded.push_back(static_cast<char>((high << 4) | low));
    index += 2;
  }
#ifdef _WIN32
  if (decoded.size() >= 3
      && decoded[0] == '/'
      && std::isalpha(static_cast<unsigned char>(decoded[1])) != 0
      && decoded[2] == ':') {
    decoded.erase(decoded.begin());
  }
#endif
  return decoded;
}

std::vector<unsigned char>
read_image_file(std::string_view reference,
                std::span<const std::filesystem::path> base_directories,
                std::string &error) {
  if (reference.starts_with("http://") || reference.starts_with("https://")) {
    error = "remote image URLs are not fetched";
    return {};
  }
  std::string path_string{reference};
  if (reference.starts_with("file://")) {
    auto decoded = decode_file_url(reference);
    if (!decoded) {
      error = "invalid local image URL";
      return {};
    }
    path_string = std::move(*decoded);
  } else if (reference.contains("://")) {
    error = "unsupported image reference";
    return {};
  }
  if (path_string.empty() || path_string.contains('\0')) {
    error = "invalid local image path";
    return {};
  }
  const std::filesystem::path path{path_string};
  std::vector<std::filesystem::path> candidates;
  if (!path.is_absolute()) {
    for (const auto &directory : base_directories) {
      if (!directory.empty()) {
        candidates.push_back(directory / path);
      }
    }
  }
  candidates.push_back(path);
  for (const auto &candidate : candidates) {
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(candidate, filesystem_error)
        || filesystem_error) {
      continue;
    }
    const auto size = std::filesystem::file_size(candidate, filesystem_error);
    if (filesystem_error) {
      error = "could not read image file";
      return {};
    }
    if (size > kMaxImageBytes) {
      error = "image exceeds the 32 MiB input limit";
      return {};
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    std::ifstream stream{candidate, std::ios::binary};
    if (!stream.read(reinterpret_cast<char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()))) {
      error = "could not read image file";
      return {};
    }
    return bytes;
  }
  error = "image file not found";
  return {};
}

void write_command(std::ostream &output, std::string_view control,
                   std::string_view payload, bool tmux) {
  // Controls and payload are generated internally (numeric fields/base64),
  // never log text. Only the two ESC bytes need doubling for tmux.
  if (tmux) {
    output << "\x1bPtmux;\x1b";
  }
  output << "\x1b_G" << control;
  if (!payload.empty()) {
    output << ';' << payload;
  }
  if (tmux) {
    output << '\x1b';
  }
  output << "\x1b\\";
  if (tmux) {
    output << "\x1b\\";
  }
}

} // namespace

KittyImage
prepare_kitty_image(const DisplayImage &image,
                    std::span<const std::filesystem::path> base_directories) {
  std::vector<unsigned char> bytes;
  if (image.base64_data) {
    if (image.base64_data->size() > kMaxImageBytes) {
      return KittyImage{.error = "image exceeds the 32 MiB input limit"};
    }
    auto decoded = decode_base64(*image.base64_data);
    if (!decoded) {
      return KittyImage{.error = "invalid base64 image data"};
    }
    bytes = std::move(*decoded);
  } else if (image.url.empty()) {
    return KittyImage{.error = image.inline_data
                                 ? "inline image data is unavailable"
                                 : "image has no source"};
  } else {
    std::string error;
    bytes = read_image_file(image.url, base_directories, error);
    if (!error.empty()) {
      return KittyImage{.error = std::move(error)};
    }
  }

  int width = 0;
  int height = 0;
  int channels = 0;
  const int length = static_cast<int>(bytes.size());
  if (bytes.empty()
      || stbi_info_from_memory(bytes.data(), length, &width, &height, &channels)
             == 0) {
    return KittyImage{
        .error =
            "unsupported or invalid image (PNG, JPEG, GIF, BMP supported)"};
  }
  if (width <= 0
      || height <= 0
      || static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height)
             > kMaxImagePixels) {
    return KittyImage{.error = "image exceeds the 16 megapixel limit"};
  }
  const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels{
      stbi_load_from_memory(bytes.data(), length, &width, &height, &channels,
                            4),
      stbi_image_free};
  if (!pixels) {
    return KittyImage{.error = "could not decode image"};
  }

  constexpr std::array<unsigned char, 8> png_signature{0x89, 'P',  'N',  'G',
                                                       0x0D, 0x0A, 0x1A, 0x0A};
  const bool png =
      bytes.size() >= png_signature.size()
      && std::equal(png_signature.begin(), png_signature.end(), bytes.begin());
  // PNG is already a Kitty wire format. Other raster formats must be decoded
  // to RGBA; labeling JPEG/WebP bytes as f=100 would silently fail in
  // terminals.
  const auto rgba_size =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
  return KittyImage{
      .format = png ? 100 : 32,
      .width = width,
      .height = height,
      .payload = encode_base64(
          png ? std::span<const unsigned char>{bytes}
              : std::span<const unsigned char>{pixels.get(), rgba_size}),
  };
}

void transmit_kitty_image(std::ostream &output, const KittyImage &image,
                          std::uint32_t id, bool tmux) {
  if (!image || id == 0 || id > 0xFFFFFF) {
    return;
  }
  for (std::size_t offset = 0; offset < image.payload.size();
       offset += kChunkSize) {
    const auto chunk =
        std::string_view{image.payload}.substr(offset, kChunkSize);
    std::string control;
    if (offset == 0) {
      control = "a=t,t=d,f="
              + std::to_string(image.format)
              + ",i="
              + std::to_string(id);
      if (image.format == 32) {
        control += ",s="
                 + std::to_string(image.width)
                 + ",v="
                 + std::to_string(image.height);
      }
      control += ',';
    }
    control +=
        offset + chunk.size() < image.payload.size() ? "m=1,q=2" : "m=0,q=2";
    write_command(output, control, chunk, tmux);
  }
}

void place_kitty_image(std::ostream &output, std::uint32_t id, int columns,
                       int rows, bool tmux) {
  if (id == 0 || id > 0xFFFFFF || columns <= 0 || rows <= 0) {
    return;
  }
  // One virtual placement per image, so placeholder text need not carry an
  // underline-color placement ID. U=1 also preserves the image's aspect ratio.
  write_command(output,
                "a=p,U=1,i="
                    + std::to_string(id)
                    + ",p=1,c="
                    + std::to_string(columns)
                    + ",r="
                    + std::to_string(rows)
                    + ",q=2",
                {}, tmux);
}

void delete_kitty_image(std::ostream &output, std::uint32_t id, bool tmux) {
  if (id != 0 && id <= 0xFFFFFF) {
    write_command(output, "a=d,d=I,i=" + std::to_string(id) + ",q=2", {}, tmux);
  }
}

std::string kitty_placeholder(int row, int column) {
  // First 80 entries of the protocol's fixed row/column table, NOT a contiguous
  // Unicode range:
  // https://sw.kovidgoyal.net/kitty/graphics-protocol/#unicode-placeholders
  static constexpr std::array<std::string_view, kKittyImageColumns> diacritics{
      "\u0305", "\u030D", "\u030E", "\u0310", "\u0312", "\u033D", "\u033E",
      "\u033F", "\u0346", "\u034A", "\u034B", "\u034C", "\u0350", "\u0351",
      "\u0352", "\u0357", "\u035B", "\u0363", "\u0364", "\u0365", "\u0366",
      "\u0367", "\u0368", "\u0369", "\u036A", "\u036B", "\u036C", "\u036D",
      "\u036E", "\u036F", "\u0483", "\u0484", "\u0485", "\u0486", "\u0487",
      "\u0592", "\u0593", "\u0594", "\u0595", "\u0597", "\u0598", "\u0599",
      "\u059C", "\u059D", "\u059E", "\u059F", "\u05A0", "\u05A1", "\u05A8",
      "\u05A9", "\u05AB", "\u05AC", "\u05AF", "\u05C4", "\u0610", "\u0611",
      "\u0612", "\u0613", "\u0614", "\u0615", "\u0616", "\u0617", "\u0657",
      "\u0658", "\u0659", "\u065A", "\u065B", "\u065D", "\u065E", "\u06D6",
      "\u06D7", "\u06D8", "\u06D9", "\u06DA", "\u06DB", "\u06DC", "\u06DF",
      "\u06E0", "\u06E1", "\u06E2",
  };
  if (row < 0
      || row >= kKittyImageRows
      || column < 0
      || column >= kKittyImageColumns) {
    return {};
  }
  return std::string{"\U0010EEEE"}
       + std::string{diacritics[static_cast<std::size_t>(row)]}
       + std::string{diacritics[static_cast<std::size_t>(column)]};
}

} // namespace lupe
