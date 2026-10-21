// Credits: TwilitRealm

#pragma once

#include "button.hpp"
#include "document.hpp"
#include "tab_bar.hpp"

#include <memory>

namespace partyboard::ui {

class MenuBar : public Document {
public:
    MenuBar();

    MenuBar(const MenuBar &) = delete;
    MenuBar &operator=(const MenuBar &) = delete;

    void show() override;
    void hide(bool close) override;
    void update() override;
    bool focus() override;
    bool visible() const override;

protected:
    bool handle_nav_command(Rml::Event &event, NavCommand cmd) override;

private:
    void update_safe_area() noexcept;
    void update_overflow() noexcept;

    Rml::Element *mRoot;
    std::unique_ptr<TabBar> mTabBar;
    std::unique_ptr<Button> mCloseButton;
    Insets mTabBarPadding;
    float mTopMargin = 0.f;
    Rml::Element *mPinned = nullptr;    // Quit and the close button, outside the scrolling strip
    Rml::Element *mFadeLeft = nullptr;  // edge fades with a chevron: more tabs that way
    Rml::Element *mFadeRight = nullptr;
    int mOverflowState = -1;            // last logged (more left) | (more right) << 1
    int mFocusedTabIndex = -1;
    int mDropTabIndex = -1; // online: "Continue without them" (docs/ONLINE_DROP_VOTE.md)
    void refresh_drop_tab();
};

} // namespace partyboard::ui
