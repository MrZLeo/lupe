#include "lupe/markdown_text.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("parse markdown inline spans", "[markdown_text]") {
  const auto blocks = lupe::parse_markdown_text(
      "hello **bold** and *em* plus `code` [link](https://example.test)");

  REQUIRE(blocks.size() == 1);
  REQUIRE(blocks[0].kind == lupe::MarkdownBlockKind::Paragraph);
  REQUIRE(blocks[0].spans.size() == 8);

  REQUIRE(blocks[0].spans[0].text == "hello ");
  REQUIRE(blocks[0].spans[1].text == "bold");
  REQUIRE(blocks[0].spans[1].bold);
  REQUIRE(blocks[0].spans[2].text == " and ");
  REQUIRE(blocks[0].spans[3].text == "em");
  REQUIRE(blocks[0].spans[3].italic);
  REQUIRE(blocks[0].spans[5].text == "code");
  REQUIRE(blocks[0].spans[5].code);
  REQUIRE(blocks[0].spans[7].text == "link");
  REQUIRE(blocks[0].spans[7].link_url == "https://example.test");
}

TEST_CASE("parse markdown block types", "[markdown_text]") {
  const auto blocks = lupe::parse_markdown_text("# Title **One**\n"
                                                 "- item `id`\n"
                                                 "1. ordered\n"
                                                 "> quoted *text*");

  REQUIRE(blocks.size() == 4);
  REQUIRE(blocks[0].kind == lupe::MarkdownBlockKind::Heading);
  REQUIRE(blocks[0].level == 1);
  REQUIRE(blocks[0].spans[1].bold);

  REQUIRE(blocks[1].kind == lupe::MarkdownBlockKind::ListItem);
  REQUIRE(blocks[1].marker == "-");
  REQUIRE(blocks[1].spans[1].code);

  REQUIRE(blocks[2].kind == lupe::MarkdownBlockKind::ListItem);
  REQUIRE(blocks[2].marker == "1.");

  REQUIRE(blocks[3].kind == lupe::MarkdownBlockKind::Quote);
  REQUIRE(blocks[3].spans[1].italic);
}

TEST_CASE("parse fenced markdown code block", "[markdown_text]") {
  const auto blocks = lupe::parse_markdown_text("```json\n"
                                                 "{\"a\":1}\n"
                                                 "```\n"
                                                 "after");

  REQUIRE(blocks.size() == 2);
  REQUIRE(blocks[0].kind == lupe::MarkdownBlockKind::CodeBlock);
  REQUIRE(blocks[0].code == "{\"a\":1}");
  REQUIRE(blocks[1].kind == lupe::MarkdownBlockKind::Paragraph);
  REQUIRE(blocks[1].spans[0].text == "after");
}

TEST_CASE("keep plain lines as separate paragraphs", "[markdown_text]") {
  const auto blocks = lupe::parse_markdown_text("one\ntwo");

  REQUIRE(blocks.size() == 2);
  REQUIRE(blocks[0].kind == lupe::MarkdownBlockKind::Paragraph);
  REQUIRE(blocks[0].spans[0].text == "one");
  REQUIRE(blocks[1].kind == lupe::MarkdownBlockKind::Paragraph);
  REQUIRE(blocks[1].spans[0].text == "two");
}
