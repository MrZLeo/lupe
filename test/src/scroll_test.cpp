#include "lupe/scroll.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("row scrolling updates the viewport immediately", "[scroll]") {
  lupe::ScrollState scroll;
  lupe::update_scroll_layout(scroll, 5, 20);

  lupe::scroll_by_rows(scroll, 1);

  REQUIRE_FALSE(scroll.follow_focus);
  REQUIRE(scroll.top_row == 6);

  lupe::scroll_by_rows(scroll, -2);
  REQUIRE(scroll.top_row == 4);
}

TEST_CASE("row scrolling clamps to content bounds", "[scroll]") {
  lupe::ScrollState scroll;
  lupe::update_scroll_layout(scroll, 0, 3);

  lupe::scroll_by_rows(scroll, -1);
  REQUIRE(scroll.top_row == 0);

  lupe::scroll_by_rows(scroll, 10);
  REQUIRE(scroll.top_row == 3);

  lupe::update_scroll_layout(scroll, 0, 1);
  REQUIRE(scroll.top_row == 1);
}

TEST_CASE("overscrolling does not delay the first reverse step", "[scroll]") {
  lupe::ScrollState scroll;
  lupe::update_scroll_layout(scroll, 0, 3);

  lupe::scroll_by_rows(scroll, 3);
  for (int event = 0; event < 1000; ++event) {
    lupe::scroll_by_rows(scroll, 1);
  }
  REQUIRE(scroll.top_row == scroll.max_top_row);

  lupe::scroll_by_rows(scroll, -1);
  REQUIRE(scroll.top_row == scroll.max_top_row - 1);
}

TEST_CASE("jump scrolling reaches the extremes", "[scroll]") {
  lupe::ScrollState scroll;
  lupe::update_scroll_layout(scroll, 0, 12);
  lupe::scroll_by_rows(scroll, 5);

  lupe::scroll_to_bottom(scroll);
  REQUIRE_FALSE(scroll.follow_focus);
  lupe::update_scroll_layout(scroll, 0, 12);
  REQUIRE(scroll.top_row == 12);

  // The jump survives a stale max_top_row: the next layout clamps to the
  // real bottom even when the content grew since the jump was requested.
  lupe::scroll_by_rows(scroll, -3);
  lupe::scroll_to_bottom(scroll);
  lupe::update_scroll_layout(scroll, 0, 40);
  REQUIRE(scroll.top_row == 40);

  lupe::scroll_to_top(scroll);
  REQUIRE_FALSE(scroll.follow_focus);
  REQUIRE(scroll.top_row == 0);
}

TEST_CASE("selection navigation resumes focus following", "[scroll]") {
  lupe::ScrollState scroll;
  lupe::update_scroll_layout(scroll, 2, 20);
  lupe::scroll_by_rows(scroll, 5);
  REQUIRE(scroll.top_row == 7);

  lupe::follow_selection(scroll);
  lupe::update_scroll_layout(scroll, 12, 20);

  REQUIRE(scroll.follow_focus);
  REQUIRE(scroll.top_row == 12);
}

TEST_CASE("scroll progress follows the visible viewport", "[scroll]") {
  lupe::ScrollState scroll;

  REQUIRE(lupe::scroll_progress_percent(scroll) == 100);

  lupe::update_scroll_layout(scroll, 0, 8);
  REQUIRE(lupe::scroll_progress_percent(scroll) == 0);

  lupe::scroll_by_rows(scroll, 3);
  REQUIRE(lupe::scroll_progress_percent(scroll) == 37);

  lupe::scroll_by_rows(scroll, 20);
  REQUIRE(lupe::scroll_progress_percent(scroll) == 100);
}
