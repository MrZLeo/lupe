#ifndef PROJECT_LOG_MESSAGE_HPP_
#define PROJECT_LOG_MESSAGE_HPP_

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace lupe {
struct DisplayImage {
  std::string mime_type;
  std::string url;
  bool inline_data{false};
  std::shared_ptr<const std::string> base64_data;
};

struct LogMessage {
  std::string role{"unknown"};
  std::string content;
  std::vector<std::string> annotations;
  std::string timestamp;
  std::string raw_type;
  std::size_t source_line{0};
  std::vector<DisplayImage> images;
};
} // namespace lupe

#endif // PROJECT_LOG_MESSAGE_HPP_
