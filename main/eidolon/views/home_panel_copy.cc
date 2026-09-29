#include "eidolon/views/home_panel_copy.h"

#include <cctype>
#include <cstring>

namespace eidolon::home_copy {

namespace {

// korvo-1 has no BOOT key: the recovery gesture is a long press on SET.
struct Entry {
    const char* english;
    const char* chinese;
};
constexpr Entry kTexts[] = {
    // Projected status lines (ui_state_mapper.cc, eidolon_ui_labels.cc).
    {"Finding service", "正在寻找主机"},
    {"Checking device", "正在核验设备"},
    {"Connecting", "正在连接主机"},
    {"Waiting for service", "等待服务配置"},
    {"Host unavailable", "主机不可达"},
    {"Reconnecting to Host", "正在重连主机"},
    // Scene details.
    {"Starting Eidolon...", "请稍候"},
    {"Loading UI assets...", "正在加载界面资源"},
    {"Connecting to Owner network...", "正在连接家庭网络"},
    {"Complete setup on your controller", "请在手机上完成配网"},
    {"Updating device...", "请勿断电"},
    {"Approve this device in Eidolon", "请在手机 Eidolon 中批准这台面板"},
    {"Preparing your service...", "正在准备服务"},
    {"Connection interrupted. Trying to resume...", "连接中断，正在尝试恢复"},
    {"Voice service disabled; reset is required", "语音服务已停用，需要重置设备"},
    {"Use the physical recovery procedure", "长按 SET 键进入恢复流程"},
    {"Service unavailable", "服务暂不可用"},
    {"Looking for your Hub...", "正在局域网内查找你的主机"},
    {"Confirming device access...", "正在确认设备权限"},
    {"Connecting to your service...", "正在连接服务"},
    {"Waiting for service configuration", "主机正在准备这台面板的服务"},
    {"Checking device status...", "正在检查设备状态"},
    {"Check Host and Wi-Fi. Retrying automatically.", "请检查主机和 Wi-Fi，正在自动重试"},
    {"Please wait. Reconnecting automatically.", "请稍候，正在自动重连"},
    {"Waiting for automatic connection", "等待自动连接"},
    {"No start input available", "没有可用的开始方式"},
    {"Talk input unavailable", "说话按键不可用"},
    // Runtime details (application.cc).
    {"Preparing secure device setup...", "正在准备安全配网"},
    {"Ready for secure device setup", "请用手机 Eidolon 添加这台面板"},
    {"Configuration recovery blocked. Hold BOOT to retry setup", "配置恢复受阻，长按 SET 键重新配网"},
    {"Validating Wi-Fi and Host...", "正在验证 Wi-Fi 和主机"},
    {"Wi-Fi and Host confirmed", "Wi-Fi 和主机已确认"},
    {"Finishing device setup...", "正在完成配网"},
    {"Returning to saved Wi-Fi. Hold BOOT to change network", "正在回连已保存的 Wi-Fi，长按 SET 键可更换网络"},
    {"Setup closed - long-press BOOT to reopen", "配网已关闭，长按 SET 键重新打开"},
    {"Wi-Fi disconnected", "Wi-Fi 已断开"},
    {"Owner network unavailable", "家庭网络不可用"},
    {"Downloading UI assets...", "正在下载界面资源"},
    {"Update complete. Restarting...", "更新完成，正在重启"},
    {"Firmware update failed", "固件更新失败"},
    {"Restoring the operational channel...", "正在恢复与主机的连接"},
    // Input hints (ui_input_profile.h).
    {"Press button to start", "按 SET 键开始"},
    {"Press button to end", "按 SET 键结束"},
};

bool StartsWith(const char* text, const char* prefix) {
    return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

}  // namespace

std::string Title(UiScene scene, const char* status_text) {
    const char* status = status_text != nullptr ? status_text : "";
    switch (scene) {
    case UiScene::Starting: return "正在启动";
    case UiScene::Loading: return "正在加载";
    case UiScene::Network: return "正在连接网络";
    case UiScene::Commissioning: return "设备配网";
    case UiScene::Updating: return "正在更新";
    case UiScene::WaitingApproval: return "等待批准";
    case UiScene::PreparingService:
    case UiScene::Reconnecting: {
        const std::string text = Text(status);
        if (text != status) return text;
        return scene == UiScene::Reconnecting ? "正在重新连接" : "正在准备服务";
    }
    case UiScene::Removed: return "设备已移除";
    case UiScene::RecoveryRequired: return "需要恢复";
    case UiScene::Error: return "暂不可用";
    case UiScene::Ready:
    case UiScene::OpeningConversation:
    case UiScene::Conversation:
    case UiScene::Ended:
        break;
    }
    return Text(status);
}

std::string Text(const char* english) {
    if (english == nullptr) return {};
    for (const Entry& entry : kTexts) {
        if (std::strcmp(entry.english, english) == 0) return entry.chinese;
    }
    // Details that carry a value after a fixed lead-in.
    static constexpr const char* kWifi = "Connecting to Wi-Fi";
    if (StartsWith(english, kWifi)) {
        std::string ssid = english + std::strlen(kWifi);
        while (!ssid.empty() && ssid.front() == ' ') ssid.erase(ssid.begin());
        if (ssid.size() >= 3 && ssid.compare(ssid.size() - 3, 3, "...") == 0) ssid.resize(ssid.size() - 3);
        return ssid.empty() ? "正在连接 Wi-Fi" : "正在连接 Wi-Fi「" + ssid + "」";
    }
    static constexpr const char* kFirmware = "Installing firmware";
    if (StartsWith(english, kFirmware)) {
        std::string version = english + std::strlen(kFirmware);
        while (!version.empty() && (version.front() == ' ' || version.front() == '.')) version.erase(version.begin());
        return version.empty() ? "正在安装固件，请勿断电" : "正在安装固件 " + version + "，请勿断电";
    }
    return english;
}

int ProgressPercent(const char* detail) {
    if (detail == nullptr || !std::isdigit(static_cast<unsigned char>(detail[0]))) return -1;
    int value = 0;
    const char* p = detail;
    for (; std::isdigit(static_cast<unsigned char>(*p)) && value <= 100; ++p) value = value * 10 + (*p - '0');
    return *p == '%' && value <= 100 ? value : -1;
}

}  // namespace eidolon::home_copy
