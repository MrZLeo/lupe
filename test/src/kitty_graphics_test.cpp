#include "image_test_data.hpp"
#include "lupe/kitty_graphics.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

TEST_CASE("prepare PNG pixels for direct Kitty transmission",
          "[kitty_graphics]") {
  const auto prepared = lupe::prepare_kitty_image(image_test::image(), {});
  REQUIRE(prepared);
  CHECK(prepared.format == 100);
  CHECK(prepared.width == 2);
  CHECK(prepared.height == 1);
  CHECK(prepared.payload == image_test::png);

  std::string noncanonical{image_test::png};
  noncanonical.pop_back(); // Missing padding is accepted and canonicalized.
  noncanonical.insert(20, "\r\n ");
  const auto normalized =
      lupe::prepare_kitty_image(image_test::image(noncanonical), {});
  REQUIRE(normalized);
  CHECK(normalized.payload == prepared.payload);
}

TEST_CASE("decode non-PNG images to Kitty RGBA", "[kitty_graphics]") {
  auto image = image_test::image(image_test::bmp);
  image.mime_type = "image/png"; // Trust decoded bytes, not log MIME claims.
  const auto prepared = lupe::prepare_kitty_image(image, {});
  REQUIRE(prepared);
  CHECK(prepared.format == 32);
  CHECK(prepared.width == 1);
  CHECK(prepared.height == 1);
  CHECK(prepared.payload == "/wAA/w=="); // Opaque red.

  std::ostringstream output;
  lupe::transmit_kitty_image(output, prepared, 42);
  CHECK(output.str()
        == "\x1b_Ga=t,t=d,f=32,i=42,s=1,v=1,m=0,q=2;/wAA/w==\x1b\\");
}

TEST_CASE("decode JPEG and GIF images before using the Kitty wire format",
          "[kitty_graphics]") {
  for (const auto data : {image_test::jpeg, image_test::gif}) {
    const auto prepared =
        lupe::prepare_kitty_image(image_test::image(data), {});
    REQUIRE(prepared);
    CHECK(prepared.format == 32);
    CHECK(prepared.width == 1);
    CHECK(prepared.height == 1);
    CHECK(prepared.payload.size() == 8); // One RGBA pixel, base64 encoded.
  }
}

TEST_CASE("reject invalid image payloads before sending commands",
          "[kitty_graphics]") {
  for (const auto invalid :
       {"not base64!", "A", "A===", "eB==", "eA=", "eA==A", "=eA=", "eA===="}) {
    CAPTURE(invalid);
    const auto prepared =
        lupe::prepare_kitty_image(image_test::image(invalid), {});
    REQUIRE_FALSE(prepared);
    CHECK(prepared.error.contains("base64"));
    std::ostringstream output;
    lupe::transmit_kitty_image(output, prepared, 42);
    CHECK(output.str().empty());
  }
  for (const auto invalid : {"", "eA==", "iVBORw0KGgo="}) {
    CAPTURE(invalid);
    CHECK_FALSE(lupe::prepare_kitty_image(image_test::image(invalid), {}));
  }
  const auto missing =
      lupe::prepare_kitty_image(lupe::DisplayImage{.inline_data = true}, {});
  REQUIRE_FALSE(missing);
  CHECK(missing.error.contains("unavailable"));

  const auto oversized =
      image_test::image(std::string(32 * 1024 * 1024 + 1, 'A'));
  CHECK(lupe::prepare_kitty_image(oversized, {}).error.contains("limit"));
}

TEST_CASE("reject oversized image dimensions before decoding pixels",
          "[kitty_graphics]") {
  // The PNG header claims 8192 x 8192 pixels: well above the 16 MP limit.
  constexpr std::string_view oversized_png =
      "iVBORw0KGgoAAAANSUhEUgAAIAAAACAACAYAAAD0In+KAAAADklEQVR4nGP4z8DwHwQB"
      "EPgD/U6VwW8AAAAASUVORK5CYII=";
  const auto prepared =
      lupe::prepare_kitty_image(image_test::image(oversized_png), {});
  REQUIRE_FALSE(prepared);
  CHECK(prepared.error.contains("16 megapixel"));
}

TEST_CASE("read local image references without launching a viewer",
          "[kitty_graphics]") {
  const auto directory =
      std::filesystem::temp_directory_path()
      / ("lupe-kitty-test-"
         + std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
  REQUIRE(std::filesystem::create_directory(directory));
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{directory};

  const auto path = directory / "image data.bmp";
  {
    std::ofstream stream{path, std::ios::binary};
    stream.write(reinterpret_cast<const char *>(image_test::bmp_bytes.data()),
                 static_cast<std::streamsize>(image_test::bmp_bytes.size()));
    REQUIRE(stream.good());
  }
  const std::vector<std::filesystem::path> bases{directory / "missing",
                                                 directory};
  const auto relative = lupe::prepare_kitty_image(
      lupe::DisplayImage{.url = "image data.bmp"}, bases);
  REQUIRE(relative);
  CHECK(relative.payload == "/wAA/w==");
  const auto absolute =
      lupe::prepare_kitty_image(lupe::DisplayImage{.url = path.string()}, {});
  REQUIRE(absolute);
  CHECK(absolute.payload == relative.payload);

  std::string url_path = path.generic_string();
  if (!url_path.starts_with('/')) {
    url_path.insert(0, "/");
  }
  url_path.replace(url_path.find(' '), 1, "%20");
  for (const auto prefix : {"file://", "file://localhost"}) {
    const auto local_url = lupe::prepare_kitty_image(
        lupe::DisplayImage{.url = prefix + url_path}, {});
    REQUIRE(local_url);
    CHECK(local_url.payload == relative.payload);
  }
  CHECK(lupe::prepare_kitty_image(lupe::DisplayImage{.url = "missing.png"},
                                   bases)
            .error.contains("not found"));
}

TEST_CASE("never fetch image URLs or pass unsafe references to a shell",
          "[kitty_graphics]") {
  for (const auto url :
       {"https://example.com/image.png", "http://localhost/private.png"}) {
    const auto prepared =
        lupe::prepare_kitty_image(lupe::DisplayImage{.url = url}, {});
    REQUIRE_FALSE(prepared);
    CHECK(prepared.error.contains("not fetched"));
  }
  for (const auto url :
       {"file://host/image.png", "file:///tmp/%00.png", "file:///tmp/%GG.png",
        "file:///tmp/%2", "file://", "smb://host/image.png"}) {
    CAPTURE(url);
    CHECK_FALSE(
        lupe::prepare_kitty_image(lupe::DisplayImage{.url = url}, {}));
  }
  CHECK_FALSE(lupe::prepare_kitty_image(
      lupe::DisplayImage{.url = std::string{"image\0.png", 10}}, {}));
}

TEST_CASE("Kitty direct transfers use bounded base64 chunks and quiet replies",
          "[kitty_graphics]") {
  const lupe::KittyImage image{.payload = std::string(8200, 'A')};
  std::ostringstream output;
  lupe::transmit_kitty_image(output, image, 42);
  const std::string data = output.str();
  std::string_view remaining{data};
  std::string payload;
  int chunks = 0;
  while (!remaining.empty()) {
    REQUIRE(remaining.starts_with("\x1b_G"));
    const auto separator = remaining.find(';');
    const auto end = remaining.find("\x1b\\");
    REQUIRE(separator < end);
    const auto control = remaining.substr(3, separator - 3);
    const auto chunk = remaining.substr(separator + 1, end - separator - 1);
    CHECK(chunk.size() <= 4096);
    CHECK(chunk.size() % 4 == 0);
    CHECK(control.contains("q=2"));
    if (chunks == 0) {
      CHECK(control == "a=t,t=d,f=100,i=42,m=1,q=2");
    } else {
      CHECK_FALSE(control.contains("a="));
      CHECK_FALSE(control.contains("i="));
    }
    CHECK(control.contains(chunks < 2 ? "m=1" : "m=0"));
    payload.append(chunk);
    remaining.remove_prefix(end + 2);
    ++chunks;
  }
  CHECK(chunks == 3);
  CHECK(payload == image.payload);
}

TEST_CASE("virtual Kitty placements are resized and deleted by ID",
          "[kitty_graphics]") {
  std::ostringstream output;
  lupe::place_kitty_image(output, 42, 20, 12);
  lupe::delete_kitty_image(output, 42);
  CHECK(output.str()
        == "\x1b_Ga=p,U=1,i=42,p=1,c=20,r=12,q=2\x1b\\"
           "\x1b_Ga=d,d=I,i=42,q=2\x1b\\");
  output.str("");
  lupe::place_kitty_image(output, 0, 20, 12);
  lupe::place_kitty_image(output, 42, 0, 12);
  lupe::delete_kitty_image(output, 0);
  CHECK(output.str().empty());
}

TEST_CASE("wrap graphics commands for tmux passthrough", "[kitty_graphics]") {
  std::ostringstream output;
  lupe::transmit_kitty_image(output, lupe::KittyImage{.payload = "eA=="}, 42,
                              true);
  lupe::place_kitty_image(output, 42, 20, 12, true);
  lupe::delete_kitty_image(output, 42, true);
  CHECK(output.str()
        == "\x1bPtmux;\x1b\x1b_Ga=t,t=d,f=100,i=42,m=0,q=2;eA==\x1b\x1b\\\x1b\\"
           "\x1bPtmux;\x1b\x1b_Ga=p,U=1,i=42,p=1,c=20,r=12,q=2\x1b\x1b\\\x1b\\"
           "\x1bPtmux;\x1b\x1b_Ga=d,d=I,i=42,q=2\x1b\x1b\\\x1b\\");
}

TEST_CASE("Kitty placeholders explicitly encode source coordinates",
          "[kitty_graphics]") {
  CHECK(lupe::kitty_placeholder(0, 0) == "\U0010EEEE\u0305\u0305");
  CHECK(lupe::kitty_placeholder(1, 3) == "\U0010EEEE\u030D\u0310");
  CHECK(lupe::kitty_placeholder(11, 79) == "\U0010EEEE\u034C\u06E2");
  CHECK(lupe::kitty_placeholder(-1, 0).empty());
  CHECK(lupe::kitty_placeholder(0, 80).empty());
  CHECK(lupe::kitty_placeholder(12, 0).empty());
}
