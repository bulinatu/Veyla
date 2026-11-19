#include "app/navigation.h"

#include <algorithm>

namespace brp::next {

NavigationModel::NavigationModel()
    : pages_{
          {Page::Home, "Home", "Overview", 0, true},
          {Page::Console, "Console", "Streaming", 1, true},
          {Page::Streaming, "Streaming", "Streaming", 2, true},
          {Page::AiVideo, "AI Video Engine", "Processing", 3, true},
          {Page::Display, "Display", "Processing", 4, true},
          {Page::Overlay, "Overlay", "Workspace", 5, true},
          {Page::Profiles, "Profiles", "Workspace", 6, true},
          {Page::Diagnostics, "Diagnostics", "Workspace", 7, true},
          {Page::Settings, "Settings", "Application", 8, true},
      } {}

const PageInfo* NavigationModel::Find(Page page) const {
    const auto it = std::find_if(pages_.begin(), pages_.end(),
                                 [page](const PageInfo& info) { return info.page == page; });
    return it == pages_.end() ? nullptr : &*it;
}

bool NavigationModel::Select(Page page) {
    const PageInfo* info = Find(page);
    if (!info || !info->enabled || page == current_) {
        return false;
    }
    history_.push_back(current_);
    if (history_.size() > 32) {
        history_.erase(history_.begin());
    }
    current_ = page;
    return true;
}

bool NavigationModel::Back() {
    if (history_.empty()) {
        return false;
    }
    current_ = history_.back();
    history_.pop_back();
    return true;
}

}  // namespace brp::next
