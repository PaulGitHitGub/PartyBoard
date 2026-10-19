// Credits: TwilitRealm

#pragma once
#include "window.hpp"

namespace partyboard::ui {

class SettingsWindow : public Window {
public:
    SettingsWindow(bool prelaunch = false);

    void update() override;

protected:
    bool mPrelaunch;
#if defined(PARTYBOARD_HD_BETA)
    // PARTYBOARD_TEST_OPEN_UI=phone (screenshots): the row to select, and progress.
    Component *mTestPhoneRow = nullptr;
    int mTestPhoneStep = 0;
    Component *mTestVideoRow = nullptr; // =video: Window Mode
#endif
};

} // namespace partyboard::ui
