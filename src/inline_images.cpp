#include "inline_images.hpp"
#include "loupe/kitty_graphics.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>
#include <memory>
#include <ostream>
#include <utility>
#include <vector>

namespace loupe {

class InlineImages::Row final : public ftxui::Node {
public:
  Row(InlineImages &owner, const DisplayImage &image, int row, int columns,
      std::span<const std::filesystem::path> base_directories,
      std::ostream &output)
      : owner_(owner), image_(image), row_(row),
        columns_(std::clamp(columns, 1, kKittyImageColumns)),
        base_directories_(base_directories.begin(), base_directories.end()),
        output_(output) {}

  void ComputeRequirement() override {
    requirement_.min_x = columns_;
    requirement_.min_y = 1;
    requirement_.flex_shrink_x = 1;
  }

  void Render(ftxui::Screen &screen) override {
    const auto visible = ftxui::Box::Intersection(box_, screen.stencil);
    if (visible.IsEmpty() || row_ < 0 || row_ >= kKittyImageRows) {
      return;
    }
    const int columns = std::min(columns_, box_.x_max - box_.x_min + 1);
    auto &entry = owner_.use(image_, columns, base_directories_, output_);
    if (!entry.error.empty()) {
      if (row_ == 0) {
        auto label = ftxui::text("[image: " + entry.error + "]")
                   | ftxui::color(ftxui::Color::YellowLight);
        label->ComputeRequirement();
        label->SetBox(box_);
        label->Render(screen);
      }
      return;
    }

    // Color is an integer image identifier, not a visual approximation. Do not
    // let FTXUI quantize it to a palette on SSH/tmux with incomplete TERM info.
    ftxui::Terminal::SetColorSupport(ftxui::Terminal::Color::TrueColor);
    const auto color =
        ftxui::Color::RGB(static_cast<std::uint8_t>(entry.id >> 16),
                          static_cast<std::uint8_t>(entry.id >> 8),
                          static_cast<std::uint8_t>(entry.id));
    for (int x = std::max(box_.x_min, visible.x_min);
         x <= std::min(box_.x_min + columns - 1, visible.x_max); ++x) {
      auto &cell = screen.CellAt(x, box_.y_min);
      cell.character = kitty_placeholder(row_, x - box_.x_min);
      cell.foreground_color = color;
    }
  }

private:
  InlineImages &owner_;
  const DisplayImage &image_;
  int row_;
  int columns_;
  std::vector<std::filesystem::path> base_directories_;
  std::ostream &output_;
};

class InlineImages::Frame final : public ftxui::Node {
public:
  Frame(InlineImages &owner, ftxui::Element document, std::ostream &output)
      : Node({std::move(document)}), owner_(owner), output_(output) {}

  void SetBox(ftxui::Box box) override {
    Node::SetBox(box);
    children_[0]->SetBox(box);
  }

  void Render(ftxui::Screen &screen) override {
    owner_.begin_frame();
    children_[0]->Render(screen);
    owner_.end_frame(output_);
    // Do not flush here: uploads/deletions precede FTXUI's text frame in the
    // existing synchronized-output buffer, giving one atomic terminal update.
  }

private:
  InlineImages &owner_;
  std::ostream &output_;
};

InlineImages::Entry &
InlineImages::use(const DisplayImage &image, int columns,
                  std::span<const std::filesystem::path> base_directories,
                  std::ostream &output) {
  auto [iterator, inserted] = entries_.try_emplace(&image);
  auto &entry = iterator->second;
  if (inserted) {
    const auto prepared = prepare_kitty_image(image, base_directories);
    if (!prepared) {
      entry.error = prepared.error;
    } else {
      // Keep IDs in the 24 bits represented by foreground RGB. Vary the
      // starting point across processes: tmux panes share the terminal's ID
      // namespace. Within this process, IDs also survive viewer reloads.
      static std::atomic<std::uint32_t> next_id{static_cast<std::uint32_t>(
          std::chrono::steady_clock::now().time_since_epoch().count())};
      entry.id = next_id.fetch_add(1) % 0xFFFFFF + 1;
      transmit_kitty_image(output, prepared, entry.id, tmux_);
    }
  }
  if (!entry.used && entry.id != 0) {
    // Updating the same virtual placement handles width changes without
    // resending pixels. Reissue once per frame, never once per image row.
    place_kitty_image(output, entry.id, columns, kKittyImageRows, tmux_);
  }
  entry.used = true;
  return entry;
}

void InlineImages::begin_frame() {
  for (auto &[image, entry] : entries_) {
    entry.used = false;
  }
}

void InlineImages::end_frame(std::ostream &output) {
  for (auto iterator = entries_.begin(); iterator != entries_.end();) {
    if (!iterator->second.used) {
      delete_kitty_image(output, iterator->second.id, tmux_);
      iterator = entries_.erase(iterator);
    } else {
      ++iterator;
    }
  }
}

void InlineImages::clear(std::ostream &output) {
  for (const auto &[image, entry] : entries_) {
    delete_kitty_image(output, entry.id, tmux_);
  }
  entries_.clear();
}

ftxui::Element
InlineImages::line(const DisplayImage &image, int row, int columns,
                   std::span<const std::filesystem::path> base_directories,
                   std::ostream &output) {
  return std::make_shared<Row>(*this, image, row, columns, base_directories,
                               output);
}

ftxui::Element
InlineImages::frame(ftxui::Element document, std::ostream &output) {
  return std::make_shared<Frame>(*this, std::move(document), output);
}

} // namespace loupe
