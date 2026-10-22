#ifndef LUPE_KITTY_GRAPHICS_HPP_
#define LUPE_KITTY_GRAPHICS_HPP_

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <span>
#include <string>

#include "lupe/log_message.hpp"

namespace lupe {

struct KittyImage {
  int format{100}; // PNG (100) or packed RGBA (32).
  int width{0};
  int height{0};
  std::string payload{}; // Canonical base64, without whitespace.
  std::string error{};

  explicit operator bool() const { return error.empty() && !payload.empty(); }
};

// Read inline data or a local file and prepare a direct Kitty transfer. Never
// fetch remote URLs, create temporary files, or invoke an external program.
KittyImage
prepare_kitty_image(const DisplayImage &image,
                    std::span<const std::filesystem::path> base_directories);

// Protocol commands are quiet (q=2) so replies cannot leak into TUI input.
// tmux wraps only graphics commands in DCS passthrough, never placeholder text.
void transmit_kitty_image(std::ostream &output, const KittyImage &image,
                          std::uint32_t id, bool tmux = false);
void place_kitty_image(std::ostream &output, std::uint32_t id, int columns,
                       int rows, bool tmux = false);
void
delete_kitty_image(std::ostream &output, std::uint32_t id, bool tmux = false);

inline constexpr int kKittyImageColumns = 80;
inline constexpr int kKittyImageRows = 12;

// A one-cell Unicode placeholder with explicit source row/column diacritics.
// Its foreground RGB value must encode the image ID (1..0xFFFFFF).
std::string kitty_placeholder(int row, int column);

} // namespace lupe

#endif // LUPE_KITTY_GRAPHICS_HPP_
