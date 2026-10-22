#include "text_wrap.hpp"

#include <catch2/catch_test_macros.hpp>
#include <ftxui/screen/string.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace {

std::string line_text(const lupe::DisplayLine &line) {
  std::string text;
  for (const auto &segment : line) {
    text += segment.text;
  }
  return text;
}

std::string joined_text(const std::vector<lupe::DisplayLine> &lines) {
  std::string text;
  for (const auto &line : lines) {
    text += line_text(line);
  }
  return text;
}

void
require_width(const std::vector<lupe::DisplayLine> &lines, std::size_t width) {
  for (const auto &line : lines) {
    REQUIRE(static_cast<std::size_t>(ftxui::string_width(line_text(line)))
            <= width);
  }
}

} // namespace

TEST_CASE("long unbroken tool text wraps without losing its tail",
          "[text_wrap]") {
  for (
      const std::string text : {
          "https://example.test/very/long/path/to/a/"
          "file.json?cursor=1234567890",
          R"({"result":{"path":"/a/long/path/to/a/file","value":"abcdefghijk"}})",
          "tools.read({path:'/a/long/path/without/"
          "spaces'});console.log(result);",
      }) {
    std::vector<lupe::DisplayLine> lines;
    lupe::append_wrapped_line(lines, {{.text = text}}, {}, 0, 13);
    require_width(lines, 13);
    REQUIRE(lines.size() > 1);
    CHECK(joined_text(lines) == text);
  }
}

TEST_CASE("wrapping retains more than twelve thousand bytes", "[text_wrap]") {
  const std::string text = std::string(50000, 'x') + "END_OF_LONG_OUTPUT";
  std::vector<lupe::DisplayLine> lines;
  lupe::append_wrapped_line(lines, {{.text = text}}, {}, 0, 80);
  require_width(lines, 80);
  REQUIRE(lines.size() > 600);
  CHECK(joined_text(lines) == text);
  CHECK(line_text(lines.back()).ends_with("END_OF_LONG_OUTPUT"));
}

TEST_CASE("long UTF-8 text wraps at glyph boundaries and display widths",
          "[text_wrap]") {
  const std::string text =
      "工具调用参数很长，需要继续折行。e\u0301界\u0301🙂完成";
  for (const std::size_t width : {3U, 5U, 8U}) {
    std::vector<lupe::DisplayLine> lines;
    lupe::append_wrapped_line(lines, {{.text = text}}, {}, 0, width);
    require_width(lines, width);
    CHECK(joined_text(lines) == text);
    for (const auto &line : lines) {
      const auto value = line_text(line);
      REQUIRE_FALSE(value.empty());
      CHECK_FALSE(value.starts_with("\u0301"));
      CHECK((static_cast<unsigned char>(value.front()) & 0xC0U) != 0x80U);
    }
  }
}

TEST_CASE("hard wrapping retains all segment styles", "[text_wrap]") {
  const lupe::StyledSegment original{
      .text = "abcdefghijklmnopqrstuvwxyz",
      .color = ftxui::Color::CyanLight,
      .bold = true,
      .italic = true,
      .dim = true,
      .code = true,
      .link = true,
  };
  std::vector<lupe::DisplayLine> lines;
  lupe::append_wrapped_line(lines, {original}, {}, 0, 7);
  require_width(lines, 7);
  CHECK(joined_text(lines) == original.text);
  for (const auto &line : lines) {
    for (const auto &segment : line) {
      CHECK(segment.color == original.color);
      CHECK(segment.bold);
      CHECK(segment.italic);
      CHECK(segment.dim);
      CHECK(segment.code);
      CHECK(segment.link);
    }
  }
}

TEST_CASE("wrapping preserves word boundaries and leading indentation",
          "[text_wrap]") {
  std::vector<lupe::DisplayLine> words;
  lupe::append_wrapped_line(words, {{.text = "alpha beta gamma"}}, {}, 0, 10);
  REQUIRE(words.size() == 2);
  CHECK(line_text(words[0]) == "alpha beta");
  CHECK(line_text(words[1]) == "gamma");

  std::vector<lupe::DisplayLine> code;
  lupe::append_wrapped_line(code, {{.text = "    abcdefghijkl"}}, {}, 0, 8);
  require_width(code, 8);
  REQUIRE(code.size() == 2);
  CHECK(line_text(code[0]) == "    abcd");
  CHECK(line_text(code[1]) == "efghijkl");
}

TEST_CASE("long words wrap with list and code continuation prefixes",
          "[text_wrap]") {
  std::vector<lupe::DisplayLine> lines;
  lupe::append_wrapped_line(lines, {{.text = "abcdefghijklmnopqrstuvwxyz"}},
                            {.text = "> "}, 2, 12);
  require_width(lines, 12);
  REQUIRE(lines.size() == 3);
  CHECK(line_text(lines[0]) == "> abcdefghij");
  CHECK(line_text(lines[1]) == "  klmnopqrst");
  CHECK(line_text(lines[2]) == "  uvwxyz");
}

TEST_CASE("narrow widths do not overflow on prefixes or loop forever",
          "[text_wrap]") {
  for (const std::size_t width : {0U, 1U, 2U, 3U}) {
    std::vector<lupe::DisplayLine> lines;
    lupe::append_wrapped_line(lines, {{.text = "abcdef"}},
                              {.text = "a very long prefix "}, 100, width);
    require_width(lines, width == 0 ? 1 : width);
    REQUIRE_FALSE(lines.empty());
    CHECK(line_text(lines.back()).ends_with('f'));
  }

  std::vector<lupe::DisplayLine> wide;
  lupe::append_wrapped_line(wide, {{.text = "工具"}}, {.text = "> "}, 2, 2);
  require_width(wide, 2);
  CHECK(joined_text(wide) == "工具");
}

TEST_CASE("a wide glyph has a visible fallback in a one-cell column",
          "[text_wrap]") {
  std::vector<lupe::DisplayLine> lines;
  lupe::append_wrapped_line(lines, {{.text = "a界b"}}, {}, 0, 1);
  require_width(lines, 1);
  CHECK(joined_text(lines) == "a?b");
}

TEST_CASE("wrapping appends empty and styled logical lines", "[text_wrap]") {
  std::vector<lupe::DisplayLine> lines;
  lupe::append_wrapped_line(lines, {}, {}, 0, 8);
  REQUIRE(lines.size() == 1);
  CHECK(line_text(lines[0]).empty());

  const std::vector<lupe::StyledSegment> segments{
      {.text = "red", .color = ftxui::Color::Red},
      {.text = "greenblue", .color = ftxui::Color::Green},
  };
  lupe::append_wrapped_line(lines, segments, {}, 0, 5);
  require_width(lines, 5);
  CHECK(joined_text(lines) == "redgreenblue");
  REQUIRE(lines.size() >= 3);
  CHECK(lines[1][0].color == ftxui::Color::Red);
  CHECK(lines.back().back().color == ftxui::Color::Green);
}
