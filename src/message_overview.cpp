#include "message_overview.hpp"
#include "lupe/log_message.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/cell.hpp>
#include <ftxui/screen/screen.hpp>

namespace lupe {
namespace {

std::size_t index_for_row(std::size_t message_count, int height, int row) {
  if (message_count <= 1 || height <= 1) {
    return 0;
  }
  const auto scale = static_cast<long double>(message_count - 1)
                   / static_cast<long double>(height - 1);
  return std::min(message_count - 1,
                  static_cast<std::size_t>(
                      std::llround(static_cast<long double>(row) * scale)));
}

int row_for_index(std::size_t message_count, int height, std::size_t index) {
  if (height <= 1) {
    return 0;
  }
  if (message_count <= 1) {
    return (height - 1) / 2;
  }
  const auto scale = static_cast<long double>(height - 1)
                   / static_cast<long double>(message_count - 1);
  return std::clamp(
      static_cast<int>(std::llround(static_cast<long double>(index) * scale)),
      0, height - 1);
}

ftxui::Box
centered_track_box(const ftxui::Box &box, std::size_t message_count) {
  const int available_height = box.y_max - box.y_min + 1;
  if (available_height <= 0 || message_count == 0) {
    return ftxui::Box{box.x_min, box.x_max, box.y_min, box.y_min - 1};
  }

  const int scaled_height = std::max(
      1, static_cast<int>(static_cast<long long>(available_height) * 4 / 5));
  const int track_height = static_cast<int>(
      std::min(message_count, static_cast<std::size_t>(scaled_height)));
  const int top_margin = (available_height - track_height) / 2;
  const int track_top = box.y_min + top_margin;
  return ftxui::Box{box.x_min, box.x_max, track_top,
                    track_top + track_height - 1};
}

class MessageOverview final : public ftxui::Node {
public:
  MessageOverview(const std::vector<LogMessage> &messages, std::size_t selected,
                  std::optional<std::size_t> hovered, ftxui::Box &reflected_box)
      : messages_(messages), selected_(selected), hovered_(hovered),
        reflected_box_(reflected_box) {}

  void ComputeRequirement() override {
    requirement_.min_x = kMessageOverviewWidth;
    requirement_.min_y = 1;
    requirement_.flex_grow_y = 1;
    requirement_.flex_shrink_y = 1;
  }

  void SetBox(ftxui::Box box) override {
    ftxui::Node::SetBox(box);
    reflected_box_ = centered_track_box(box, messages_.size());
  }

  void Render(ftxui::Screen &screen) override {
    const ftxui::Box track = centered_track_box(box_, messages_.size());
    const int height = track.y_max - track.y_min + 1;
    const int width = track.x_max - track.x_min + 1;
    if (messages_.empty() || height <= 0 || width <= 0) {
      return;
    }

    std::vector<std::optional<std::size_t>> rows(
        static_cast<std::size_t>(height));
    if (messages_.size() <= static_cast<std::size_t>(height)) {
      for (std::size_t index = 0; index < messages_.size(); ++index) {
        rows[static_cast<std::size_t>(
            row_for_index(messages_.size(), height, index))] = index;
      }
    } else {
      for (int row = 0; row < height; ++row) {
        rows[static_cast<std::size_t>(row)] =
            index_for_row(messages_.size(), height, row);
      }
    }

    const std::optional<int> hovered_row =
        hovered_.has_value() && *hovered_ < messages_.size()
            ? std::optional<int>{row_for_index(messages_.size(), height,
                                               *hovered_)}
            : std::nullopt;
    const std::optional<int> selected_row =
        selected_ < messages_.size() ? std::optional<int>{row_for_index(
                                           messages_.size(), height, selected_)}
                                     : std::nullopt;

    for (int row = 0; row < height; ++row) {
      auto index = rows[static_cast<std::size_t>(row)];
      if (!index.has_value()) {
        continue;
      }

      const bool is_selected = selected_row == row;
      const bool is_hovered = hovered_row == row;
      if (is_selected && !hovered_row.has_value()) {
        index = selected_;
      }
      if (is_hovered) {
        index = *hovered_;
      }

      constexpr int kBarWidth = 2;
      constexpr int kHoveredBarWidth = 7;
      int requested_bar_width = kBarWidth;
      if (hovered_row.has_value()) {
        const std::size_t proximity =
            messages_.size() <= static_cast<std::size_t>(height)
                ? std::max(*index, *hovered_) - std::min(*index, *hovered_)
                : static_cast<std::size_t>(std::abs(row - *hovered_row));
        if (proximity <= 2) {
          requested_bar_width =
              kHoveredBarWidth - static_cast<int>(proximity) * 2;
        }
      }
      const int bar_width = std::min(requested_bar_width, width);
      const int bar_left = track.x_max - bar_width + 1;
      const bool emphasized =
          is_hovered || (!hovered_row.has_value() && is_selected);

      const ftxui::Color role_color =
          message_role_color(messages_[*index].role);
      for (int column = 0; column < bar_width; ++column) {
        ftxui::Cell &cell = screen.CellAt(bar_left + column, track.y_min + row);
        cell.character = "━";
        cell.foreground_color = role_color;
        cell.bold = emphasized;
        cell.dim = !emphasized;
      }
    }
  }

private:
  const std::vector<LogMessage> &messages_;
  std::size_t selected_;
  std::optional<std::size_t> hovered_;
  ftxui::Box &reflected_box_;
};

} // namespace

ftxui::Color message_role_color(std::string_view role) {
  if (role == "user") {
    return ftxui::Color::MagentaLight;
  }
  if (role == "assistant") {
    return ftxui::Color::GreenLight;
  }
  if (role == "system") {
    return ftxui::Color::CyanLight;
  }
  if (role == "tool") {
    return ftxui::Color::YellowLight;
  }
  return ftxui::Color::GrayLight;
}

std::optional<std::size_t>
message_overview_index_at(const ftxui::Box &box, std::size_t message_count,
                          int x, int y) {
  if (message_count == 0 || !box.Contain(x, y)) {
    return std::nullopt;
  }
  const int height = box.y_max - box.y_min + 1;
  if (height <= 0) {
    return std::nullopt;
  }
  return index_for_row(message_count, height, y - box.y_min);
}

ftxui::Element
message_overview(const std::vector<LogMessage> &messages, std::size_t selected,
                 std::optional<std::size_t> hovered,
                 ftxui::Box &reflected_box) {
  return std::make_shared<MessageOverview>(messages, selected, hovered,
                                           reflected_box);
}

} // namespace lupe
