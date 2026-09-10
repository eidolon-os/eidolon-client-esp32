#include "mdns.h"
#include "eidolon/hub_discovery.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

mdns_result_t* supplied = nullptr;
int queries = 0;

int main() {
    mdns_txt_item_t wanted_txt[] = {{"txtvers", "1"}, {"owner_domain_id", "owner-wanted"},
        {"owner_domain_descriptor_uri", "https://moved.local/descriptor"}};
    mdns_txt_item_t other_txt[] = {{"txtvers", "1"}, {"owner_domain_id", "owner-other"},
        {"owner_domain_descriptor_uri", "https://other.local/descriptor"}};
    mdns_result_t wanted{nullptr, "owner-wanted", 9443, 3, wanted_txt};
    mdns_result_t other{nullptr, "owner-other", 9443, 3, other_txt};
    eidolon::HubDiscovery discovery;
    eidolon::AuthorityCandidateRecord selected;
    for (bool other_first : {false, true}) {
        wanted.next = other_first ? nullptr : &other;
        other.next = other_first ? &wanted : nullptr;
        supplied = other_first ? &other : &wanted;
        assert(discovery.Discover(selected, "owner-wanted") == ESP_OK);
        assert(selected.owner_domain_id == "owner-wanted");
    }
    // No unrelated candidate escapes even if it is the only answer.
    supplied = &other; other.next = nullptr;
    assert(discovery.Discover(selected, "owner-wanted") == ESP_ERR_NOT_FOUND);
    assert(selected.owner_domain_id.empty());

    eidolon::AuthorityCandidateRecord commissioned;
    commissioned.owner_domain_id = "owner-wanted";
    commissioned.owner_domain_descriptor_uri = "https://saved.local/descriptor";
    queries = 0;
    int accepts = 0;
    assert(discovery.RefreshOwnerDirectory(commissioned, [&](const auto& route) {
        ++accepts;
        assert(route.owner_domain_descriptor_uri == commissioned.owner_domain_descriptor_uri);
        return ESP_OK;
    }) == ESP_OK);
    assert(accepts == 1 && queries == 0); // mDNS cannot block a working route.

    // Relocation can recover a failed saved route, while ignoring another Owner.
    supplied = &other; other.next = &wanted; wanted.next = nullptr;
    std::vector<std::string> attempted;
    assert(discovery.RefreshOwnerDirectory(commissioned, [&](const auto& route) {
        assert(route.owner_domain_id == "owner-wanted");
        attempted.push_back(route.owner_domain_descriptor_uri);
        return attempted.size() == 1 ? ESP_FAIL : ESP_OK;
    }) == ESP_OK);
    assert((attempted == std::vector<std::string>{"https://saved.local/descriptor", "https://moved.local/descriptor"}));

    // Discovery supplies a route, never authorization. Rejected verification
    // remains rejected rather than being converted into discovery success.
    accepts = 0;
    assert(discovery.RefreshOwnerDirectory(commissioned, [&](const auto&) {
        return ++accepts == 1 ? ESP_FAIL : ESP_ERR_INVALID_RESPONSE;
    }) == ESP_ERR_INVALID_RESPONSE);
    assert(accepts == 2);

    supplied = &other; other.next = nullptr; accepts = 0;
    assert(discovery.RefreshOwnerDirectory(commissioned, [&](const auto&) {
        ++accepts; return ESP_FAIL;
    }) == ESP_FAIL);
    assert(accepts == 1); // Never send a request to the other Owner.

    queries = 0;
    assert(discovery.RefreshOwnerDirectory(commissioned, [&](const auto&) {
        return ESP_ERR_INVALID_STATE;
    }) == ESP_ERR_INVALID_STATE);
    assert(queries == 0); // A fenced operation cannot start discovery.

    commissioned.owner_domain_id.clear(); queries = 0;
    assert(discovery.RefreshOwnerDirectory(commissioned, [&](const auto&) {
        assert(false); return ESP_OK;
    }) == ESP_ERR_INVALID_ARG);
    assert(queries == 0);
    std::cout << "Owner route and discovery regressions passed\n";
}
