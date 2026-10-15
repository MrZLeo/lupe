#ifndef PROJECT_SCROLL_HPP_
#define PROJECT_SCROLL_HPP_

namespace lupe {

struct ScrollState {
  int top_row{0};
  int max_top_row{0};
  bool follow_focus{true};
  // Row height of the visible area, reported by LineFrame during layout.
  // Renderers use it to size the virtualized window before layout runs.
  int viewport_rows{0};
};

void follow_selection(ScrollState &scroll);
void
update_scroll_layout(ScrollState &scroll, int focused_top_row, int max_top_row);
void scroll_by_rows(ScrollState &scroll, int rows);
void scroll_to_top(ScrollState &scroll);
void scroll_to_bottom(ScrollState &scroll);
int scroll_progress_percent(const ScrollState &scroll);

// Scroll geometry shared by LineFrame (which applies it during layout) and
// renderers (which need to know, before layout, which rows will be visible).
int
centered_top_row(int focus_first_row, int focus_last_row, int viewport_rows);
int max_top_row_for(int total_rows, int viewport_rows);

} // namespace lupe

#endif // PROJECT_SCROLL_HPP_
