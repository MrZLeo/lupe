#include "lupe/structured_text.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("format Python-style dict repr", "[structured_text]") {
  const auto formatted = lupe::format_structured_text(
      "{'First Name': 'Emma', 'Address': '123 Main Street, Anytown, USA', "
      "'Credit Card Number': '4237-4252-7456-2574'}");

  REQUIRE(formatted
          == "{\n"
             "  'First Name': 'Emma',\n"
             "  'Address': '123 Main Street, Anytown, USA',\n"
             "  'Credit Card Number': '4237-4252-7456-2574'\n"
             "}");
}

TEST_CASE("format nested JSON-like text", "[structured_text]") {
  const auto formatted = lupe::format_structured_text(
      R"({"args":{"city":"Boston"},"ids":[1,2]})");

  REQUIRE(formatted
          == "{\n"
             "  \"args\": {\n"
             "    \"city\": \"Boston\"\n"
             "  },\n"
             "  \"ids\": [\n"
             "    1,\n"
             "    2\n"
             "  ]\n"
             "}");
}

TEST_CASE("format escaped newlines inside string values", "[structured_text]") {
  const auto formatted = lupe::format_structured_text(
      "{'Riverside View': 'Rating: 4.6\\nReviews: Beautiful hotel\\nExcellent "
      "location'}");

  REQUIRE(formatted
          == "{\n"
             "  'Riverside View': 'Rating: 4.6\n"
             "    Reviews: Beautiful hotel\n"
             "    Excellent location'\n"
             "}");
}

TEST_CASE("preserve escaped backslash sequences inside string values",
          "[structured_text]") {
  const auto formatted = lupe::format_structured_text(
      R"({"path":"C:\\newfolder","pattern":"\\n"})");

  REQUIRE(formatted
          == "{\n"
             "  \"path\": \"C:\\\\newfolder\",\n"
             "  \"pattern\": \"\\\\n\"\n"
             "}");
}

TEST_CASE("normalize line continuations in plain text", "[structured_text]") {
  REQUIRE(lupe::format_structured_text("Use the tools. \\\nNext line.")
          == "Use the tools. \nNext line.");
  REQUIRE(lupe::format_structured_text("One\\nTwo") == "One\nTwo");
}

TEST_CASE("leave plain text alone", "[structured_text]") {
  REQUIRE(lupe::format_structured_text("Hotel Names:\nRiverside View Hotel")
          == "Hotel Names:\nRiverside View Hotel");
}
