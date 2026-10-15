#ifndef PROJECT_MESSAGE_OVERVIEW_HPP_
#define PROJECT_MESSAGE_OVERVIEW_HPP_

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>

#include "lupe/log_message.hpp"

namespace lupe {

inline constexpr int kMessageOverviewWidth = 7;

ftxui::Color message_role_color(std::string_view role);

std::optional<std::size_t>
message_overview_index_at(const ftxui::Box &box, std::size_t message_count,
                          int x, int y);

ftxui::Element
message_overview(const std::vector<LogMessage> &messages, std::size_t selected,
                 std::optional<std::size_t> hovered, ftxui::Box &reflected_box);

} // namespace lupe

#endif // PROJECT_MESSAGE_OVERVIEW_HPP_
