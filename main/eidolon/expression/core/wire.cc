#include "wire.h"
#include <cJSON.h>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <memory>
namespace eidolon::expression {
namespace {
bool Keys(const cJSON* obj,std::initializer_list<std::string_view> allowed) {
    if (!cJSON_IsObject(obj)) return false;
    for (auto* item=obj->child;item;item=item->next) {
        if (!item->string) return false;
        bool known=false;
        for (auto key:allowed) if (key==item->string) known=true;
        if (!known) return false;
        for (auto* prior=obj->child;prior!=item;prior=prior->next)
            if (!std::strcmp(prior->string,item->string)) return false;
    }
    return true;
}
const cJSON* Get(const cJSON* obj,const char* key) { return cJSON_GetObjectItemCaseSensitive(obj,key); }
bool Text(const cJSON* item,std::string_view value,bool optional=false) {
    return (!item && optional) || (cJSON_IsString(item) && value==item->valuestring);
}
bool Number(const cJSON* item,uint32_t& out,uint32_t max,bool optional=false) {
    if (!item) return optional;
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) || item->valuedouble<0 ||
        item->valuedouble>max || std::floor(item->valuedouble)!=item->valuedouble) return false;
    out=static_cast<uint32_t>(item->valuedouble);return true;
}
bool Id(const cJSON* item,std::array<char,97>& out) {
    if (!cJSON_IsString(item)) return false;
    std::string_view value=item->valuestring;
    if (value.empty() || value.size()>96) return false;
    for (char c:value) if (!(c>='a' && c<='z') && !(c>='A' && c<='Z') &&
        !(c>='0' && c<='9') && c!='_' && c!='.' && c!=':' && c!='-') return false;
    std::memcpy(out.data(),value.data(),value.size());return true;
}
}
bool DecodePlan(std::string_view json,uint64_t token,WirePlan& output) {
    if (json.empty() || json.size()>kMaxPlanBytes || json.find('\0')!=std::string_view::npos ||
        json.find("\\u0000")!=std::string_view::npos) return false;
    const char* end=nullptr;
    std::unique_ptr<cJSON,decltype(&cJSON_Delete)> root(
        cJSON_ParseWithLengthOpts(json.data(),json.size(),&end,false),cJSON_Delete);
    if (!root || !end) return false;
    for (const char* p=end;p<json.data()+json.size();++p)
        if (*p!=' ' && *p!='\n' && *p!='\r' && *p!='\t') return false;
    auto* obj=root.get();
    if (!Keys(obj,{"schema_version","presentation_id","response_id","profile",
                   "catalog_revision","max_duration_ms","steps","on_finish"})) return false;
    uint32_t version=1;
    if (!Number(Get(obj,"schema_version"),version,1,true) || version!=1 ||
        !Text(Get(obj,"profile"),kProfile,true) ||
        !Text(Get(obj,"catalog_revision"),kCatalogRevision,true) ||
        !Text(Get(obj,"on_finish"),"resume_current_base",true)) return false;
    WirePlan candidate;candidate.plan.token=token;
    if (!Id(Get(obj,"presentation_id"),candidate.presentation_id) ||
        !Id(Get(obj,"response_id"),candidate.response_id) ||
        !Number(Get(obj,"max_duration_ms"),candidate.plan.max_duration_ms,kMaxDurationMs)) return false;
    auto* steps=Get(obj,"steps");
    if (!cJSON_IsArray(steps)) return false;
    int count=cJSON_GetArraySize(steps);
    if (count<1 || count>static_cast<int>(kMaxSteps)) return false;
    candidate.plan.count=count;
    for (int i=0;i<count;++i) {
        auto* entry=cJSON_GetArrayItem(steps,i);auto& step=candidate.plan.steps[i];
        if (!Keys(entry,{"gesture","variant","intensity","at_ms","duration_ms"})) return false;
        auto* gesture=Get(entry,"gesture");
        if (!cJSON_IsString(gesture)) return false;
        size_t index=0;
        while (index<kGestureNames.size() && kGestureNames[index]!=gesture->valuestring) ++index;
        if (index==kGestureNames.size()) return false;
        step.gesture=static_cast<Gesture>(index);
        auto* variant=Get(entry,"variant");
        if (!Text(variant,"default",true) && !Text(variant,"subtle")) return false;
        step.subtle=Text(variant,"subtle");
        auto* intensity=Get(entry,"intensity");
        if (intensity) {
            if (!cJSON_IsNumber(intensity) || !std::isfinite(intensity->valuedouble) ||
                intensity->valuedouble<0 || intensity->valuedouble>1) return false;
            step.intensity=static_cast<float>(intensity->valuedouble);
        }
        if (!Number(Get(entry,"at_ms"),step.at_ms,kMaxDurationMs,true) ||
            !Number(Get(entry,"duration_ms"),step.duration_ms,kMaxDurationMs)) return false;
    }
    if (!Runtime::Valid(candidate.plan)) return false;
    output=candidate;return true;
}
}
