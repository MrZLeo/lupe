#include "line_frame.hpp"
#include "lupe/scroll.hpp"

#include <algorithm>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/util/autoreset.hpp>
#include <memory>
#include <string>
#include <utility>

namespace lupe {
namespace {

class LineFrame final : public ftxui::Node {
public:
  LineFrame(ftxui::Element child, ScrollState &scroll)
      : Node(ftxui::Elements{std::move(child)}), scroll_(scroll) {}

  void ComputeRequirement() override {
    ftxui::Node::ComputeRequirement();
    if (!scroll_.follow_focus) {
      requirement_.focused.enabled = false;
    }
  }

  void SetBox(ftxui::Box box) override {
    ftxui::Node::SetBox(box);

    pending_box_ = box;
    const int external_height = box.y_max - box.y_min;
    scroll_.viewport_rows = external_height + 1;
    pending_internal_height_ = std::max(requirement_.min_y, external_height);
    pending_max_top_row_ =
        max_top_row_for(pending_internal_height_, scroll_.viewport_rows);
    pending_focused_top_row_ = 0;
    if (requirement_.focused.enabled) {
      const auto &focused = requirement_.focused.box;
      pending_focused_top_row_ =
          centered_top_row(focused.y_min, focused.y_max, scroll_.viewport_rows);
    }
    layout_pending_ = true;

    set_child_box();
  }

  void Check(ftxui::Node::Status *status) override {
    ftxui::Node::Status child_status;
    child_status.iteration = status->iteration;
    ftxui::Node::Check(&child_status);
    status->need_iteration |= child_status.need_iteration;

    // Wrapped elements refine their height over multiple layout iterations.
    // Preserve manual scroll state until the child reports a stable layout.
    if (!layout_pending_ || child_status.need_iteration) {
      return;
    }

    update_scroll_layout(scroll_, pending_focused_top_row_,
                         pending_max_top_row_);
    set_child_box();
    layout_pending_ = false;
  }

  void Render(ftxui::Screen &screen) override {
    const ftxui::AutoReset<ftxui::Box> stencil(
        &screen.stencil, ftxui::Box::Intersection(box_, screen.stencil));
    children_[0]->Render(screen);
  }

private:
  void set_child_box() {
    ftxui::Box child_box = pending_box_;
    child_box.y_min = pending_box_.y_min - scroll_.top_row;
    child_box.y_max =
        pending_box_.y_min + pending_internal_height_ - scroll_.top_row;
    children_[0]->SetBox(child_box);
  }

  ScrollState &scroll_;
  ftxui::Box pending_box_;
  int pending_internal_height_{0};
  int pending_focused_top_row_{0};
  int pending_max_top_row_{0};
  bool layout_pending_{false};
};

class ScrollProgressIndicator final : public ftxui::Node {
public:
  explicit ScrollProgressIndicator(const ScrollState &scroll)
      : scroll_(scroll) {}

  void ComputeRequirement() override {
    requirement_.min_x = 4;
    requirement_.min_y = 1;
  }

  void Render(ftxui::Screen &screen) override {
    auto label =
        ftxui::text(std::to_string(scroll_progress_percent(scroll_)) + "%")
        | ftxui::align_right;
    label->ComputeRequirement();
    label->SetBox(box_);
    label->Render(screen);
  }

private:
  const ScrollState &scroll_;
};

} // namespace

ftxui::Element line_frame(ftxui::Element content, ScrollState &scroll) {
  return std::make_shared<LineFrame>(std::move(content), scroll);
}

ftxui::Element scroll_progress_indicator(const ScrollState &scroll) {
  return std::make_shared<ScrollProgressIndicator>(scroll);
}

} // namespace lupe
