// Credits: TwilitRealm

#pragma once

#include "document.hpp"

#include <chrono>
#include <deque>

namespace partyboard::ui {

class Overlay : public Document {
public:
    Overlay();

    void show() override;
    void update() override;

protected:
    bool handle_nav_command(Rml::Event &event, NavCommand cmd) override;

    Rml::Element *mFpsCounter = nullptr;
    Rml::Element *mPing = nullptr;
    Rml::Element *mPaused = nullptr;
    Rml::Element *mCountdown = nullptr;
    // Online drop vote (docs/ONLINE_DROP_VOTE.md).
    Rml::Element *mDropWait = nullptr;
    Rml::Element *mCtrlDrop = nullptr;
    bool mDropWaitShown = false;
    std::string mDropWaitText;
    std::string mDropWaitLogged;
    std::string mCtrlDropText;
    void update_drop_banners();
    int mCountdownShown = 0;
    bool mPauseHintShown = false;
    bool mPingHiddenForMinigame = false;
    Rml::Element *mMenuHint = nullptr;
    bool mMenuHintShown = false;
    bool mMenuBarOpen = false; // top-anchored boxes moved below the menu bar's tab band
    unsigned mPausedSeats = 0;
    Uint64 mPingLastUpdate = 0;
    std::string mPingLayout; // which computers (seat ranges) the ping box lists, for its log line
    Rml::Element *mCurrentToast = nullptr;
    Rml::Element *mControllerWarning = nullptr;
    Rml::Element *mMenuNotification = nullptr;
    clock::time_point mCurrentToastStartTime;
    clock::time_point mMenuNotificationStartTime;

    struct FpsFrameEvent {
        Uint64 endCounter;
        Uint64 processingTicks;
    };

    std::deque<FpsFrameEvent> mFpsFrameEvents;
    Uint64 mFpsSumTicks = 0;
    bool mFpsHavePrevCounter = false;
    Uint64 mFpsPrevCounter = 0;
    Uint64 mFpsLastUpdate = 0;

    void advance_fps_counter(float &outFps, Uint64 perfFreq);
};

} // namespace partyboard::ui
