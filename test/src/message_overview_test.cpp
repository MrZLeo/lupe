#include "message_overview.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>
#include <optional>
#include <vector>

#include "lupe/log_message.hpp"

namespace {

int bar_width(const ftxui::Screen &screen, int row) {
  int width = 0;
  for (int column = 0; column < screen.dimx(); ++column) {
    if (screen.CellAt(column, row).character == "━") {
      ++width;
    }
  }
  return width;
}

} // namespace

TEST_CASE("message overview maps its full height onto the transcript",
          "[message_overview]") {
  const ftxui::Box box{2, 8, 4, 12};

  REQUIRE(lupe::message_overview_index_at(box, 21, 2, 4) == 0);
  REQUIRE(lupe::message_overview_index_at(box, 21, 5, 8) == 10);
  REQUIRE(lupe::message_overview_index_at(box, 21, 8, 12) == 20);
  REQUIRE_FALSE(lupe::message_overview_index_at(box, 21, 1, 8).has_value());
  REQUIRE_FALSE(lupe::message_overview_index_at(box, 0, 5, 8).has_value());
}

TEST_CASE("message overview packs entries contiguously and centers them",
          "[message_overview]") {
  const std::vector<lupe::LogMessage> messages{
      lupe::LogMessage{.role = "user"},
      lupe::LogMessage{.role = "assistant"},
      lupe::LogMessage{.role = "tool"},
  };
  ftxui::Box reflected;
  auto overview = lupe::message_overview(messages, 1, std::nullopt, reflected);
  auto screen = ftxui::Screen::Create(
      ftxui::Dimension::Fixed(lupe::kMessageOverviewWidth),
      ftxui::Dimension::Fixed(7));

  ftxui::Render(screen, overview);

  REQUIRE(reflected == (ftxui::Box{0, 6, 2, 4}));
  REQUIRE(bar_width(screen, 0) == 0);
  REQUIRE(bar_width(screen, 1) == 0);
  REQUIRE(bar_width(screen, 2) == 2);
  REQUIRE(bar_width(screen, 3) == 2);
  REQUIRE(bar_width(screen, 4) == 2);
  REQUIRE(bar_width(screen, 5) == 0);
  REQUIRE(bar_width(screen, 6) == 0);
  REQUIRE(screen.CellAt(4, 3).character != "━");
  REQUIRE(screen.CellAt(5, 3).character == "━");
  REQUIRE(screen.CellAt(6, 3).character == "━");
  REQUIRE(screen.CellAt(6, 3).bold);
  REQUIRE_FALSE(screen.CellAt(6, 3).dim);
  REQUIRE_FALSE(screen.CellAt(6, 2).bold);
  REQUIRE(screen.CellAt(6, 2).dim);
}

TEST_CASE("message overview stretches left toward the mouse focus",
          "[message_overview]") {
  const std::vector<lupe::LogMessage> messages{
      lupe::LogMessage{.role = "user"},
      lupe::LogMessage{.role = "assistant"},
      lupe::LogMessage{.role = "system"},
      lupe::LogMessage{.role = "tool"},
      lupe::LogMessage{.role = "unknown"},
      lupe::LogMessage{.role = "user"},
  };
  ftxui::Box reflected;
  auto overview = lupe::message_overview(messages, 5, 2, reflected);
  auto screen = ftxui::Screen::Create(
      ftxui::Dimension::Fixed(lupe::kMessageOverviewWidth),
      ftxui::Dimension::Fixed(9));

  ftxui::Render(screen, overview);

  REQUIRE(reflected == (ftxui::Box{0, 6, 1, 6}));
  REQUIRE(bar_width(screen, 1) == 3);
  REQUIRE(bar_width(screen, 2) == 5);
  REQUIRE(bar_width(screen, 3) == 7);
  REQUIRE(bar_width(screen, 4) == 5);
  REQUIRE(bar_width(screen, 5) == 3);
  REQUIRE(bar_width(screen, 6) == 2);
  REQUIRE(screen.CellAt(6, 1).foreground_color == ftxui::Color::MagentaLight);
  REQUIRE(screen.CellAt(6, 2).foreground_color == ftxui::Color::GreenLight);
  REQUIRE(screen.CellAt(6, 3).foreground_color == ftxui::Color::CyanLight);
  REQUIRE(screen.CellAt(6, 4).foreground_color == ftxui::Color::YellowLight);
  REQUIRE(screen.CellAt(0, 3).character == "━");
  REQUIRE(screen.CellAt(0, 2).character != "━");
  REQUIRE(screen.CellAt(6, 3).bold);
  REQUIRE_FALSE(screen.CellAt(6, 3).dim);
  REQUIRE_FALSE(screen.CellAt(6, 6).bold);
  REQUIRE(screen.CellAt(6, 6).dim);
}
