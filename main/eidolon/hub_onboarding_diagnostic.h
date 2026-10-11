#pragma once

#include <cstdio>
#include <string>

namespace eidolon {

// Attempt-local evidence only. Never contains request bodies or credentials.
// Stage identifiers are stable diagnostic codes, not lifecycle decisions.
struct HubOnboardingDiagnostic {
    const char* stage = "trust-load";
    int error = 0;
    int http_status = 0;  // zero means no HTTP response recorded for this stage

    void BeginStage(const char* value) {
        stage = value;
        http_status = 0;
    }
    const char* RecoveryDetail() const {
        if (http_status == 429 || http_status >= 500) {
            return "Host service is temporarily unavailable. Retrying automatically; no setup needed";
        }
        if (http_status == 401 || http_status == 403) {
            return "Host could not verify this device. Retrying; check device access in the management app";
        }
        if (http_status == 0) {
            return "Cannot reach the Host. Retrying automatically; no setup needed";
        }
        return "Could not prepare the service. Retrying automatically";
    }
    std::string RetryDetail(const char* error_name, int attempt, int seconds) const {
        char text[224];
        const std::string http = http_status ? std::to_string(http_status) : "no response";
        std::snprintf(text, sizeof(text),
            "Hub step: %s\n%s (0x%x), HTTP %s\nAttempt %d failed. Retry delay: %ds",
            stage, error_name, static_cast<unsigned int>(error), http.c_str(),
            attempt, seconds);
        return text;
    }
};

}  // namespace eidolon
