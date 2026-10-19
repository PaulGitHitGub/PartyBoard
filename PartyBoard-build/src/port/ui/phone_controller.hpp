#pragma once

#include "component.hpp"

#include <RmlUi/Core.h>

namespace partyboard::ui {

// Settings > Input > Phone Controller: the link a phone on the same Wi-Fi opens
// to become player 1's controller, as a QR code, plus whether one is connected.
// The link comes from the phone-controller launcher (PARTYBOARD_PHONE_* in the
// environment) and this Mac's current Wi-Fi address, re-read while it is shown,
// so switching networks updates the code.
class PhoneControllerPanel : public Component {
public:
    explicit PhoneControllerPanel(Rml::Element *parent);
    void update() override;

private:
    void refresh();

    Rml::Element *mStatus = nullptr;
    Rml::Element *mStatusText = nullptr;
    Rml::Element *mQrTile = nullptr;
    Rml::Element *mQrImage = nullptr;
    Rml::Element *mLinkIntro = nullptr;
    Rml::Element *mLink = nullptr;
    Rml::Element *mHelp = nullptr;
    Rml::Element *mToggle = nullptr;
    Rml::String mShownUrl;
    Rml::String mShownStatus;
    uint64_t mNextRefresh = 0;
};

// Short status for the Phone Controller row in the left pane.
Rml::String phone_controller_status_label();

} // namespace partyboard::ui
