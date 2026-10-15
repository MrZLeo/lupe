#include "lupe/synchronized_output.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <ios>
#include <ostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kBegin = "\x1b[?2026h";
constexpr std::string_view kEnd = "\x1b[?2026l";

std::string synchronized_frame(const std::string &payload) {
  return std::string(kBegin) + payload + std::string(kEnd);
}

class ShortWriteBuffer final : public std::streambuf {
public:
  std::string output;

protected:
  std::streamsize xsputn(const char *data, std::streamsize size) override {
    const std::streamsize accepted =
        short_next_write_ ? std::min<std::streamsize>(3, size) : size;
    short_next_write_ = false;
    output.append(data, static_cast<std::size_t>(accepted));
    return accepted;
  }

  int sync() override { return 0; }

private:
  bool short_next_write_{true};
};

} // namespace

TEST_CASE("synchronized output wraps each flush", "[synchronized-output]") {
  std::ostringstream output;

  {
    const lupe::ScopedSynchronizedOutput synchronized_output(output);
    output << "first";
    output << " frame" << std::flush;
    output << "second" << std::flush;
  }

  REQUIRE(output.str()
          == synchronized_frame("first frame") + synchronized_frame("second"));
}

TEST_CASE("synchronized output ignores empty flushes",
          "[synchronized-output]") {
  std::ostringstream output;

  {
    const lupe::ScopedSynchronizedOutput synchronized_output(output);
    output << std::flush;
  }

  REQUIRE(output.str().empty());
}

TEST_CASE("synchronized output preserves embedded nulls",
          "[synchronized-output]") {
  std::ostringstream output;
  const std::string payload("before\0after", 12);

  {
    const lupe::ScopedSynchronizedOutput synchronized_output(output);
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    output.flush();
  }

  REQUIRE(output.str() == synchronized_frame(payload));
}

TEST_CASE("synchronized output keeps the flush sentinel after the frame",
          "[synchronized-output]") {
  std::ostringstream output;
  const std::string payload("frame\0", 6);

  {
    const lupe::ScopedSynchronizedOutput synchronized_output(output);
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    output.flush();
  }

  REQUIRE(output.str() == synchronized_frame("frame") + '\0');
}

TEST_CASE("synchronized output forwards an empty flush sentinel",
          "[synchronized-output]") {
  std::ostringstream output;

  {
    const lupe::ScopedSynchronizedOutput synchronized_output(output);
    output.put('\0');
    output.flush();
  }

  REQUIRE(output.str() == std::string(1, '\0'));
}

TEST_CASE("nested synchronized output produces one frame",
          "[synchronized-output]") {
  std::ostringstream output;

  {
    const lupe::ScopedSynchronizedOutput outer(output);
    {
      const lupe::ScopedSynchronizedOutput inner(output);
      output << "nested" << std::flush;
    }
  }

  REQUIRE(output.str() == synchronized_frame("nested"));
}

TEST_CASE("synchronized output closes a partially written frame",
          "[synchronized-output]") {
  ShortWriteBuffer upstream;
  std::ostream output(&upstream);
  const std::string payload("frame\0", 6);

  {
    const lupe::ScopedSynchronizedOutput synchronized_output(output);
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    output.flush();
  }

  REQUIRE(upstream.output
          == std::string(kBegin.substr(0, 3)) + std::string(kEnd) + '\0');
}
