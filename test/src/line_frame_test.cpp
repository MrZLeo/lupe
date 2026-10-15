#include "line_frame.hpp"

#include <catch2/catch_test_macros.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <string>

#include "lupe/scroll.hpp"

TEST_CASE("manual scrolling survives wrapped layout passes", "[scroll]") {
  lupe::ScrollState scroll;
  auto frame = lupe::line_frame(
      ftxui::paragraph(
          "zero one two three four five six seven eight nine ten eleven twelve "
          "thirteen fourteen fifteen sixteen seventeen eighteen omega"),
      scroll);
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(12),
                                      ftxui::Dimension::Fixed(4));

  ftxui::Render(screen, frame);
  REQUIRE(scroll.max_top_row > 0);
  REQUIRE(!screen.ToString().contains("omega"));

  lupe::scroll_by_rows(scroll, scroll.max_top_row);
  const int bottom_row = scroll.top_row;

  ftxui::Render(screen, frame);

  REQUIRE(scroll.top_row == bottom_row);
  REQUIRE(scroll.top_row == scroll.max_top_row);
  REQUIRE(screen.ToString().contains("omega"));
}

TEST_CASE("progress indicator uses the layout from the current frame",
          "[scroll]") {
  lupe::ScrollState scroll;
  auto document = ftxui::vbox({
      lupe::line_frame(ftxui::vbox({
                            ftxui::text("zero"),
                            ftxui::text("one"),
                            ftxui::text("two"),
                            ftxui::text("three"),
                            ftxui::text("four"),
                        }),
                        scroll)
          | ftxui::flex,
      lupe::scroll_progress_indicator(scroll),
  });
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(8),
                                      ftxui::Dimension::Fixed(3));

  ftxui::Render(screen, document);
  REQUIRE(screen.ToString().contains("0%"));

  lupe::scroll_by_rows(scroll, scroll.max_top_row);
  ftxui::Render(screen, document);
  REQUIRE(screen.ToString().contains("100%"));
}
