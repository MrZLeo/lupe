#include "lupe/log_message.hpp"
#include "lupe/search.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

TEST_CASE("find text matches returns case-insensitive ranges", "[search]") {
  REQUIRE(lupe::find_text_matches("Hotel hotel HOT", "hot")
          == std::vector<lupe::SearchMatchRange>{
              {.offset = 0, .length = 3},
              {.offset = 6, .length = 3},
              {.offset = 12, .length = 3},
          });
  REQUIRE(lupe::find_text_matches("aaaa", "aa")
          == std::vector<lupe::SearchMatchRange>{
              {.offset = 0, .length = 2},
              {.offset = 2, .length = 2},
          });
  REQUIRE(lupe::find_text_matches("abc", "").empty());
  REQUIRE(lupe::find_text_matches("abc", "missing").empty());
}

TEST_CASE("search matches message fields case-insensitively", "[search]") {
  const lupe::LogMessage message{
      .role = "assistant",
      .content = "Booked the Riverside View Hotel.",
      .annotations = {"call reserve_room {\"city\":\"Boston\"}"},
      .timestamp = "2026-06-17T10:15:00Z",
      .raw_type = "message",
  };

  REQUIRE(lupe::message_matches(message, "riverside"));
  REQUIRE(lupe::message_matches(message, "ASSISTANT"));
  REQUIRE(lupe::message_matches(message, "reserve_room"));
  REQUIRE(lupe::message_matches(message, "10:15"));
  REQUIRE(lupe::message_matches(message, "MESSAGE"));
  REQUIRE_FALSE(lupe::message_matches(message, "missing"));
  REQUIRE_FALSE(lupe::message_matches(message, ""));
}

TEST_CASE("find message matches returns matching indexes", "[search]") {
  const std::vector<lupe::LogMessage> messages{
      {.role = "system", .content = "rules"},
      {.role = "user", .content = "Find hotels in Boston"},
      {.role = "assistant", .content = "Riverside View Hotel"},
      {.role = "tool", .content = "No flights"},
  };

  REQUIRE(lupe::find_message_matches(messages, "hotel")
          == std::vector<std::size_t>{1, 2});
  REQUIRE(lupe::find_message_matches(messages, "tool")
          == std::vector<std::size_t>{3});
  REQUIRE(lupe::find_message_matches(messages, "missing").empty());
  REQUIRE(lupe::find_message_matches(messages, "").empty());
}

TEST_CASE("find next match wraps in both directions", "[search]") {
  const std::vector<std::size_t> matches{1, 3, 7};

  REQUIRE(
      lupe::find_next_match(matches, 0, lupe::SearchDirection::Forward, true)
      == 1);
  REQUIRE(
      lupe::find_next_match(matches, 1, lupe::SearchDirection::Forward, true)
      == 1);
  REQUIRE(
      lupe::find_next_match(matches, 1, lupe::SearchDirection::Forward, false)
      == 3);
  REQUIRE(
      lupe::find_next_match(matches, 7, lupe::SearchDirection::Forward, false)
      == 1);

  REQUIRE(
      lupe::find_next_match(matches, 8, lupe::SearchDirection::Backward, true)
      == 7);
  REQUIRE(
      lupe::find_next_match(matches, 7, lupe::SearchDirection::Backward, true)
      == 7);
  REQUIRE(lupe::find_next_match(matches, 7, lupe::SearchDirection::Backward,
                                 false)
          == 3);
  REQUIRE(lupe::find_next_match(matches, 1, lupe::SearchDirection::Backward,
                                 false)
          == 7);
}

TEST_CASE("match ordinal reports selected match position", "[search]") {
  const std::vector<std::size_t> matches{1, 3, 7};

  REQUIRE(lupe::match_ordinal(matches, 1) == 0);
  REQUIRE(lupe::match_ordinal(matches, 7) == 2);
  REQUIRE_FALSE(lupe::match_ordinal(matches, 2).has_value());
}
