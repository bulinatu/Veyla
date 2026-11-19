#pragma once

#include "core/types.h"

#include <string>
#include <vector>

namespace brp::next {

struct PageInfo {
    Page page = Page::Home;
    std::string title;
    std::string group;
    int icon = 0;
    bool enabled = true;
};

// Navigation state only. It knows nothing about Direct2D or the shell.
class NavigationModel {
public:
    NavigationModel();

    const std::vector<PageInfo>& Pages() const { return pages_; }
    Page Current() const { return current_; }
    const std::vector<Page>& History() const { return history_; }

    bool Select(Page page);
    bool Back();
    bool CanGoBack() const { return !history_.empty(); }

    void SetCollapsed(bool collapsed) { collapsed_ = collapsed; }
    bool Collapsed() const { return collapsed_; }

    const PageInfo* Find(Page page) const;

private:
    std::vector<PageInfo> pages_;
    std::vector<Page> history_;
    Page current_ = Page::Home;
    bool collapsed_ = false;
};

}  // namespace brp::next
