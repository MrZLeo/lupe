#ifndef PROJECT_LINE_FRAME_HPP_
#define PROJECT_LINE_FRAME_HPP_

#include <ftxui/dom/elements.hpp>

#include "lupe/scroll.hpp"

namespace lupe {

ftxui::Element line_frame(ftxui::Element content, ScrollState &scroll);
ftxui::Element scroll_progress_indicator(const ScrollState &scroll);

} // namespace lupe

#endif // PROJECT_LINE_FRAME_HPP_
