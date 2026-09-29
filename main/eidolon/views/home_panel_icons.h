#ifndef EIDOLON_HOME_PANEL_ICONS_H_
#define EIDOLON_HOME_PANEL_ICONS_H_

#include "eidolon/eidolon_ui_model.h"
#include "eidolon/smarthome/smarthome_tiles.h"

// The smart home panel's Material Icons (Apache-2.0, the same font the Eidolon
// mobile app ships), as UTF-8. Each line names the code point and the pixel
// sizes whose font carries it: scripts/eidolon/gen_home_panel_fonts.py reads
// this file, so a glyph added here and regenerated is in the right fonts.
//   20: font_home_icons_20, inline after text (fallback of the text font)
//   24: font_home_icons_24, tile discs, microphone, status line
//   48: font_home_icons_48, system page
#define HOME_ICON_LIGHTBULB        "\xee\x8d\xbb"  // U+E37B  24
#define HOME_ICON_POWER            "\xee\x93\xa3"  // U+E4E3  24 48
#define HOME_ICON_AC_UNIT          "\xee\x80\xb7"  // U+E037  24
#define HOME_ICON_FIRE             "\xee\x8e\x92"  // U+E392  24
#define HOME_ICON_SHOWER           "\xee\x96\xa0"  // U+E5A0  24
#define HOME_ICON_CURTAINS         "\xf3\xb0\x9e\x95"  // U+F0795  24
#define HOME_ICON_CURTAINS_CLOSED  "\xf3\xb0\x9e\x96"  // U+F0796  24
#define HOME_ICON_CHECKROOM        "\xee\x85\x9d"  // U+E15D  24
#define HOME_ICON_AIR              "\xee\x81\xa4"  // U+E064  24
#define HOME_ICON_WATER_DROP       "\xf3\xb0\x96\xa2"  // U+F05A2  24
#define HOME_ICON_TV               "\xee\x9a\x87"  // U+E687  24
#define HOME_ICON_SPEAKER          "\xee\x97\x9b"  // U+E5DB  24
#define HOME_ICON_LAUNDRY          "\xee\x8e\x98"  // U+E398  24
#define HOME_ICON_CLEANING         "\xee\x85\xa7"  // U+E167  24
#define HOME_ICON_RICE_BOWL        "\xee\x94\xb8"  // U+E538  24
#define HOME_ICON_KITCHEN          "\xee\x8d\x9e"  // U+E35E  24
#define HOME_ICON_LOCK             "\xee\x8e\xae"  // U+E3AE  24
#define HOME_ICON_LOCK_OPEN        "\xee\x8e\xb0"  // U+E3B0  24
#define HOME_ICON_VIDEOCAM         "\xee\x9a\xa8"  // U+E6A8  24
#define HOME_ICON_THERMOMETER      "\xee\x87\x89"  // U+E1C9  24
#define HOME_ICON_MIC              "\xee\x8f\xa1"  // U+E3E1  24
#define HOME_ICON_MIC_OFF          "\xee\x8f\xa5"  // U+E3E5  24
#define HOME_ICON_CHECK_CIRCLE     "\xee\x85\x99"  // U+E159  24
#define HOME_ICON_ERROR            "\xee\x88\xb7"  // U+E237  24 48
#define HOME_ICON_WARNING          "\xee\x9b\x8b"  // U+E6CB  24
#define HOME_ICON_QUESTION         "\xf3\xb0\x95\x95"  // U+F0555  24
#define HOME_ICON_SPARKLE          "\xee\x82\xb7"  // U+E0B7  24
#define HOME_ICON_SYNC             "\xee\x98\xaf"  // U+E62F  20 24 48
#define HOME_ICON_HOME             "\xee\x8c\x98"  // U+E318  48
#define HOME_ICON_ADD              "\xee\x81\x87"  // U+E047  20
#define HOME_ICON_REMOVE           "\xee\x94\x96"  // U+E516  20
#define HOME_ICON_CHEVRON_LEFT     "\xee\x85\x9e"  // U+E15E  20
#define HOME_ICON_CHEVRON_RIGHT    "\xee\x85\x9f"  // U+E15F  20
#define HOME_ICON_SETTINGS         "\xee\x95\xbf"  // U+E57F  20
#define HOME_ICON_CLOUD_OFF        "\xee\x85\xb3"  // U+E173  20 48
#define HOME_ICON_DOWNLOADING      "\xee\x88\x84"  // U+E204  48
#define HOME_ICON_WIFI             "\xee\x9b\xa7"  // U+E6E7  48
#define HOME_ICON_PHONELINK_SETUP  "\xee\x92\xb3"  // U+E4B3  48
#define HOME_ICON_SYSTEM_UPDATE    "\xee\x98\xb6"  // U+E636  48
#define HOME_ICON_HOURGLASS        "\xee\x8c\xa5"  // U+E325  48
#define HOME_ICON_HUB              "\xf3\xb0\x94\x9d"  // U+F051D  48
#define HOME_ICON_LINK_OFF         "\xee\x8e\x81"  // U+E381  48
#define HOME_ICON_RESTART          "\xee\x94\xb1"  // U+E531  48

namespace eidolon::home_icons {

// Size 24.
inline const char* Device(smarthome::DeviceIcon icon) {
    using smarthome::DeviceIcon;
    switch (icon) {
    case DeviceIcon::Light: return HOME_ICON_LIGHTBULB;
    case DeviceIcon::Power: return HOME_ICON_POWER;
    case DeviceIcon::Cooling: return HOME_ICON_AC_UNIT;
    case DeviceIcon::Heating: return HOME_ICON_FIRE;
    case DeviceIcon::WaterHeater: return HOME_ICON_SHOWER;
    case DeviceIcon::CurtainOpen: return HOME_ICON_CURTAINS;
    case DeviceIcon::CurtainClosed: return HOME_ICON_CURTAINS_CLOSED;
    case DeviceIcon::DryingRack: return HOME_ICON_CHECKROOM;
    case DeviceIcon::Fan:
    case DeviceIcon::Purifier: return HOME_ICON_AIR;
    case DeviceIcon::Humidifier: return HOME_ICON_WATER_DROP;
    case DeviceIcon::Tv: return HOME_ICON_TV;
    case DeviceIcon::Speaker: return HOME_ICON_SPEAKER;
    case DeviceIcon::Washer: return HOME_ICON_LAUNDRY;
    case DeviceIcon::Vacuum: return HOME_ICON_CLEANING;
    case DeviceIcon::RiceCooker: return HOME_ICON_RICE_BOWL;
    case DeviceIcon::Appliance: return HOME_ICON_KITCHEN;
    case DeviceIcon::Locked: return HOME_ICON_LOCK;
    case DeviceIcon::Unlocked: return HOME_ICON_LOCK_OPEN;
    case DeviceIcon::Camera: return HOME_ICON_VIDEOCAM;
    case DeviceIcon::Thermometer: return HOME_ICON_THERMOMETER;
    }
    return HOME_ICON_POWER;
}

// Size 48: the picture for a system page.
inline const char* Hero(UiScene scene) {
    switch (scene) {
    case UiScene::Starting: return HOME_ICON_POWER;
    case UiScene::Loading: return HOME_ICON_DOWNLOADING;
    case UiScene::Network: return HOME_ICON_WIFI;
    case UiScene::Commissioning: return HOME_ICON_PHONELINK_SETUP;
    case UiScene::Updating: return HOME_ICON_SYSTEM_UPDATE;
    case UiScene::WaitingApproval: return HOME_ICON_HOURGLASS;
    case UiScene::PreparingService: return HOME_ICON_HUB;
    case UiScene::Reconnecting: return HOME_ICON_SYNC;
    case UiScene::Removed: return HOME_ICON_LINK_OFF;
    case UiScene::RecoveryRequired: return HOME_ICON_RESTART;
    case UiScene::Error: return HOME_ICON_ERROR;
    case UiScene::Ready:
    case UiScene::OpeningConversation:
    case UiScene::Conversation:
    case UiScene::Ended:
        break;
    }
    return HOME_ICON_HOME;
}

}  // namespace eidolon::home_icons

#endif  // EIDOLON_HOME_PANEL_ICONS_H_
