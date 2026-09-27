#include <cassert>
#include "eidolon/hub_onboarding_diagnostic.h"

int main() {
    eidolon::HubOnboardingDiagnostic d;
    d.BeginStage("directory-fetch");
    d.error = -1;
    d.http_status = 403;
    auto denied = d.RetryDetail("ESP_FAIL", 1, 10);
    assert(denied.find("directory-fetch") != std::string::npos);
    assert(denied.find("HTTP 403") != std::string::npos);
    // A failed new candidate must not inherit the first candidate's HTTP response.
    d.BeginStage("directory-fetch");
    auto no_response = d.RetryDetail("ESP_FAIL", 5, 120);
    assert(no_response.find("HTTP no response") != std::string::npos);
    assert(no_response.find("403") == std::string::npos);
    assert(no_response.find("Attempt 5 failed. Retry delay: 120s") != std::string::npos);
    // Preserve both an explicit code and the platform's symbolic error.
    assert(no_response.find("ESP_FAIL (0x") != std::string::npos);
    d = {};
    assert(d.error == 0 && d.http_status == 0);
    d.BeginStage("configuration-validate");
    d.http_status = 200;
    d.error = 0x108;
    auto invalid = d.RetryDetail("ESP_ERR_INVALID_RESPONSE", 2, 20);
    assert(invalid.find("configuration-validate") != std::string::npos);
    assert(invalid.find("0x108") != std::string::npos);
    assert(invalid.find("HTTP 200") != std::string::npos);
}
