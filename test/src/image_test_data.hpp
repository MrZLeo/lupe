#ifndef LUPE_TEST_IMAGE_DATA_HPP_
#define LUPE_TEST_IMAGE_DATA_HPP_

#include <array>
#include <memory>
#include <string>
#include <string_view>

#include "lupe/log_message.hpp"

namespace image_test {

inline constexpr std::string_view png =
    "iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAYAAAD0In+KAAAADklEQVR4nGP4z8DwHwQB"
    "EPgD/U6VwW8AAAAASUVORK5CYII=";
inline constexpr std::string_view bmp =
    "Qk06AAAAAAAAADYAAAAoAAAAAQAAAAEAAAABABgAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "D/AA==";
// A one-pixel red JPEG, stripped of application metadata.
inline constexpr std::string_view jpeg =
    "/9j/wAARCAABAAEDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL"
    "/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAk"
    "M2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4"
    "eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ"
    "2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL"
    "/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAV"
    "YnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3"
    "eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX"
    "2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9sAQwACAgICAgIDAgIDBQMDAwUGBQUFBQYIBgYGBgYI"
    "CggICAgICAoKCgoKCgoKDAwMDAwMDg4ODg4PDw8PDw8PDw8P/9sAQwECAgIEBAQHBAQHEAsJ"
    "CxAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQ/90A"
    "BAAB/9oADAMBAAIRAxEAPwD4vooor+Uz/fw//9k=";
inline constexpr std::string_view gif =
    "R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7";

inline constexpr std::array<unsigned char, 58> bmp_bytes{
    0x42, 0x4D, 58, 0, 0, 0, 0, 0, 0,  0, 54, 0, 0, 0, 40, 0, 0,   0, 1, 0,
    0,    0,    1,  0, 0, 0, 1, 0, 24, 0, 0,  0, 0, 0, 4,  0, 0,   0, 0, 0,
    0,    0,    0,  0, 0, 0, 0, 0, 0,  0, 0,  0, 0, 0, 0,  0, 255, 0,
};

inline lupe::DisplayImage image(std::string_view data = png) {
  return lupe::DisplayImage{
      .inline_data = true,
      .base64_data = std::make_shared<const std::string>(data),
  };
}

} // namespace image_test

#endif // LUPE_TEST_IMAGE_DATA_HPP_
