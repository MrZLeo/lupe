#include "lupe/scroll.hpp"

#include <algorithm>
#include <limits>

namespace lupe {

void follow_selection(ScrollState &scroll) { scroll.follow_focus = true; }

void update_scroll_layout(ScrollState &scroll, int focused_top_row,
                          int max_top_row) {
  scroll.max_top_row = std::max(0, max_top_row);
  if (scroll.follow_focus) {
    scroll.top_row = std::clamp(focused_top_row, 0, scroll.max_top_row);
    return;
  }
  scroll.top_row = std::clamp(scroll.top_row, 0, scroll.max_top_row);
}

void scroll_by_rows(ScrollState &scroll, int rows) {
  scroll.follow_focus = false;
  const int clamped_rows =
      std::clamp(rows, -scroll.top_row, scroll.max_top_row - scroll.top_row);
  scroll.top_row += clamped_rows;
}

void scroll_to_top(ScrollState &scroll) {
  scroll.follow_focus = false;
  scroll.top_row = 0;
}

void scroll_to_bottom(ScrollState &scroll) {
  scroll.follow_focus = false;
  // max_top_row may be stale until the next layout (view switches reuse the
  // same ScrollState for different content). Overshoot and let
  // update_scroll_layout clamp to the real bottom. Half of INT_MAX keeps the
  // intermediate arithmetic in scroll_by_rows free of overflow.
  scroll.top_row = std::numeric_limits<int>::max() / 2;
}

int scroll_progress_percent(const ScrollState &scroll) {
  if (scroll.max_top_row <= 0) {
    return 100;
  }

  const int top_row = std::clamp(scroll.top_row, 0, scroll.max_top_row);
  return static_cast<int>(
      static_cast<long long>(top_row) * 100 / scroll.max_top_row);
}

int
centered_top_row(int focus_first_row, int focus_last_row, int viewport_rows) {
  const int focus_rows = focus_last_row - focus_first_row + 1;
  if (focus_rows >= viewport_rows) {
    // Taller than the viewport: anchor the top so the block reads from its
    // first line and the window below stays filled.
    return focus_first_row;
  }
  // Center the block, but never so far that any part of it leaves the
  // viewport.
  const int centered = focus_first_row
                     - (viewport_rows - 1) / 2
                     + (focus_last_row - focus_first_row) / 2;
  return std::clamp(centered, focus_last_row - viewport_rows + 1,
                    focus_first_row);
}

int max_top_row_for(int total_rows, int viewport_rows) {
  return std::max(0, total_rows - viewport_rows);
}

} // namespace lupe
