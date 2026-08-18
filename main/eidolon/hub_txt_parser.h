#ifndef EIDOLON_HUB_TXT_PARSER_H_
#define EIDOLON_HUB_TXT_PARSER_H_

#include <esp_err.h>

#include <map>
#include <string>

#include "hub_types.h"

namespace eidolon {

class HubTxtParser {
public:
    static esp_err_t Parse(const std::map<std::string, std::string>& entries,
                           AuthorityCandidateRecord& out);

private:
    static esp_err_t ValidateForTxtVers(const AuthorityCandidateRecord& record);
    static void ApplyKnownFields(AuthorityCandidateRecord& record);
    static bool HasUrlScheme(const std::string& url);
};

}  // namespace eidolon

#endif  // EIDOLON_HUB_TXT_PARSER_H_
