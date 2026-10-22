#include "text_wrap.hpp"

#include <algorithm>
#include <cstddef>
#include <ftxui/screen/string.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lupe {
namespace {

struct WrapToken {
  std::string_view text;
  const StyledSegment *style{nullptr};
  bool space{false};
  std::size_t cells{0};
};

std::vector<WrapToken>
tokenize_for_wrap(const std::vector<StyledSegment> &segments) {
  std::vector<WrapToken> tokens;
  for (const StyledSegment &segment : segments) {
    std::size_t index = 0;
    while (index < segment.text.size()) {
      const std::size_t start = index;
      const bool space = segment.text[index] == ' ';
      while (index < segment.text.size()
             && (segment.text[index] == ' ') == space) {
        ++index;
      }
      const auto text =
          std::string_view{segment.text}.substr(start, index - start);
      tokens.push_back(WrapToken{
          .text = text,
          .style = &segment,
          .space = space,
          .cells = static_cast<std::size_t>(ftxui::string_width(text)),
      });
    }
  }
  return tokens;
}

StyledSegment styled_text(const StyledSegment &style, std::string_view text) {
  // Copy only the style, not the whole source string for each wrapped piece.
  return StyledSegment{
      .text = std::string{text},
      .color = style.color,
      .bold = style.bold,
      .italic = style.italic,
      .dim = style.dim,
      .code = style.code,
      .link = style.link,
  };
}

void fit_prefix(StyledSegment &prefix, std::size_t width) {
  if (static_cast<std::size_t>(ftxui::string_width(prefix.text)) <= width) {
    return;
  }
  std::string kept;
  std::size_t cells = 0;
  for (const auto &glyph : ftxui::Utf8ToGlyphs(prefix.text)) {
    const auto glyph_cells =
        static_cast<std::size_t>(ftxui::string_width(glyph));
    if (glyph_cells > width - cells) {
      break;
    }
    kept += glyph;
    cells += glyph_cells;
  }
  prefix.text = std::move(kept);
}

} // namespace

void append_wrapped_line(std::vector<DisplayLine> &out,
                         const std::vector<StyledSegment> &segments,
                         StyledSegment first_prefix,
                         std::size_t continuation_cells, std::size_t width) {
  width = std::max(width, std::size_t{1});
  // Decorative prefixes must leave room for text, including a wide glyph.
  const std::size_t prefix_budget = width - std::min(width, std::size_t{2});
  continuation_cells = std::min(continuation_cells, prefix_budget);
  fit_prefix(first_prefix, prefix_budget);

  DisplayLine line;
  std::size_t cells = 0;
  if (!first_prefix.text.empty()) {
    cells = static_cast<std::size_t>(ftxui::string_width(first_prefix.text));
    line.push_back(std::move(first_prefix));
  }
  bool fresh_line = false;
  bool has_text = false;
  const auto next_line = [&]() {
    out.push_back(std::move(line));
    line = DisplayLine{};
    if (continuation_cells > 0) {
      line.push_back(StyledSegment{
          .text = std::string(continuation_cells, ' '),
      });
    }
    cells = continuation_cells;
    fresh_line = true;
    has_text = false;
  };

  for (const WrapToken &token : tokenize_for_wrap(segments)) {
    if (token.space && fresh_line) {
      continue; // Drop separators at a soft wrap, not logical-line indentation.
    }
    if (token.cells > width - cells && has_text) {
      next_line();
      if (token.space) {
        continue;
      }
    }
    const auto append_piece = [&](std::string_view text,
                                  std::size_t text_cells) {
      if (text.empty()) {
        return;
      }
      line.push_back(styled_text(*token.style, text));
      cells += text_cells;
      fresh_line = false;
      has_text = has_text || !token.space;
    };
    if (token.cells <= width - cells) {
      append_piece(token.text, token.cells);
      continue;
    }

    // Long paths, identifiers, JSON, and source lines cannot rely on spaces.
    // Plain ASCII can be split directly without allocating a glyph per byte.
    if (token.cells == token.text.size()) {
      std::size_t offset = 0;
      while (offset < token.text.size()) {
        if (cells == width) {
          next_line();
        }
        const std::size_t count =
            std::min(width - cells, token.text.size() - offset);
        append_piece(token.text.substr(offset, count), count);
        offset += count;
      }
      continue;
    }

    std::string piece;
    std::size_t piece_cells = 0;
    for (const auto &glyph : ftxui::Utf8ToGlyphs(token.text)) {
      if (glyph.empty()) {
        continue; // FTXUI inserts a placeholder after each full-width glyph.
      }
      std::string_view text = glyph;
      auto glyph_cells = static_cast<std::size_t>(ftxui::string_width(glyph));
      if (glyph_cells > width) {
        // A two-cell glyph cannot render in a one-cell column. Make that
        // limitation visible rather than silently letting the screen eat it.
        text = "?";
        glyph_cells = 1;
      }
      if (glyph_cells > width - cells - piece_cells) {
        append_piece(piece, piece_cells);
        piece.clear();
        piece_cells = 0;
        next_line();
      }
      piece += text;
      piece_cells += glyph_cells;
    }
    append_piece(piece, piece_cells);
  }
  out.push_back(std::move(line));
}

} // namespace lupe
