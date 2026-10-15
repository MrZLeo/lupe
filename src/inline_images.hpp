#ifndef LUPE_INLINE_IMAGES_HPP_
#define LUPE_INLINE_IMAGES_HPP_

#include <cstdint>
#include <filesystem>
#include <ftxui/dom/elements.hpp>
#include <iosfwd>
#include <map>
#include <span>
#include <string>

#include "lupe/log_message.hpp"

namespace lupe {

// Owns only the terminal resources for the current viewport. Image bytes are
// decoded lazily when a row actually intersects the rendering stencil, then
// released after transmission. Call clear before replacing the message vector.
class InlineImages {
public:
  explicit InlineImages(bool tmux = false) : tmux_(tmux) {}
  InlineImages(const InlineImages &) = delete;
  InlineImages &operator=(const InlineImages &) = delete;
  InlineImages(InlineImages &&) noexcept = default;
  InlineImages &operator=(InlineImages &&) noexcept = default;

  ftxui::Element line(const DisplayImage &image, int row, int columns,
                      std::span<const std::filesystem::path> base_directories,
                      std::ostream &output);
  ftxui::Element frame(ftxui::Element document, std::ostream &output);
  void clear(std::ostream &output);

private:
  class Row;
  class Frame;
  struct Entry {
    std::uint32_t id{0};
    std::string error;
    bool used{false};
  };
  Entry &use(const DisplayImage &image, int columns,
             std::span<const std::filesystem::path> base_directories,
             std::ostream &output);
  void begin_frame();
  void end_frame(std::ostream &output);

  bool tmux_;
  std::map<const DisplayImage *, Entry> entries_;
};

} // namespace lupe

#endif // LUPE_INLINE_IMAGES_HPP_
