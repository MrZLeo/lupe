#include <algorithm>
#include <argum/data.h>
#include <argum/parser.h>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/task.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "inline_images.hpp"
#include "line_frame.hpp"
#include "lupe/kitty_graphics.hpp"
#include "lupe/log_format.hpp"
#include "lupe/log_message.hpp"
#include "lupe/log_parser.hpp"
#include "lupe/markdown_text.hpp"
#include "lupe/message_projection.hpp"
#include "lupe/scroll.hpp"
#include "lupe/search.hpp"
#include "lupe/session_ir.hpp"
#include "lupe/session_parser.hpp"
#include "lupe/structured_text.hpp"
#include "lupe/synchronized_output.hpp"
#include "lupe/version.hpp"
#include "message_overview.hpp"
#include "text_wrap.hpp"

namespace {
struct CliOptions {
  std::filesystem::path path;
  std::optional<lupe::LogFormat> format;
};

struct CliParseResult {
  CliOptions options;
  std::string format_error;
  bool run{false};
  int exit_code{EXIT_SUCCESS};
};

using lupe::append_wrapped_line;
using lupe::DisplayLine;
using lupe::StyledSegment;

struct ImageDisplayRow {
  std::size_t image_index{0};
  int row{0};
};

struct AppState {
  std::filesystem::path path;
  lupe::LogFormat format{lupe::LogFormat::Generic};
  lupe::SessionParseResult session;
  lupe::ParseResult parsed;
  std::size_t selected{0};
  lupe::ScrollState scroll;
  std::string status;
  bool search_active{false};
  std::string search_input;
  std::string search_query;
  std::vector<std::size_t> search_matches;
  std::size_t search_origin_selected{0};
  bool show_diagnostics{false};
  // Per-message fold state: folded[i] collapses message i to its header
  // plus a one-line summary (see fold_summary_line). Sized to messages on
  // load and reset on reload, since indices may shift.
  std::vector<bool> folded;
  // Cached display lines, rebuilt when the terminal width or the messages
  // change. message_rows[i] is the [first, last) display-line range of
  // message i.
  std::vector<DisplayLine> display_lines;
  std::vector<std::pair<std::size_t, std::size_t>> message_rows;
  int lines_width{-1};
  std::optional<std::size_t> overview_hovered;
  ftxui::Box overview_box;
  std::map<std::size_t, ImageDisplayRow> image_rows;
  lupe::InlineImages inline_images{std::getenv("TMUX") != nullptr};
};

struct BrowserEntry {
  std::filesystem::path path;
  std::string display_path;
  std::string modified_label;
};

struct BrowserState {
  std::filesystem::path root;
  std::vector<BrowserEntry> entries;
  std::vector<std::size_t> visible_entries;
  std::size_t selected{0};
  lupe::ScrollState scroll;
  std::string status;
  bool search_active{false};
  std::string search_input;
  std::string search_query;
};

struct ApplicationState {
  lupe::LogFormat format{lupe::LogFormat::Generic};
  BrowserState browser;
  AppState viewer;
  bool browser_available{false};
  bool showing_browser{false};
};

enum class HighlightKind {
  None,
  Match,
  Current,
};

constexpr std::size_t kPageMove = 10;
constexpr std::size_t kNoRow = static_cast<std::size_t>(-1);

int wheel_rows(ftxui::Event &event) {
  if (!event.is_mouse()) {
    return 0;
  }
  if (event.mouse().button == ftxui::Mouse::WheelUp) {
    return -1;
  }
  if (event.mouse().button == ftxui::Mouse::WheelDown) {
    return 1;
  }
  return 0;
}

CliParseResult parse_cli_args(int argc, char **argv) {
  const char *program_name = argc > 0 && argv[0] != nullptr ? argv[0] : "lupe";

  CliParseResult result;
  Argum::Parser parser;

  parser.add(Argum::Option("--help", "-h")
                 .help("show this help message and exit")
                 .handler([&]() {
                   std::cout << parser.formatHelp(program_name);
                   // Safe: option handling runs before any worker threads.
                   std::exit(EXIT_SUCCESS); // NOLINT(concurrency-mt-unsafe)
                 }));
  parser.add(Argum::Option("--version")
                 .help("show program's version number and exit")
                 .handler([]() {
                   std::cout << "lupe " << LUPE_VERSION << '\n';
                   // Safe: option handling runs before any worker threads.
                   std::exit(EXIT_SUCCESS); // NOLINT(concurrency-mt-unsafe)
                 }));
  parser.add(Argum::Option("--format", "-f")
                 .help("log format: pi, codex, codex-exec, claudecode, "
                       "deepseek-harness, or generic (default: auto-detect)")
                 .handler([&](const std::string_view &value) {
                   result.options.format = lupe::parse_log_format(value);
                   if (!result.options.format) {
                     result.format_error =
                         "unsupported log format: "
                         + std::string{value}
                         + " (expected pi, codex, codex-exec, claudecode, "
                           "deepseek-harness, generic, or auto)";
                   }
                 }));

  parser.add(Argum::Positional("path")
                 .occurs(Argum::zeroOrOneTime)
                 .help("agent log file or directory; defaults to current "
                       "directory")
                 .handler([&](const std::string_view &value) {
                   result.options.path =
                       std::filesystem::path{std::string{value}};
                 }));

  // argum runs in its Expected-based mode (ARGUM_USE_EXPECTED), so parsing
  // errors are reported via the return value instead of exceptions.
  const auto parse_result = parser.parse(argc, argv);
  if (const auto parse_error = parse_result.error()) {
    std::cerr << parse_error->message() << '\n';
    std::cerr << parser.formatUsage(program_name) << '\n';
    result.exit_code = EXIT_FAILURE;
    return result;
  }

  if (!result.format_error.empty()) {
    std::cerr << result.format_error << '\n';
    std::cerr << parser.formatUsage(program_name) << '\n';
    result.exit_code = EXIT_FAILURE;
    return result;
  }
  // Without --format, each opened log is auto-detected on load.
  if (!result.options.format) {
    result.options.format = lupe::LogFormat::Auto;
  }

  result.run = true;
  return result;
}

std::string short_path(const std::filesystem::path &path) {
  auto filename = path.filename().string();
  if (!filename.empty()) {
    return filename;
  }
  return path.string();
}

std::string clipped_label(std::string_view text) {
  constexpr std::size_t kMaxLabelLength = 60;
  if (text.size() <= kMaxLabelLength) {
    return std::string{text};
  }
  return std::string{text.substr(0, kMaxLabelLength - 3)} + "...";
}

ftxui::Color diagnostic_color(lupe::DiagnosticSeverity severity) {
  switch (severity) {
  case lupe::DiagnosticSeverity::Warning:
    return ftxui::Color::YellowLight;
  case lupe::DiagnosticSeverity::Error:
  case lupe::DiagnosticSeverity::Fatal:
    return ftxui::Color::RedLight;
  case lupe::DiagnosticSeverity::Info:
  default:
    return ftxui::Color::GrayLight;
  }
}

std::string lowercase_copy(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char value : text) {
    out.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(value))));
  }
  return out;
}

bool
contains_case_insensitive(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) {
    return true;
  }

  const std::string lower_haystack = lowercase_copy(haystack);
  const std::string lower_needle = lowercase_copy(needle);
  return lower_haystack.contains(lower_needle);
}

bool is_log_candidate(const std::filesystem::path &path) {
  const std::string extension = lowercase_copy(path.extension().string());
  return extension == ".json"
      || extension == ".jsonl"
      || extension == ".ndjson"
      || extension == ".log"
      || extension == ".zstd";
}

bool is_ignored_browser_directory(const std::filesystem::path &path) {
  const std::string name = path.filename().string();
  if (name == ".git"
      || name == ".cpm-cache"
      || name == "build"
      || name == "CMakeFiles"
      || name == "_deps"
      || name == "node_modules"
      || name == "dist"
      || name == "out"
      || name == "target"
      || name == ".venv"
      || name == "venv") {
    return true;
  }
  return name.rfind("cmake-build-", 0) == 0;
}

std::string pluralize(std::size_t count, std::string_view singular,
                      std::string_view plural) {
  return std::to_string(count)
       + " "
       + std::string{count == 1 ? singular : plural};
}

std::string age_label(std::filesystem::file_time_type modified_time) {
  using namespace std::chrono;

  const auto now = std::filesystem::file_time_type::clock::now();
  if (modified_time >= now) {
    return "just now";
  }

  const auto age = now - modified_time;
  const auto minutes = duration_cast<std::chrono::minutes>(age).count();
  if (minutes < 1) {
    return "just now";
  }
  if (minutes < 60) {
    return pluralize(static_cast<std::size_t>(minutes), "minute ago",
                     "minutes ago");
  }

  const auto hours = duration_cast<std::chrono::hours>(age).count();
  if (hours < 24) {
    return pluralize(static_cast<std::size_t>(hours), "hour ago", "hours ago");
  }

  const auto days = duration_cast<std::chrono::hours>(age).count() / 24;
  if (days < 30) {
    return pluralize(static_cast<std::size_t>(days), "day ago", "days ago");
  }
  if (days < 365) {
    return pluralize(static_cast<std::size_t>(days / 30), "month ago",
                     "months ago");
  }
  return pluralize(static_cast<std::size_t>(days / 365), "year ago",
                   "years ago");
}

std::string relative_display_path(const std::filesystem::path &path,
                                  const std::filesystem::path &root) {
  std::error_code error;
  const auto relative = std::filesystem::relative(path, root, error);
  if (!error && !relative.empty()) {
    return relative.generic_string();
  }
  return path.generic_string();
}

std::string browser_query(const BrowserState &state) {
  return state.search_active ? state.search_input : state.search_query;
}

void apply_browser_filter(BrowserState &state) {
  lupe::follow_selection(state.scroll);
  state.visible_entries.clear();
  const std::string query = browser_query(state);

  for (std::size_t index = 0; index < state.entries.size(); ++index) {
    if (contains_case_insensitive(state.entries[index].display_path, query)) {
      state.visible_entries.push_back(index);
    }
  }

  if (state.visible_entries.empty()) {
    state.selected = 0;
    return;
  }
  state.selected = std::min(state.selected, state.visible_entries.size() - 1);
}

void refresh_browser(BrowserState &state) {
  state.entries.clear();
  std::error_code error;

  if (!std::filesystem::exists(state.root, error) || error) {
    state.status = "directory does not exist: " + state.root.string();
    apply_browser_filter(state);
    return;
  }
  if (!std::filesystem::is_directory(state.root, error) || error) {
    state.status = "not a directory: " + state.root.string();
    apply_browser_filter(state);
    return;
  }

  const auto options =
      std::filesystem::directory_options::skip_permission_denied;
  std::filesystem::recursive_directory_iterator iterator{state.root, options,
                                                         error};
  if (error) {
    state.status = "could not read directory: " + error.message();
    apply_browser_filter(state);
    return;
  }

  const std::filesystem::recursive_directory_iterator end;
  bool skipped_entries = false;

  while (!error && iterator != end) {
    const auto entry = *iterator;
    std::error_code entry_error;
    if (entry.is_directory(entry_error)) {
      if (is_ignored_browser_directory(entry.path())) {
        iterator.disable_recursion_pending();
      }
    } else if (!entry_error
               && entry.is_regular_file(entry_error)
               && !entry_error
               && is_log_candidate(entry.path())) {
      std::string modified = "unknown";
      std::error_code time_error;
      const auto write_time =
          std::filesystem::last_write_time(entry.path(), time_error);
      if (!time_error) {
        modified = age_label(write_time);
      }
      state.entries.push_back(BrowserEntry{
          .path = entry.path(),
          .display_path = relative_display_path(entry.path(), state.root),
          .modified_label = modified,
      });
    }

    iterator.increment(error);
    if (error) {
      skipped_entries = true;
      error.clear();
    }
  }

  std::ranges::sort(state.entries,
                    [](const BrowserEntry &left, const BrowserEntry &right) {
                      return lowercase_copy(left.display_path)
                           < lowercase_copy(right.display_path);
                    });

  apply_browser_filter(state);
  state.status = skipped_entries ? "some entries were skipped" : "";
}

void begin_browser_search(BrowserState &state) {
  state.search_active = true;
  state.search_input.clear();
  state.status.clear();
  apply_browser_filter(state);
}

void cancel_browser_search(BrowserState &state) {
  state.search_active = false;
  state.search_input.clear();
  state.status = "search canceled";
  apply_browser_filter(state);
}

void commit_browser_search(BrowserState &state) {
  state.search_active = false;
  state.search_query = state.search_input;
  apply_browser_filter(state);
  if (state.search_query.empty()) {
    state.status = "empty search";
    return;
  }
  state.status = state.visible_entries.empty() ? "no matches" : "";
}

void update_browser_search_preview(BrowserState &state) {
  state.status.clear();
  apply_browser_filter(state);
}

void move_browser_up(BrowserState &state, std::size_t amount) {
  lupe::follow_selection(state.scroll);
  state.selected -= std::min(state.selected, amount);
}

void move_browser_down(BrowserState &state, std::size_t amount) {
  lupe::follow_selection(state.scroll);
  if (state.visible_entries.empty()) {
    state.selected = 0;
    return;
  }
  const std::size_t last = state.visible_entries.size() - 1;
  state.selected += std::min(last - state.selected, amount);
}

// render_browser lays out each entry as a two-row block followed by a blank
// separator row; selection sync mirrors that geometry.
constexpr int kBrowserRowsPerEntry = 3;

// While a selection jump is pending (follow_focus set), top_row still holds
// the pre-jump offset; the recenter happens at the next layout. Wheel
// events arriving in the same input burst would scroll — and judge
// selection visibility — against that stale offset, so resolve the
// recenter here first, with the same geometry LineFrame applies. Also
// refreshes max_top_row so the wheel delta is clamped against the current
// content, not the previous view's.
void resolve_pending_browser_recenter(BrowserState &state) {
  if (!state.scroll.follow_focus
      || state.scroll.viewport_rows <= 0
      || state.visible_entries.empty()) {
    return;
  }
  state.selected = std::min(state.selected, state.visible_entries.size() - 1);
  const int first_row = static_cast<int>(state.selected) * kBrowserRowsPerEntry;
  const int total_rows =
      static_cast<int>(state.visible_entries.size()) * kBrowserRowsPerEntry;
  state.scroll.max_top_row =
      lupe::max_top_row_for(total_rows, state.scroll.viewport_rows);
  // The focused element is the two-row entry block; its last row is
  // first_row + 1, matching what LineFrame centers on during layout.
  state.scroll.top_row =
      std::clamp(lupe::centered_top_row(first_row, first_row + 1,
                                         state.scroll.viewport_rows),
                 0, state.scroll.max_top_row);
}

// While the mouse wheel scrolls, the topmost visible entry is the current
// one, so the selection follows the viewport in both directions and
// keyboard navigation resumes from what is on screen.
void sync_browser_selection_to_viewport(BrowserState &state) {
  if (state.visible_entries.empty() || state.scroll.viewport_rows <= 0) {
    return;
  }
  const std::size_t last = state.visible_entries.size() - 1;
  const int top = std::max(state.scroll.top_row, 0);
  // First entry whose highlight row is at or below the top edge; the
  // highlight only renders on the entry's first row, so anchoring on that
  // row keeps the selection marker on screen.
  const int index = (top + kBrowserRowsPerEntry - 1) / kBrowserRowsPerEntry;
  state.selected = std::min(static_cast<std::size_t>(index), last);
}

const BrowserEntry *selected_browser_entry(const BrowserState &state) {
  if (state.visible_entries.empty()) {
    return nullptr;
  }
  return &state.entries[state.visible_entries[state.selected]];
}

AppState
load_viewer_state(const std::filesystem::path &path, lupe::LogFormat format) {
  auto session = lupe::parse_session_file(path, format);
  lupe::ParseResult parsed;
  if (!session.has_fatal_error()) {
    parsed.messages = lupe::make_display_messages(session.session);
  }
  for (const auto &diagnostic : session.diagnostics) {
    parsed.errors.push_back(lupe::format_diagnostic(diagnostic));
  }
  parsed.diagnostics = session.diagnostics;
  AppState loaded;
  loaded.path = path;
  // Surface the effective format (relevant when Auto resolved it), so the
  // status line and reload both use what actually parsed the file.
  loaded.format = session.session.format;
  // A fatal log has no trustworthy transcript; open on the diagnostics.
  loaded.show_diagnostics = session.has_fatal_error();
  loaded.session = std::move(session);
  loaded.parsed = std::move(parsed);
  loaded.folded.assign(loaded.parsed.messages.size(), false);
  return loaded;
}

// ftxui's text() drops control characters, so raw tabs vanish from command
// output (`nl -ba` separators, indented code). Expand them against standard
// 8-column stops. Column counting tracks code points, not bytes, so
// multi-byte UTF-8 does not skew the stops.
constexpr std::size_t kTabWidth = 8;

std::string expand_tabs(std::string_view text) {
  if (!text.contains('\t')) {
    return std::string{text};
  }

  std::string expanded;
  expanded.reserve(text.size());
  std::size_t column = 0;
  for (const char raw : text) {
    const auto byte = static_cast<unsigned char>(raw);
    if (raw == '\t') {
      const std::size_t spaces = kTabWidth - column % kTabWidth;
      expanded.append(spaces, ' ');
      column += spaces;
      continue;
    }
    expanded.push_back(raw);
    if (raw == '\n') {
      column = 0;
    } else if ((byte & 0xC0U) != 0x80U) {
      ++column;
    }
  }
  return expanded;
}

// ---------------------------------------------------------------------------
// Display line model
//
// Message bodies are wrapped into styled display lines once per terminal
// width and cached. Each frame instantiates ftxui elements only for the rows
// intersecting the viewport, so the per-frame cost tracks the viewport size
// instead of the document size. Laying out the whole document with nested
// flexboxes on every frame was the scrolling bottleneck.

// Clip text to `width` cells at a glyph boundary.
std::string clip_to_width(std::string_view text, std::size_t width) {
  std::string kept;
  std::size_t cells = 0;
  for (const std::string &glyph : ftxui::Utf8ToGlyphs(text)) {
    const std::size_t glyph_cells =
        static_cast<std::size_t>(ftxui::string_width(glyph));
    if (cells + glyph_cells > width) {
      break;
    }
    kept += glyph;
    cells += glyph_cells;
  }
  return kept;
}

// Truncate a single display line to `width` cells at glyph boundaries. Used
// for header metadata, which must stay on one line.
void truncate_display_line(DisplayLine &line, std::size_t width) {
  std::size_t cells = 0;
  std::size_t kept_segments = 0;
  for (; kept_segments < line.size(); ++kept_segments) {
    StyledSegment &segment = line[kept_segments];
    const std::size_t segment_cells =
        static_cast<std::size_t>(ftxui::string_width(segment.text));
    if (cells + segment_cells <= width) {
      cells += segment_cells;
      continue;
    }
    segment.text = clip_to_width(segment.text, width - cells);
    ++kept_segments;
    break;
  }
  line.resize(kept_segments);
}

StyledSegment
style_from_markdown(const lupe::MarkdownSpan &span, ftxui::Color base_color) {
  return StyledSegment{
      .text = span.text,
      .color = base_color,
      .bold = span.bold,
      .italic = span.italic,
      .dim = false,
      .code = span.code,
      .link = !span.link_url.empty(),
  };
}

std::vector<StyledSegment>
segments_from_markdown(const std::vector<lupe::MarkdownSpan> &spans,
                       ftxui::Color base_color) {
  std::vector<StyledSegment> segments;
  segments.reserve(spans.size());
  for (const lupe::MarkdownSpan &span : spans) {
    segments.push_back(style_from_markdown(span, base_color));
  }
  return segments;
}

void append_markdown_lines(std::string_view content, std::size_t width,
                           std::vector<DisplayLine> &out) {
  using ftxui::Color;
  for (const lupe::MarkdownBlock &block :
       lupe::parse_markdown_text(content)) {
    switch (block.kind) {
    case lupe::MarkdownBlockKind::Blank:
      out.emplace_back();
      break;

    case lupe::MarkdownBlockKind::CodeBlock: {
      std::size_t start = 0;
      while (start <= block.code.size()) {
        const std::size_t end = block.code.find('\n', start);
        const std::string_view code_line =
            end == std::string_view::npos
                ? std::string_view{block.code}.substr(start)
                : std::string_view{block.code}.substr(start, end - start);
        append_wrapped_line(
            out,
            {StyledSegment{.text = expand_tabs(code_line),
                           .color = Color::CyanLight}},
            StyledSegment{.text = "  ", .color = Color::CyanLight}, 2, width);
        if (end == std::string_view::npos) {
          break;
        }
        start = end + 1;
      }
      break;
    }

    case lupe::MarkdownBlockKind::Heading: {
      const Color heading_color =
          block.level <= 2 ? Color::White : Color::GrayLight;
      std::vector<StyledSegment> segments =
          segments_from_markdown(block.spans, heading_color);
      for (StyledSegment &segment : segments) {
        segment.bold = true;
      }
      append_wrapped_line(out, segments, StyledSegment{}, 0, width);
      break;
    }

    case lupe::MarkdownBlockKind::ListItem: {
      const std::size_t indent =
          static_cast<std::size_t>(std::min(block.level, 6)) * 2;
      StyledSegment prefix{
          .text = std::string(indent, ' ')
                + (block.marker.empty() ? "-" : block.marker)
                + " ",
          .color = Color::GrayDark,
      };
      const std::size_t prefix_cells =
          static_cast<std::size_t>(ftxui::string_width(prefix.text));
      append_wrapped_line(out,
                          segments_from_markdown(block.spans, Color::GrayLight),
                          std::move(prefix), prefix_cells, width);
      break;
    }

    case lupe::MarkdownBlockKind::Quote: {
      std::vector<StyledSegment> segments =
          segments_from_markdown(block.spans, Color::GrayLight);
      for (StyledSegment &segment : segments) {
        segment.dim = true;
      }
      append_wrapped_line(out, segments,
                          StyledSegment{.text = "> ", .color = Color::GrayDark},
                          2, width);
      break;
    }

    case lupe::MarkdownBlockKind::Paragraph:
      append_wrapped_line(out,
                          segments_from_markdown(block.spans, Color::GrayLight),
                          StyledSegment{}, 0, width);
      break;
    }
  }
}

void append_verbatim_lines(std::string_view content, std::size_t width,
                           std::vector<DisplayLine> &out) {
  std::size_t start = 0;
  while (start <= content.size()) {
    const std::size_t end = content.find('\n', start);
    const std::string_view line = end == std::string_view::npos
                                    ? content.substr(start)
                                    : content.substr(start, end - start);
    append_wrapped_line(out,
                        {StyledSegment{.text = expand_tabs(line),
                                       .color = ftxui::Color::GrayLight}},
                        StyledSegment{}, 0, width);
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
}

ftxui::Element
styled_piece(std::string_view value, const StyledSegment &segment,
             HighlightKind highlight) {
  using namespace ftxui;

  Element element = text(std::string{value});
  if (highlight == HighlightKind::Current) {
    element = element | color(Color::Black) | bgcolor(Color::MagentaLight);
  } else if (highlight == HighlightKind::Match) {
    element = element | color(Color::Black) | bgcolor(Color::YellowLight);
  } else if (segment.code) {
    element = element | color(Color::CyanLight);
  } else if (segment.link) {
    element = element | color(Color::CyanLight) | underlined;
  } else {
    element = element | color(segment.color);
  }

  if (segment.bold) {
    element = element | bold;
  }
  if (segment.italic) {
    element = element | italic;
  }
  if (segment.dim) {
    element = element | dim;
  }
  return element;
}

ftxui::Element
render_display_line(const DisplayLine &line, std::string_view search_query,
                    bool current_search_match) {
  using namespace ftxui;

  Elements pieces;
  for (const StyledSegment &segment : line) {
    if (segment.text.empty()) {
      continue;
    }
    const auto ranges = lupe::find_text_matches(segment.text, search_query);
    std::size_t cursor = 0;
    for (const auto &range : ranges) {
      if (range.offset > cursor) {
        pieces.push_back(
            styled_piece(segment.text.substr(cursor, range.offset - cursor),
                         segment, HighlightKind::None));
      }
      pieces.push_back(
          styled_piece(segment.text.substr(range.offset, range.length), segment,
                       current_search_match ? HighlightKind::Current
                                            : HighlightKind::Match));
      cursor = range.offset + range.length;
    }
    pieces.push_back(styled_piece(segment.text.substr(cursor), segment,
                                  HighlightKind::None));
  }

  if (pieces.empty()) {
    return text("");
  }
  return hbox(std::move(pieces));
}

ftxui::Element row_spacer(std::size_t rows) {
  return ftxui::text("")
       | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, static_cast<int>(rows));
}

void clamp_selection(AppState &state) {
  lupe::follow_selection(state.scroll);
  if (state.parsed.messages.empty()) {
    state.selected = 0;
    return;
  }
  state.selected = std::min(state.selected, state.parsed.messages.size() - 1);
}

void refresh_search_matches(AppState &state) {
  state.search_matches =
      lupe::find_message_matches(state.parsed.messages, state.search_query);
}

void restore_search_origin(AppState &state) {
  lupe::follow_selection(state.scroll);
  if (state.parsed.messages.empty()) {
    state.selected = 0;
    return;
  }
  state.selected =
      std::min(state.search_origin_selected, state.parsed.messages.size() - 1);
}

// Messages with an empty body and no annotations are already one line.
bool message_foldable(const lupe::LogMessage &message) {
  return !message.content.empty()
      || !message.annotations.empty()
      || !message.images.empty();
}

void ensure_fold_state_size(AppState &state) {
  if (state.folded.size() != state.parsed.messages.size()) {
    state.folded.assign(state.parsed.messages.size(), false);
  }
}

// A search match hidden inside a folded message would highlight nothing, so
// jumps onto a match open the fold they land in.
void unfold_message(AppState &state, std::size_t index) {
  if (index < state.folded.size() && state.folded[index]) {
    state.folded[index] = false;
    state.lines_width = -1; // Row geometry changed; rebuild display lines.
  }
}

void toggle_fold(AppState &state) {
  if (state.parsed.messages.empty()
      || state.selected >= state.parsed.messages.size()) {
    return;
  }
  if (!message_foldable(state.parsed.messages[state.selected])) {
    state.status = "nothing to fold";
    return;
  }
  ensure_fold_state_size(state);
  state.folded[state.selected] = !state.folded[state.selected];
  lupe::follow_selection(state.scroll);
  state.lines_width = -1;
  state.status.clear();
}

void set_all_folds(AppState &state, bool folded) {
  if (state.parsed.messages.empty()) {
    return;
  }
  ensure_fold_state_size(state);
  std::size_t changed = 0;
  for (std::size_t index = 0; index < state.parsed.messages.size(); ++index) {
    if (!message_foldable(state.parsed.messages[index])
        || state.folded[index] == folded) {
      continue;
    }
    state.folded[index] = folded;
    ++changed;
  }
  if (changed == 0) {
    state.status = folded ? "nothing to fold" : "no folds";
    return;
  }
  lupe::follow_selection(state.scroll);
  state.lines_width = -1;
  state.status = std::string{folded ? "folded " : "unfolded "}
               + std::to_string(changed)
               + " messages";
}

std::string search_progress(const AppState &state) {
  const std::string_view query = state.search_active
                                   ? std::string_view{state.search_input}
                                   : std::string_view{state.search_query};
  if (query.empty()) {
    return "";
  }

  const std::string label = "\"" + clipped_label(query) + "\"";
  if (state.search_matches.empty()) {
    return "0/0 " + label;
  }

  const auto ordinal =
      lupe::match_ordinal(state.search_matches, state.selected);
  if (!ordinal.has_value()) {
    return std::to_string(state.search_matches.size()) + " matches " + label;
  }

  return std::to_string(*ordinal + 1)
       + "/"
       + std::to_string(state.search_matches.size())
       + " "
       + label;
}

void begin_search(AppState &state) {
  lupe::follow_selection(state.scroll);
  state.search_active = true;
  state.search_origin_selected = state.selected;
  state.search_input.clear();
  state.search_matches.clear();
  state.status.clear();
}

void update_search_preview(AppState &state) {
  clamp_selection(state);
  restore_search_origin(state);
  state.status.clear();

  if (state.search_input.empty()) {
    state.search_matches.clear();
    return;
  }

  state.search_matches =
      lupe::find_message_matches(state.parsed.messages, state.search_input);
  const auto match =
      lupe::find_next_match(state.search_matches, state.search_origin_selected,
                             lupe::SearchDirection::Forward, true);
  if (match.has_value()) {
    state.selected = *match;
    unfold_message(state, *match);
  } else {
    restore_search_origin(state);
  }
}

void cancel_search(AppState &state) {
  state.search_active = false;
  clamp_selection(state);
  restore_search_origin(state);
  refresh_search_matches(state);
  state.status = "search canceled";
}

void commit_search(AppState &state) {
  state.search_active = false;
  if (state.search_input.empty()) {
    refresh_search_matches(state);
    restore_search_origin(state);
    state.status = "empty search";
    return;
  }

  state.search_query = state.search_input;
  refresh_search_matches(state);

  const auto match =
      lupe::find_next_match(state.search_matches, state.search_origin_selected,
                             lupe::SearchDirection::Forward, true);
  if (!match.has_value()) {
    restore_search_origin(state);
    state.status = "no matches";
    return;
  }

  state.selected = *match;
  unfold_message(state, *match);
  lupe::follow_selection(state.scroll);
  state.status.clear();
}

void jump_to_search_match(AppState &state, lupe::SearchDirection direction,
                          bool include_selected) {
  if (state.search_query.empty()) {
    state.status = "no active search";
    return;
  }

  refresh_search_matches(state);
  const auto match = lupe::find_next_match(
      state.search_matches, state.selected, direction, include_selected);
  if (!match.has_value()) {
    state.status = "no matches";
    return;
  }

  state.selected = *match;
  unfold_message(state, *match);
  lupe::follow_selection(state.scroll);
  state.status.clear();
}

void move_up(AppState &state, std::size_t amount) {
  lupe::follow_selection(state.scroll);
  state.selected -= std::min(state.selected, amount);
}

void move_down(AppState &state, std::size_t amount) {
  lupe::follow_selection(state.scroll);
  if (state.parsed.messages.empty()) {
    state.selected = 0;
    return;
  }
  const std::size_t last = state.parsed.messages.size() - 1;
  state.selected += std::min(last - state.selected, amount);
}

void reload(AppState &state) {
  state.inline_images.clear(std::cout);
  AppState loaded = load_viewer_state(state.path, state.format);
  state.session = std::move(loaded.session);
  state.parsed = std::move(loaded.parsed);
  if (state.session.has_fatal_error()) {
    state.show_diagnostics = true;
  } else if (state.parsed.errors.empty()) {
    state.show_diagnostics = false;
  }
  state.lines_width = -1; // Force a display-line rebuild.
  // Message indices may shift across a reload; folds do not carry over.
  state.folded.assign(state.parsed.messages.size(), false);
  state.overview_hovered.reset();
  clamp_selection(state);
  if (!state.search_query.empty()) {
    jump_to_search_match(state, lupe::SearchDirection::Forward, true);
  }
  state.status =
      "reloaded " + std::to_string(state.parsed.messages.size()) + " messages";
}

void open_diagnostics(AppState &state) {
  if (state.parsed.errors.empty()) {
    state.status = "no diagnostics";
    return;
  }
  state.show_diagnostics = true;
  state.overview_hovered.reset();
  state.status.clear();
  state.scroll.follow_focus = false;
  state.scroll.top_row = 0;
}

void close_diagnostics(AppState &state) {
  state.show_diagnostics = false;
  lupe::follow_selection(state.scroll);
}

bool handle_diagnostics_event(AppState &state, const ftxui::Event &event) {
  if (event == ftxui::Event::e
      || event == ftxui::Event::Escape
      || event == ftxui::Event::q) {
    close_diagnostics(state);
    return true;
  }
  if (event == ftxui::Event::j || event == ftxui::Event::ArrowDown) {
    lupe::scroll_by_rows(state.scroll, 1);
    return true;
  }
  if (event == ftxui::Event::k || event == ftxui::Event::ArrowUp) {
    lupe::scroll_by_rows(state.scroll, -1);
    return true;
  }
  if (event == ftxui::Event::PageDown) {
    lupe::scroll_by_rows(state.scroll, static_cast<int>(kPageMove));
    return true;
  }
  if (event == ftxui::Event::PageUp) {
    lupe::scroll_by_rows(state.scroll, -static_cast<int>(kPageMove));
    return true;
  }
  if (event == ftxui::Event::g || event == ftxui::Event::Home) {
    lupe::scroll_to_top(state.scroll);
    return true;
  }
  if (event == ftxui::Event::G || event == ftxui::Event::End) {
    lupe::scroll_to_bottom(state.scroll);
    return true;
  }
  if (event == ftxui::Event::r) {
    reload(state);
    return true;
  }
  // Swallow all other input while the diagnostics view is open.
  return true;
}

// One-line summary standing in for a folded message's body and
// annotations: a fold marker, the first non-empty content line clipped to
// fit, and the hidden source-line count. Muted throughout so folded blocks
// recede while skimming; the header above keeps the role color.
DisplayLine
fold_summary_line(const lupe::LogMessage &message, std::size_t width) {
  using ftxui::Color;

  // Mirror the body rendering's source text (see append_message_display_
  // lines) so the preview matches what unfolding would show.
  const bool verbatim_body =
      message.role == "tool" || message.role == "unknown";
  const std::string formatted =
      verbatim_body ? std::string{}
                    : lupe::format_structured_text(message.content);
  const std::string_view content = verbatim_body ? message.content : formatted;

  std::string_view rest{content};
  std::string_view preview;
  while (!rest.empty()) {
    const std::size_t end = rest.find('\n');
    const std::string_view line = rest.substr(0, end);
    if (!line.empty()) {
      preview = line;
      break;
    }
    rest = end == std::string_view::npos ? std::string_view{}
                                         : rest.substr(end + 1);
  }
  // Messages whose payload lives in annotations (tool calls carry an empty
  // content and the call in annotations) preview their first annotation
  // line instead.
  std::string annotation_preview;
  if (preview.empty()) {
    for (const std::string &annotation : message.annotations) {
      std::string_view annotation_rest{annotation};
      while (!annotation_rest.empty()) {
        const std::size_t end = annotation_rest.find('\n');
        const std::string_view line = annotation_rest.substr(0, end);
        if (!line.empty()) {
          annotation_preview = std::string{line};
          break;
        }
        annotation_rest = end == std::string_view::npos
                            ? std::string_view{}
                            : annotation_rest.substr(end + 1);
      }
      if (!annotation_preview.empty()) {
        break;
      }
    }
    preview = annotation_preview;
  }

  // Count every source line the fold hides: the body (or its "(empty)"
  // stand-in) plus the annotation lines.
  std::size_t hidden = 0;
  if (message.content.empty()) {
    hidden = 1;
  } else {
    hidden = static_cast<std::size_t>(
                 std::count(content.begin(), content.end(), '\n'))
           + 1;
  }
  for (const std::string &annotation : message.annotations) {
    hidden += static_cast<std::size_t>(
                  std::count(annotation.begin(), annotation.end(), '\n'))
            + 1;
  }

  const std::string marker = "\u25BA ";
  const std::string trailer = "  \u00B7 " + pluralize(hidden, "line", "lines");
  const std::size_t fixed =
      static_cast<std::size_t>(ftxui::string_width(marker))
      + static_cast<std::size_t>(ftxui::string_width(trailer));
  const std::size_t available = width > fixed ? width - fixed : 0;

  std::string preview_text;
  bool ellipsis = false;
  if (preview.empty()) {
    preview_text = clip_to_width("(empty)", available);
  } else {
    const std::string expanded = expand_tabs(preview);
    // The continuation mark earns its cell whenever content follows the
    // preview: more hidden lines, or the preview itself clipped short.
    ellipsis = hidden > 1;
    std::size_t budget = available;
    if (ellipsis && budget > 0) {
      --budget;
    }
    preview_text = clip_to_width(expanded, budget);
    if (!ellipsis && preview_text.size() < expanded.size()) {
      ellipsis = true;
      if (budget > 0) {
        --budget;
      }
      preview_text = clip_to_width(expanded, budget);
    }
    // The mark never overflows the budget itself at degenerate widths.
    if (ellipsis
        && static_cast<std::size_t>(ftxui::string_width(preview_text)) + 1
               > available) {
      ellipsis = false;
    }
  }

  DisplayLine line{
      StyledSegment{.text = marker, .color = Color::GrayLight, .dim = true},
      StyledSegment{.text = ellipsis ? preview_text + "\u2026" : preview_text,
                    .color = Color::GrayLight,
                    .dim = true},
      StyledSegment{.text = trailer, .color = Color::GrayDark, .dim = true},
  };
  // Overly narrow terminals sacrifice the trailer before the preview.
  truncate_display_line(line, width);
  return line;
}

// Expand one message into display lines: a single-line header, the wrapped
// body, wrapped annotations, and a trailing blank separator line. A folded
// message emits only the header, a one-line summary, and the separator.
void append_message_display_lines(const lupe::LogMessage &message,
                                  std::size_t width, bool folded,
                                  std::vector<DisplayLine> &out) {
  using ftxui::Color;

  DisplayLine header{
      StyledSegment{
          .text = message.role,
          .color = lupe::message_role_color(message.role),
          .bold = true,
      },
  };
  if (!message.timestamp.empty()) {
    header.push_back(StyledSegment{.text = "  " + message.timestamp,
                                   .color = Color::GrayLight,
                                   .dim = true});
  }
  if (message.source_line > 0) {
    header.push_back(StyledSegment{
        .text = "  line " + std::to_string(message.source_line), .dim = true});
  }
  if (!message.raw_type.empty() && message.raw_type != message.role) {
    header.push_back(StyledSegment{.text = "  " + message.raw_type,
                                   .color = Color::GrayLight,
                                   .dim = true});
  }
  truncate_display_line(header, width);
  out.push_back(std::move(header));

  if (folded && message_foldable(message)) {
    out.push_back(fold_summary_line(message, width));
    out.emplace_back(); // Blank separator between messages.
    return;
  }

  // Tool results and unknown payloads are raw output, not prose. Render
  // them verbatim; Markdown would eat comment markers (`/* */`), turn ` * `
  // lines into lists, and mangle indentation. They also skip
  // format_structured_text: it rewrites the two characters `\n` into a line
  // break, which corrupts literal escapes in command output (printf format
  // strings, regexes, code).
  const bool verbatim_body =
      message.role == "tool" || message.role == "unknown";
  const std::string formatted =
      verbatim_body ? std::string{}
                    : lupe::format_structured_text(message.content);
  const std::string_view content = verbatim_body ? message.content : formatted;
  if (message.content.empty()) {
    out.push_back({StyledSegment{.text = "(empty)", .dim = true}});
  } else if (verbatim_body) {
    append_verbatim_lines(content, width, out);
  } else {
    append_markdown_lines(content, width, out);
  }

  for (const std::string &annotation : message.annotations) {
    std::size_t start = 0;
    while (start <= annotation.size()) {
      const std::size_t end = annotation.find('\n', start);
      const std::string_view line =
          end == std::string_view::npos
              ? std::string_view{annotation}.substr(start)
              : std::string_view{annotation}.substr(start, end - start);
      append_wrapped_line(out,
                          {StyledSegment{.text = expand_tabs(line),
                                         .color = Color::YellowLight,
                                         .dim = true}},
                          StyledSegment{}, 0, width);
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
  }

  out.emplace_back(); // Blank separator between messages.
}

// Rebuild the cached display lines when the terminal width changed or the
// messages were reloaded. Returns true when a rebuild happened, meaning row
// geometry changed and any scroll offset predates it.
bool ensure_display_lines(AppState &state) {
  const int terminal_width = ftxui::Terminal::Size().dimx;
  if (terminal_width == state.lines_width && !state.display_lines.empty()) {
    return false;
  }
  state.lines_width = terminal_width;
  state.display_lines.clear();
  state.message_rows.clear();
  state.image_rows.clear();
  const std::size_t wrap_width = static_cast<std::size_t>(
      std::max(1, terminal_width - lupe::kMessageOverviewWidth - 3));
  for (std::size_t index = 0; index < state.parsed.messages.size(); ++index) {
    const std::size_t first_row = state.display_lines.size();
    const bool folded = index < state.folded.size() && state.folded[index];
    const auto &message = state.parsed.messages[index];
    append_message_display_lines(message, wrap_width, folded,
                                 state.display_lines);
    if (!folded && !message.images.empty()) {
      state.display_lines.pop_back(); // Put image cells before the separator.
      for (std::size_t image = 0; image < message.images.size(); ++image) {
        for (int row = 0; row < lupe::kKittyImageRows; ++row) {
          state.image_rows.emplace(state.display_lines.size(),
                                   ImageDisplayRow{image, row});
          state.display_lines.emplace_back();
        }
        state.display_lines.emplace_back();
      }
    }
    state.message_rows.emplace_back(first_row, state.display_lines.size());
  }
  return true;
}

std::size_t
message_row_owner(const std::vector<std::pair<std::size_t, std::size_t>> &rows,
                  std::size_t row) {
  std::size_t low = 0;
  std::size_t high = rows.size();
  while (low + 1 < high) {
    const std::size_t mid = low + (high - low) / 2;
    if (rows[mid].first <= row) {
      low = mid;
    } else {
      high = mid;
    }
  }
  return low;
}

// The display row to center for the selected message when it is the
// current search match but taller than the viewport: its first row where a
// segment contains the query, so the highlighted match lands on screen the
// way vim keeps the match line visible after n/N. kNoRow when whole-block
// centering applies — no active match, the block fits the viewport, or the
// match is in no single segment (spanning wrapped lines or styled segments),
// where the block anchor at least shows the message.
std::size_t search_focus_row(const AppState &state, std::string_view query,
                             int viewport_rows) {
  if (query.empty()
      || state.selected >= state.message_rows.size()
      || !std::ranges::binary_search(state.search_matches, state.selected)) {
    return kNoRow;
  }
  const auto [first_row, end_row] = state.message_rows[state.selected];
  if (end_row - first_row <= static_cast<std::size_t>(viewport_rows)) {
    return kNoRow;
  }
  for (std::size_t row = first_row; row < end_row; ++row) {
    for (const StyledSegment &segment : state.display_lines[row]) {
      if (!lupe::find_text_matches(segment.text, query).empty()) {
        return row;
      }
    }
  }
  return kNoRow;
}

std::string_view visible_query(const AppState &state) {
  return state.search_active ? std::string_view{state.search_input}
                             : std::string_view{state.search_query};
}

// Viewer counterpart of resolve_pending_browser_recenter: before a wheel
// delta is applied, resolve the recenter a pending selection jump would
// receive at the next layout, using the same geometry render() applies —
// including the search-match row focus, or the first wheel notch after n/N
// would snap a tall message from the centered match back to its header.
void resolve_pending_recenter(AppState &state) {
  if (!state.scroll.follow_focus || state.scroll.viewport_rows <= 0) {
    return;
  }
  ensure_display_lines(state);
  if (state.message_rows.empty()) {
    return;
  }
  state.selected = std::min(state.selected, state.message_rows.size() - 1);
  const auto [first_row, end_row] = state.message_rows[state.selected];
  const std::size_t match_row =
      search_focus_row(state, visible_query(state), state.scroll.viewport_rows);
  const int focus_first = match_row != kNoRow ? static_cast<int>(match_row)
                                              : static_cast<int>(first_row);
  const int focus_last = match_row != kNoRow ? static_cast<int>(match_row)
                                             : static_cast<int>(end_row) - 1;
  state.scroll.max_top_row = lupe::max_top_row_for(
      static_cast<int>(state.display_lines.size()), state.scroll.viewport_rows);
  state.scroll.top_row =
      std::clamp(lupe::centered_top_row(focus_first, focus_last,
                                         state.scroll.viewport_rows),
                 0, state.scroll.max_top_row);
}

// Viewer counterpart of sync_browser_selection_to_viewport: while the
// mouse wheel scrolls, the message at the top of the viewport is the
// current one.
void sync_selection_to_viewport(AppState &state) {
  if (state.scroll.viewport_rows <= 0) {
    return;
  }
  if (ensure_display_lines(state)) {
    // The rebuild rewrapped every row (terminal resize, reload): top_row is
    // still in the old geometry, so anchoring on it would select an
    // arbitrary message. The next layout re-clamps first.
    return;
  }
  if (state.message_rows.empty()) {
    return;
  }
  const std::size_t last = state.message_rows.size() - 1;
  const std::size_t top =
      static_cast<std::size_t>(std::max(state.scroll.top_row, 0));
  std::size_t owner = message_row_owner(state.message_rows, top);
  // When only a message's trailing blank separator remains at the top, its
  // content is entirely above the viewport; the topmost visible message is
  // the next one.
  if (state.message_rows[owner].second - 1 <= top && owner < last) {
    ++owner;
  }
  state.selected = owner;
}

bool handle_message_overview_mouse(AppState &state, ftxui::Event &event) {
  if (!event.is_mouse()) {
    return false;
  }
  if (state.show_diagnostics || state.parsed.messages.empty()) {
    state.overview_hovered.reset();
    return false;
  }

  const auto hovered = lupe::message_overview_index_at(
      state.overview_box, state.parsed.messages.size(), event.mouse().x,
      event.mouse().y);
  state.overview_hovered = hovered;
  if (!hovered.has_value()) {
    return false;
  }

  if (event.mouse().button == ftxui::Mouse::Left) {
    if (event.mouse().motion == ftxui::Mouse::Pressed) {
      state.selected = *hovered;
      lupe::follow_selection(state.scroll);
      state.status.clear();
      return true;
    }
    if (event.mouse().motion == ftxui::Mouse::Released) {
      return true;
    }
  }

  return event.mouse().motion == ftxui::Mouse::Moved;
}

ftxui::Element render_errors(const std::vector<std::string> &errors) {
  using namespace ftxui;

  Elements lines;
  lines.push_back(text("No messages parsed") | bold | color(Color::RedLight));
  lines.push_back(separatorEmpty());
  for (const auto &error : errors) {
    lines.push_back(paragraph(error) | color(Color::YellowLight));
  }
  return vbox(std::move(lines)) | flex;
}

ftxui::Element render_browser_entry(const BrowserEntry &entry, bool selected) {
  using namespace ftxui;

  const Color path_color = selected ? Color::MagentaLight : Color::GrayLight;
  const Element block = vbox({
      text(entry.display_path) | color(path_color),
      text(entry.modified_label) | color(Color::GrayDark),
  });

  if (selected) {
    return hbox({
               text("▌") | color(Color::MagentaLight),
               text(" "),
               block,
           })
         | focus;
  }

  return hbox({
      text("  "),
      block,
  });
}

ftxui::Element render_browser_empty(const BrowserState &state) {
  using namespace ftxui;

  Elements lines;
  if (state.entries.empty()) {
    lines.push_back(text("No log files found") | bold | color(Color::RedLight));
    lines.push_back(separatorEmpty());
    lines.push_back(
        paragraph(
            "Looking for .json, .jsonl, .ndjson, .log, and .zstd files under "
            + state.root.string())
        | color(Color::YellowLight));
  } else {
    lines.push_back(text("No matches") | bold | color(Color::YellowLight));
    lines.push_back(separatorEmpty());
    lines.push_back(paragraph("No file path matches \""
                              + clipped_label(browser_query(state))
                              + "\"")
                    | color(Color::GrayLight));
  }
  return vbox(std::move(lines)) | flex;
}

ftxui::Element render_browser(BrowserState &state) {
  using namespace ftxui;

  Elements rows;
  if (state.visible_entries.empty()) {
    rows.push_back(render_browser_empty(state));
  } else {
    for (std::size_t visible_index = 0;
         visible_index < state.visible_entries.size(); ++visible_index) {
      const BrowserEntry &entry =
          state.entries[state.visible_entries[visible_index]];
      rows.push_back(
          render_browser_entry(entry, visible_index == state.selected));
      rows.push_back(separatorEmpty());
    }
  }

  const std::string count = pluralize(state.entries.size(), "file", "files");
  const std::string cursor =
      state.visible_entries.empty()
          ? "0/0"
          : std::to_string(state.selected + 1)
                + "/"
                + std::to_string(state.visible_entries.size());

  Elements status_items{
      text(" Lupe ")
          | bold
          | color(Color::Black)
          | bgcolor(Color::MagentaLight),
      text("  " + count) | color(Color::GrayLight),
      text("  " + cursor) | color(Color::GrayLight),
      text("  " + state.root.string()) | color(Color::GrayLight) | xflex_shrink,
  };

  const std::string query = browser_query(state);
  if (!state.status.empty()) {
    status_items.push_back(
        text("  " + state.status) | color(Color::YellowLight) | xflex_shrink);
  }
  if (!query.empty()) {
    status_items.push_back(text("  filter \"" + clipped_label(query) + "\"")
                           | color(Color::CyanLight)
                           | xflex_shrink);
  }

  const Element help =
      state.search_active
          ? hbox({
                text("/") | bold | color(Color::CyanLight),
                text(state.search_input) | color(Color::White),
                text("  enter filter  esc cancel  backspace edit")
                    | color(Color::GrayDark),
            })
          : text("j/k move  PgUp/Dn  g/G first/last  / find  "
                 "enter open  r refresh  q quit")
                | color(Color::GrayDark);

  return vbox({
             hbox(std::move(status_items)),
             separatorEmpty(),
             lupe::line_frame(vbox(std::move(rows)) | vscroll_indicator,
                               state.scroll)
                 | flex,
             separatorEmpty(),
             help,
         })
       | flex;
}

ftxui::Element render(AppState &state, bool can_return_to_browser) {
  using namespace ftxui;

  const std::string_view visible_search_query =
      state.search_active ? std::string_view{state.search_input}
                          : std::string_view{state.search_query};

  Elements rows;
  if (state.show_diagnostics) {
    // Structured counterpart of the status line's one-line summary: the
    // full list with severity and source line broken out, complete
    // messages, and a fatal banner when the transcript was withheld.
    const bool has_fatal = state.session.has_fatal_error();
    std::string heading =
        pluralize(state.parsed.errors.size(), "diagnostic", "diagnostics");
    if (has_fatal) {
      heading += " \u2014 parsing stopped; transcript withheld";
    }
    rows.push_back(text(std::move(heading))
                   | bold
                   | color(has_fatal ? Color::RedLight : Color::YellowLight));
    rows.push_back(separatorEmpty());
    for (const auto &diagnostic : state.parsed.diagnostics) {
      const Color entry_color = diagnostic_color(diagnostic.severity);
      const bool fatal =
          diagnostic.severity == lupe::DiagnosticSeverity::Fatal;
      Element severity = text(lupe::severity_name(diagnostic.severity))
                       | color(entry_color)
                       | size(WIDTH, EQUAL, 9);
      if (fatal) {
        severity = severity | bold;
      }
      rows.push_back(hbox({
          std::move(severity),
          text(diagnostic.source_line > 0
                   ? "line " + std::to_string(diagnostic.source_line)
                   : "")
              | color(entry_color)
              | size(WIDTH, EQUAL, 10),
          paragraph(diagnostic.message) | color(entry_color) | xflex,
      }));
      rows.push_back(separatorEmpty());
    }
  } else if (state.parsed.messages.empty()) {
    rows.push_back(render_errors(state.parsed.errors));
  } else {
    ensure_display_lines(state);

    // Only instantiate elements for the rows intersecting the viewport
    // (plus a small overscan). Off-screen rows become fixed-height spacers,
    // which keeps the scroll geometry identical to a fully expanded list.
    //
    // When the scroller follows the selection, resolve the target top row
    // here with the same geometry LineFrame applies during layout. The
    // window must cover the rows LineFrame will show; emitting it from a
    // stale top_row would let the scroller move into spacer territory,
    // rendering blank rows.
    constexpr std::size_t kOverscan = 2;
    const std::size_t total_rows = state.display_lines.size();
    const int viewport_rows =
        state.scroll.viewport_rows > 0 ? state.scroll.viewport_rows : 60;

    // When the selected message is the current search match but taller
    // than the viewport, focus its first matching row instead of the whole
    // block (see search_focus_row); resolve_pending_recenter applies the
    // same geometry from the event handler.
    const std::size_t focus_match_row =
        state.scroll.follow_focus
            ? search_focus_row(state, visible_search_query, viewport_rows)
            : kNoRow;

    if (state.scroll.follow_focus
        && state.selected < state.message_rows.size()) {
      const auto selected_rows = state.message_rows[state.selected];
      const int focus_first = focus_match_row != kNoRow
                                ? static_cast<int>(focus_match_row)
                                : static_cast<int>(selected_rows.first);
      const int focus_last = focus_match_row != kNoRow
                               ? static_cast<int>(focus_match_row)
                               : static_cast<int>(selected_rows.second) - 1;
      const int target =
          lupe::centered_top_row(focus_first, focus_last, viewport_rows);
      state.scroll.top_row = std::clamp(
          target, 0,
          lupe::max_top_row_for(static_cast<int>(total_rows), viewport_rows));
    }
    const std::size_t top = static_cast<std::size_t>(
        std::clamp(state.scroll.top_row, 0, static_cast<int>(total_rows)));
    const std::size_t lo = top > kOverscan ? top - kOverscan : 0;
    const std::size_t hi = std::min(
        total_rows, top + static_cast<std::size_t>(viewport_rows) + kOverscan);

    if (lo > 0) {
      rows.push_back(row_spacer(lo));
    }
    // Rows of one message are emitted together; the selected message's rows
    // are wrapped and focused as a unit so scroll following centers the
    // whole message, not just its first line. The selected message's role
    // header stays visible: while manual scrolling clips the header above
    // the viewport with the body still on screen, the whole header line is
    // pinned to the top row (covering the body row that would render
    // there), and the selection bar rides it.
    std::size_t selection_bar_row = kNoRow;
    bool pin_selected_header = false;
    if (state.selected < state.message_rows.size()) {
      const auto [selected_first, selected_end] =
          state.message_rows[state.selected];
      if (selected_first >= top) {
        selection_bar_row = selected_first;
      } else if (top < selected_end) {
        pin_selected_header = true;
        selection_bar_row = top;
      }
    }
    Elements message_elements;
    std::size_t current_message =
        lo < hi ? message_row_owner(state.message_rows, lo) : 0;
    const auto flush_message = [&]() {
      if (message_elements.empty()) {
        return;
      }
      Element block = message_elements.size() == 1
                        ? std::move(message_elements.front())
                        : vbox(std::move(message_elements));
      // When a match row carries the focus, the block must not: LineFrame
      // centers on the focused element, and the render target above was
      // computed for the match row.
      if (current_message == state.selected && focus_match_row == kNoRow) {
        block = std::move(block) | focus;
      }
      rows.push_back(std::move(block));
      message_elements.clear();
    };
    std::vector<std::filesystem::path> image_base_directories;
    if (!state.session.session.cwd.empty()) {
      image_base_directories.emplace_back(state.session.session.cwd);
    }
    if (!state.path.parent_path().empty()) {
      image_base_directories.push_back(state.path.parent_path());
    }
    for (std::size_t row = lo; row < hi; ++row) {
      const std::size_t message_index =
          message_row_owner(state.message_rows, row);
      if (message_index != current_message) {
        flush_message();
        current_message = message_index;
      }
      const lupe::LogMessage &message = state.parsed.messages[message_index];
      const bool selected_message = message_index == state.selected;
      const bool current_search_match =
          selected_message
          && std::ranges::binary_search(state.search_matches, message_index);
      const std::size_t source_row =
          pin_selected_header && selected_message && row == selection_bar_row
              ? state.message_rows[message_index].first
              : row;
      Element line_element;
      const auto image_row = state.image_rows.find(source_row);
      if (image_row != state.image_rows.end()) {
        const auto &image = image_row->second;
        line_element = state.inline_images.line(
            message.images[image.image_index], image.row,
            std::max(1, state.lines_width - lupe::kMessageOverviewWidth - 3),
            image_base_directories, std::cout);
      } else {
        line_element =
            render_display_line(state.display_lines[source_row],
                                visible_search_query, current_search_match);
      }
      if (selected_message && row == selection_bar_row) {
        message_elements.push_back(hbox({
            text("▌") | color(lupe::message_role_color(message.role)),
            text(" "),
            std::move(line_element),
        }));
      } else {
        message_elements.push_back(hbox({
            text("  "),
            std::move(line_element),
        }));
      }
      if (selected_message && row == focus_match_row) {
        message_elements.back() = std::move(message_elements.back()) | focus;
      }
    }
    flush_message();
    if (hi < total_rows) {
      rows.push_back(row_spacer(total_rows - hi));
    }
  }

  const std::string count =
      std::to_string(state.parsed.messages.size()) + " messages";
  const std::string cursor =
      state.parsed.messages.empty()
          ? "0/0"
          : std::to_string(state.selected + 1)
                + "/"
                + std::to_string(state.parsed.messages.size());

  Elements status_items{
      text(" Lupe ")
          | bold
          | color(Color::Black)
          | bgcolor(Color::MagentaLight),
      text("  " + count) | color(Color::GrayLight),
      text("  " + cursor) | color(Color::GrayLight),
      text("  " + std::string{lupe::log_format_name(state.format)})
          | color(Color::GrayLight),
      // The filename identifies the open log; keep it whole and let the
      // optional trailers (inline error, status, search) shrink instead.
      text("  " + short_path(state.path)) | color(Color::GrayLight),
  };

  if (!state.parsed.errors.empty()) {
    status_items.push_back(text(" "));
    Element diagnostics_count = text(" "
                                     + pluralize(state.parsed.errors.size(),
                                                 "diagnostic", "diagnostics")
                                     + " ")
                              | color(Color::YellowLight);
    if (state.show_diagnostics) {
      diagnostics_count = diagnostics_count | inverted;
    }
    status_items.push_back(std::move(diagnostics_count));
    if (!state.show_diagnostics) {
      // The open diagnostics view already shows every error in full, so
      // only trail the first one while the view is closed.
      status_items.push_back(
          text("  " + clipped_label(state.parsed.errors.front()))
          | color(Color::YellowLight)
          | xflex_shrink);
    }
  }
  if (!state.status.empty()) {
    status_items.push_back(
        text("  " + state.status) | color(Color::YellowLight) | xflex_shrink);
  }
  const bool has_visible_search = state.search_active
                                    ? !state.search_input.empty()
                                    : !state.search_query.empty();
  if (has_visible_search) {
    const std::string prefix = state.search_active ? "  preview " : "  search ";
    status_items.push_back(text(prefix + search_progress(state))
                           | color(Color::CyanLight)
                           | xflex_shrink);
  }

  std::string help_text = "j/k move  g/G ends  / search  n/N  enter fold  "
                          "z/Z all  r reload";
  if (state.show_diagnostics) {
    help_text = "j/k scroll  g/G top/bottom  e close  r reload";
  } else if (!state.parsed.errors.empty()) {
    help_text += "  e diagnostics";
  }
  if (can_return_to_browser) {
    help_text += "  - files";
  }
  help_text += "  q quit";

  const Element help =
      state.search_active
          ? hbox({
                text("/") | bold | color(Color::CyanLight),
                text(state.search_input) | color(Color::White),
                text("  enter search  esc cancel  backspace edit")
                    | color(Color::GrayDark),
            })
          : text(help_text) | color(Color::GrayDark);

  const bool show_message_overview =
      !state.show_diagnostics && !state.parsed.messages.empty();
  Elements footer_items{help | flex};
  if (show_message_overview
      && state.overview_hovered.has_value()
      && *state.overview_hovered < state.parsed.messages.size()) {
    const std::size_t hovered = *state.overview_hovered;
    const lupe::LogMessage &message = state.parsed.messages[hovered];
    footer_items.push_back(text(" "
                                + message.role
                                + " "
                                + std::to_string(hovered + 1)
                                + "/"
                                + std::to_string(state.parsed.messages.size())
                                + "  click jump ")
                           | bold
                           | color(lupe::message_role_color(message.role)));
  }
  footer_items.push_back(text(" "));
  footer_items.push_back(lupe::scroll_progress_indicator(state.scroll)
                         | color(Color::GrayLight));
  const Element footer = hbox(std::move(footer_items));

  Element scroll_content = vbox(std::move(rows));
  if (!show_message_overview) {
    scroll_content = std::move(scroll_content) | vscroll_indicator;
  }
  Element body =
      lupe::line_frame(std::move(scroll_content), state.scroll) | flex;
  if (show_message_overview) {
    body =
        hbox({
            std::move(body) | flex,
            separatorEmpty(),
            lupe::message_overview(state.parsed.messages, state.selected,
                                    state.overview_hovered, state.overview_box),
        })
        | flex;
  }

  return vbox({
             hbox(std::move(status_items)),
             separatorEmpty(),
             std::move(body),
             separatorEmpty(),
             footer,
         })
       | flex;
}

bool handle_search_event(AppState &state, const ftxui::Event &event) {
  if (event == ftxui::Event::Escape) {
    cancel_search(state);
    return true;
  }
  if (event == ftxui::Event::Return) {
    commit_search(state);
    return true;
  }
  if (event == ftxui::Event::Backspace) {
    if (!state.search_input.empty()) {
      state.search_input.pop_back();
    }
    update_search_preview(state);
    return true;
  }
  if (event == ftxui::Event::CtrlU) {
    state.search_input.clear();
    update_search_preview(state);
    return true;
  }
  if (event.is_character()) {
    state.search_input += event.character();
    update_search_preview(state);
    return true;
  }

  return true;
}

bool
handle_event(AppState &state, ftxui::Event event, const ftxui::Closure &quit) {
  if (handle_message_overview_mouse(state, event)) {
    return true;
  }
  if (const int rows = wheel_rows(event); rows != 0) {
    if (state.show_diagnostics) {
      lupe::scroll_by_rows(state.scroll, rows);
      return true;
    }
    resolve_pending_recenter(state);
    const int previous_top_row = state.scroll.top_row;
    lupe::scroll_by_rows(state.scroll, rows);
    // A wheel event that could not move the viewport (content fits, or at
    // an edge) must not move the selection either.
    if (!state.search_active && state.scroll.top_row != previous_top_row) {
      sync_selection_to_viewport(state);
    }
    return true;
  }
  if (state.search_active) {
    return handle_search_event(state, event);
  }
  if (state.show_diagnostics) {
    return handle_diagnostics_event(state, event);
  }
  if (event == ftxui::Event::e) {
    open_diagnostics(state);
    return true;
  }
  if (event == ftxui::Event::Character('/')) {
    begin_search(state);
    return true;
  }
  if (event == ftxui::Event::n) {
    jump_to_search_match(state, lupe::SearchDirection::Forward, false);
    return true;
  }
  if (event == ftxui::Event::N) {
    jump_to_search_match(state, lupe::SearchDirection::Backward, false);
    return true;
  }
  if (event == ftxui::Event::Return) {
    toggle_fold(state);
    return true;
  }
  if (event == ftxui::Event::z) {
    set_all_folds(state, true);
    return true;
  }
  if (event == ftxui::Event::Z) {
    set_all_folds(state, false);
    return true;
  }
  if (event == ftxui::Event::q || event == ftxui::Event::Escape) {
    quit();
    return true;
  }
  if (event == ftxui::Event::j || event == ftxui::Event::ArrowDown) {
    move_down(state, 1);
    return true;
  }
  if (event == ftxui::Event::k || event == ftxui::Event::ArrowUp) {
    move_up(state, 1);
    return true;
  }
  if (event == ftxui::Event::PageDown) {
    move_down(state, kPageMove);
    return true;
  }
  if (event == ftxui::Event::PageUp) {
    move_up(state, kPageMove);
    return true;
  }
  if (event == ftxui::Event::g || event == ftxui::Event::Home) {
    move_up(state, state.parsed.messages.size());
    return true;
  }
  if (event == ftxui::Event::G || event == ftxui::Event::End) {
    move_down(state, state.parsed.messages.size());
    return true;
  }
  if (event == ftxui::Event::r) {
    reload(state);
    return true;
  }
  return false;
}

bool
handle_browser_search_event(BrowserState &state, const ftxui::Event &event) {
  if (event == ftxui::Event::Escape) {
    cancel_browser_search(state);
    return true;
  }
  if (event == ftxui::Event::Return) {
    commit_browser_search(state);
    return true;
  }
  if (event == ftxui::Event::Backspace) {
    if (!state.search_input.empty()) {
      state.search_input.pop_back();
    }
    update_browser_search_preview(state);
    return true;
  }
  if (event == ftxui::Event::CtrlU) {
    state.search_input.clear();
    update_browser_search_preview(state);
    return true;
  }
  if (event.is_character()) {
    state.search_input += event.character();
    update_browser_search_preview(state);
    return true;
  }
  return true;
}

void open_selected_file(ApplicationState &state) {
  const BrowserEntry *entry = selected_browser_entry(state.browser);
  if (entry == nullptr) {
    state.browser.status = "no file selected";
    return;
  }

  state.viewer.inline_images.clear(std::cout);
  state.viewer = load_viewer_state(entry->path, state.format);
  state.showing_browser = false;
}

bool handle_browser_event(ApplicationState &state, ftxui::Event event,
                          const ftxui::Closure &quit) {
  BrowserState &browser = state.browser;
  if (const int rows = wheel_rows(event); rows != 0) {
    resolve_pending_browser_recenter(browser);
    const int previous_top_row = browser.scroll.top_row;
    lupe::scroll_by_rows(browser.scroll, rows);
    // A wheel event that could not move the viewport must not move the
    // selection either.
    if (browser.scroll.top_row != previous_top_row) {
      sync_browser_selection_to_viewport(browser);
    }
    return true;
  }
  if (browser.search_active) {
    return handle_browser_search_event(browser, event);
  }
  if (event == ftxui::Event::Character('/')) {
    begin_browser_search(browser);
    return true;
  }
  if (event == ftxui::Event::Return) {
    open_selected_file(state);
    return true;
  }
  if (event == ftxui::Event::q || event == ftxui::Event::Escape) {
    quit();
    return true;
  }
  if (event == ftxui::Event::j || event == ftxui::Event::ArrowDown) {
    move_browser_down(browser, 1);
    return true;
  }
  if (event == ftxui::Event::k || event == ftxui::Event::ArrowUp) {
    move_browser_up(browser, 1);
    return true;
  }
  if (event == ftxui::Event::PageDown) {
    move_browser_down(browser, kPageMove);
    return true;
  }
  if (event == ftxui::Event::PageUp) {
    move_browser_up(browser, kPageMove);
    return true;
  }
  if (event == ftxui::Event::g || event == ftxui::Event::Home) {
    move_browser_up(browser, browser.visible_entries.size());
    return true;
  }
  if (event == ftxui::Event::G || event == ftxui::Event::End) {
    move_browser_down(browser, browser.visible_entries.size());
    return true;
  }
  if (event == ftxui::Event::r) {
    refresh_browser(browser);
    return true;
  }
  return false;
}

ftxui::Element render_app(ApplicationState &state) {
  auto document = state.showing_browser
                    ? render_browser(state.browser)
                    : render(state.viewer, state.browser_available);
  return state.viewer.inline_images.frame(std::move(document), std::cout);
}

bool handle_app_event(ApplicationState &state, ftxui::Event event,
                      const ftxui::Closure &quit) {
  if (state.showing_browser) {
    return handle_browser_event(state, std::move(event), quit);
  }
  if (state.browser_available
      && !state.viewer.search_active
      && (event == ftxui::Event::b || event == ftxui::Event::Character('-'))) {
    state.browser.status = "returned from " + short_path(state.viewer.path);
    state.showing_browser = true;
    return true;
  }
  return handle_event(state.viewer, std::move(event), quit);
}

std::filesystem::path current_directory_path() {
  std::error_code error;
  auto current = std::filesystem::current_path(error);
  if (!error) {
    return current;
  }
  return ".";
}

bool is_directory_path(const std::filesystem::path &path) {
  std::error_code error;
  const bool directory = std::filesystem::is_directory(path, error);
  return !error && directory;
}
} // namespace

int main(int argc, char **argv) {
  const CliParseResult cli = parse_cli_args(argc, argv);
  if (!cli.run) {
    return cli.exit_code;
  }

  const bool path_was_omitted = cli.options.path.empty();
  const std::filesystem::path start_path =
      path_was_omitted ? current_directory_path() : cli.options.path;

  ApplicationState state;
  // parse_cli_args fills in Auto when --format is absent; value_or keeps
  // this access safe even if that invariant ever changes.
  state.format = cli.options.format.value_or(lupe::LogFormat::Auto);
  if (path_was_omitted || is_directory_path(start_path)) {
    state.browser.root = start_path;
    state.browser_available = true;
    state.showing_browser = true;
    refresh_browser(state.browser);
  } else {
    state.viewer = load_viewer_state(start_path, state.format);
  }

  auto screen = ftxui::App::Fullscreen();
  screen.TrackMouse(true);
  const auto exit_loop = screen.ExitLoopClosure();
  const ftxui::Closure quit = [&] {
    state.viewer.inline_images.clear(std::cout);
    exit_loop();
  };

  auto component = ftxui::Renderer([&] { return render_app(state); });
  component |= ftxui::CatchEvent([&](ftxui::Event event) {
    return handle_app_event(state, std::move(event), quit);
  });

  {
    const lupe::ScopedSynchronizedOutput synchronized_output(std::cout);
    screen.Loop(component);
    state.viewer.inline_images.clear(std::cout);
  }
  return EXIT_SUCCESS;
}
