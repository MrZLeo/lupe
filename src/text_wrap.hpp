#ifndef LUPE_TEXT_WRAP_HPP_
#define LUPE_TEXT_WRAP_HPP_

#include <cstddef>
#include <ftxui/screen/color.hpp>
#include <string>
#include <vector>

namespace lupe {

struct StyledSegment {
  std::string text;
  ftxui::Color color{ftxui::Color::Default};
  bool bold{false};
  bool italic{false};
  bool dim{false};
  bool code{false};
  bool link{false};
};

using DisplayLine = std::vector<StyledSegment>;

// Prefer spaces for soft wraps; split overlong tokens at glyph boundaries.
// Prefixes keep their styles and continuation indentation is width-bounded.
// Width zero uses one cell; an unrenderable wide glyph in one cell shows '?'.
void append_wrapped_line(std::vector<DisplayLine> &out,
                         const std::vector<StyledSegment> &segments,
                         StyledSegment first_prefix,
                         std::size_t continuation_cells, std::size_t width);

} // namespace lupe

#endif // LUPE_TEXT_WRAP_HPP_
