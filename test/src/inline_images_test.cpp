#include "image_test_data.hpp"
#include "inline_images.hpp"
#include "line_frame.hpp"
#include "lupe/kitty_graphics.hpp"

#include <catch2/catch_test_macros.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>
#include <sstream>
#include <string>
#include <utility>

namespace {
ftxui::Element
image_block(lupe::InlineImages &images, const lupe::DisplayImage &image,
            int columns, std::ostream &output) {
  ftxui::Elements rows;
  for (int row = 0; row < lupe::kKittyImageRows; ++row) {
    rows.push_back(images.line(image, row, columns, {}, output));
  }
  return ftxui::vbox(std::move(rows));
}

std::uint32_t image_id(const std::string &commands) {
  const auto begin = commands.find(",i=") + 3;
  return static_cast<std::uint32_t>(std::stoul(commands.substr(begin)));
}
} // namespace

TEST_CASE("inline images render as normal single-cell TUI rows",
          "[inline_images]") {
  lupe::InlineImages images;
  const auto image = image_test::image();
  std::ostringstream output;
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(6),
                                      ftxui::Dimension::Fixed(12));
  ftxui::Terminal::SetColorSupport(ftxui::Terminal::Palette256);
  auto document = images.frame(image_block(images, image, 6, output), output);
  ftxui::Render(screen, document);
  REQUIRE(output.str().contains("a=t,t=d,f=100"));
  CHECK(output.str().contains("a=p,U=1"));
  const auto id = image_id(output.str());
  const auto expected_color = ftxui::Color::RGB(
      static_cast<std::uint8_t>(id >> 16), static_cast<std::uint8_t>(id >> 8),
      static_cast<std::uint8_t>(id));
  for (int row = 0; row < 12; ++row) {
    for (int column = 0; column < 6; ++column) {
      const auto &cell = screen.CellAt(column, row);
      CHECK(cell.character == lupe::kitty_placeholder(row, column));
      CHECK(ftxui::string_width(cell.character) == 1);
      CHECK(cell.foreground_color == expected_color);
    }
  }
  CHECK(screen.ToString().contains("\x1b[38;2;"));

  output.str("");
  ftxui::Render(screen, document);
  CHECK_FALSE(
      output.str().contains("a=t")); // Redraws never resend image bytes.
  CHECK(output.str().contains("a=p,U=1"));
  images.clear(output);
  CHECK(output.str().contains("a=d,d=I,i=" + std::to_string(id)));
}

TEST_CASE(
    "inline images clip while scrolling and do not cover headers or footers",
    "[inline_images]") {
  lupe::InlineImages images;
  const auto image = image_test::image();
  std::ostringstream output;
  lupe::ScrollState scroll;
  scroll.follow_focus = false;
  scroll.top_row = 4;
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(8),
                                      ftxui::Dimension::Fixed(5));
  auto document = images.frame(
      ftxui::vbox({
          ftxui::text("header"),
          lupe::line_frame(image_block(images, image, 8, output), scroll)
              | ftxui::flex,
          ftxui::text("footer"),
      }),
      output);
  ftxui::Render(screen, document);
  CHECK(screen.CellAt(0, 0).character == "h");
  CHECK(screen.CellAt(0, 4).character == "f");
  CHECK(screen.CellAt(0, 1).character == lupe::kitty_placeholder(4, 0));
  CHECK(screen.CellAt(7, 3).character == lupe::kitty_placeholder(6, 7));
  CHECK(output.str().contains("c=8,r=12"));

  output.str("");
  lupe::scroll_by_rows(scroll, 1);
  ftxui::Render(screen, document);
  CHECK(screen.CellAt(0, 1).character == lupe::kitty_placeholder(5, 0));
  CHECK_FALSE(output.str().contains("a=t"));
}

TEST_CASE("resize virtual placements without retransmitting image data",
          "[inline_images]") {
  lupe::InlineImages images;
  const auto image = image_test::image();
  std::ostringstream output;
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(10),
                                      ftxui::Dimension::Fixed(12));
  ftxui::Render(screen,
                images.frame(image_block(images, image, 10, output), output));
  const auto id = image_id(output.str());
  output.str("");
  screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(4),
                                 ftxui::Dimension::Fixed(12));
  ftxui::Render(screen,
                images.frame(image_block(images, image, 4, output), output));
  CHECK(output.str().contains("i=" + std::to_string(id) + ",p=1,c=4,r=12"));
  CHECK_FALSE(output.str().contains("a=t"));
  CHECK(screen.CellAt(3, 11).character == lupe::kitty_placeholder(11, 3));
}

TEST_CASE("release invisible images on folds, view switches and reloads",
          "[inline_images]") {
  lupe::InlineImages images;
  const auto first = image_test::image();
  const auto second = image_test::image(image_test::bmp);
  std::ostringstream output;
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(8),
                                      ftxui::Dimension::Fixed(24));
  auto document =
      images.frame(ftxui::vbox({image_block(images, first, 8, output),
                                image_block(images, second, 8, output)}),
                   output);
  ftxui::Render(screen, document);
  const auto first_id = image_id(output.str());
  REQUIRE(output.str().contains("f=100"));
  REQUIRE(output.str().contains("f=32"));
  output.str("");
  ftxui::Render(screen,
                images.frame(image_block(images, second, 8, output), output));
  CHECK(output.str().contains("a=d,d=I,i=" + std::to_string(first_id)));
  CHECK_FALSE(output.str().contains("a=t"));

  output.str("");
  ftxui::Render(screen, images.frame(ftxui::text("folded"), output));
  CHECK(output.str().contains("a=d,d=I"));
  output.str("");
  ftxui::Render(screen, images.frame(ftxui::text("browser"), output));
  CHECK(output.str().empty());
  images.clear(output);
  CHECK(output.str().empty());
}

TEST_CASE("offscreen images are not decoded or transmitted",
          "[inline_images]") {
  lupe::InlineImages images;
  const auto image = image_test::image();
  std::ostringstream output;
  lupe::ScrollState scroll;
  scroll.follow_focus = false;
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(8),
                                      ftxui::Dimension::Fixed(2));
  auto document =
      images.frame(lupe::line_frame(ftxui::vbox({
                                         ftxui::text("first"),
                                         ftxui::text("second"),
                                         image_block(images, image, 8, output),
                                     }),
                                     scroll),
                   output);
  ftxui::Render(screen, document);
  CHECK(output.str().empty());
  lupe::scroll_by_rows(scroll, 2);
  ftxui::Render(screen, document);
  CHECK(output.str().contains("a=t,t=d"));
}

TEST_CASE("invalid or remote images remain text diagnostics without commands",
          "[inline_images]") {
  for (const auto image :
       {image_test::image("not base64!"),
        lupe::DisplayImage{.url = "https://example.com/image.png"}}) {
    lupe::InlineImages images;
    std::ostringstream output;
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80),
                                        ftxui::Dimension::Fixed(12));
    ftxui::Render(screen,
                  images.frame(image_block(images, image, 80, output), output));
    CHECK(output.str().empty());
    CHECK(screen.ToString().contains("[image:"));
    CHECK_FALSE(screen.ToString().contains("\U0010EEEE"));
  }
}
