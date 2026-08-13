#include <cassert>
#include <string>
#include <vector>

#include "eidolon/device_commissioning_protocol.h"

namespace {

using eidolon::Commission;
using eidolon::CommissioningEffects;
using eidolon::CommissioningReply;

const char* kCertificateInJson =
    "-----BEGIN CERTIFICATE-----\\nMIIBdummy\\n-----END CERTIFICATE-----\\n";
const char* kCertificate =
    "-----BEGIN CERTIFICATE-----\nMIIBdummy\n-----END CERTIFICATE-----\n";

std::string Payload(const std::string& fields)
{
    return std::string("{\"schema_version\":1,\"hub_id\":\"eidolon-hub-abc\",") +
           "\"hub_certificate\":\"" + kCertificateInJson + "\"" +
           (fields.empty() ? "" : "," + fields) + "}";
}

bool Contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// One commissioning act, with every side effect written down in the order it was
// asked for. Nothing here talks to a network or to flash: the claim being tested
// is not what any single step does but when each one happens relative to the
// others.
struct Commissioner {
    std::vector<std::string> steps;
    bool trust_accepted = true;
    std::string trusted_hub;
    std::string trusted_certificate;
    CommissioningReply reply;
    std::string joined_ssid;
    std::string joined_password;

    void Run(const std::string& body, const std::string& device_id = "aa:bb:cc:dd:ee:ff")
    {
        CommissioningEffects effects;
        effects.save_trust = [this](const std::string& hub_id, const std::string& certificate) {
            steps.push_back("trust");
            trusted_hub = hub_id;
            trusted_certificate = certificate;
            return trust_accepted;
        };
        effects.reply = [this](const CommissioningReply& sent) {
            steps.push_back("reply");
            reply = sent;
        };
        effects.join_network = [this](const std::string& ssid, const std::string& password) {
            steps.push_back("join");
            joined_ssid = ssid;
            joined_password = password;
        };
        Commission(body, device_id, effects);
    }
};

const std::vector<std::string> kTrustReplyJoin = {"trust", "reply", "join"};
const std::vector<std::string> kTrustReply = {"trust", "reply"};
const std::vector<std::string> kReplyOnly = {"reply"};

void AnswersTheCommissionerBeforeChangingNetworks()
{
    // The whole point. Commissioning arrives over the device's own access point,
    // and joining the commissioned network takes that access point down — so a
    // device that joins before it answers has done everything it was asked and
    // told nobody, and the setup that succeeded reads as one that failed.
    Commissioner commissioner;
    commissioner.Run(Payload("\"wifi\":{\"ssid\":\"home\",\"password\":\"secret\"}"));
    assert(commissioner.steps == kTrustReplyJoin);
    assert(commissioner.reply.status == "200 OK");
    assert(commissioner.joined_ssid == "home");
    assert(commissioner.joined_password == "secret");
}

void RemembersTheHostBeforeItGoesAnywhere()
{
    Commissioner commissioner;
    commissioner.Run(Payload("\"wifi\":{\"ssid\":\"home\"}"));
    assert(commissioner.steps.front() == "trust");
    assert(commissioner.trusted_hub == "eidolon-hub-abc");
    assert(commissioner.trusted_certificate == kCertificate);
}

void ADeliveryWithoutWifiLeavesTheNetworkAlone()
{
    // Only the Host changed hands. A device already on the right network must not
    // be knocked off it by being told whose it is.
    Commissioner commissioner;
    commissioner.Run(Payload(""));
    assert(commissioner.steps == kTrustReply);
    assert(commissioner.reply.status == "200 OK");
    assert(commissioner.joined_ssid.empty());
}

void SaysWhetherItIsAboutToChangeNetworks()
{
    // The answer leaves before the join, so it cannot report that the device is
    // on the network — only that it is about to try.
    Commissioner joining;
    joining.Run(Payload("\"wifi\":{\"ssid\":\"home\"}"));
    assert(Contains(joining.reply.body, "\"joining_network\":true"));

    Commissioner staying;
    staying.Run(Payload(""));
    assert(Contains(staying.reply.body, "\"joining_network\":false"));
}

void NamesTheDeviceTheHostWillSeeEnrolling()
{
    Commissioner commissioner;
    commissioner.Run(Payload(""), "34:85:18:aa:bb:cc");
    assert(Contains(commissioner.reply.body, "\"device_id\":\"34:85:18:aa:bb:cc\""));
    assert(Contains(commissioner.reply.body, "\"hub_id\":\"eidolon-hub-abc\""));
    assert(Contains(commissioner.reply.body, "\"schema_version\":1"));
}

void ARefusedPayloadTrustsNothingAndJoinsNothing()
{
    Commissioner commissioner;
    commissioner.Run("{\"schema_version\":1,\"hub_id\":\"eidolon-hub-abc\"}");
    assert(commissioner.steps == kReplyOnly);
    assert(commissioner.reply.status == "400 Bad Request");
    assert(Contains(commissioner.reply.body, "\"error\":"));
}

void ARejectedCertificateStopsBeforeTheNetwork()
{
    // Storage refused the certificate, so this device does not know whose it is.
    // It stays on the access point where someone can try again rather than
    // turning up on a home network belonging to nobody.
    Commissioner commissioner;
    commissioner.trust_accepted = false;
    commissioner.Run(Payload("\"wifi\":{\"ssid\":\"home\",\"password\":\"secret\"}"));
    assert(commissioner.steps == kTrustReply);
    assert(commissioner.reply.status == "400 Bad Request");
    assert(commissioner.joined_ssid.empty());
}

void AlwaysAnswers()
{
    // Every path answers exactly once: the commissioner is waiting on a socket
    // that the device is about to close either way.
    for (const std::string& body : {std::string(""), std::string("not json"),
                                    Payload(""), Payload("\"wifi\":{\"ssid\":\"home\"}"),
                                    Payload("\"wifi\":{\"ssid\":\"\"}")}) {
        Commissioner commissioner;
        commissioner.Run(body);
        size_t replies = 0;
        for (const std::string& step : commissioner.steps) {
            replies += (step == "reply") ? 1 : 0;
        }
        assert(replies == 1);
        assert(!commissioner.reply.body.empty());
    }
}

void QuotesTheHostNameBackSafely()
{
    // The Hub id is copied out of what the commissioner sent, so it goes back out
    // escaped. An answer that is not valid JSON is an answer the commissioner
    // cannot act on.
    Commissioner commissioner;
    commissioner.Run(std::string(R"({"schema_version":1,"hub_id":"hub-\"x\"","hub_certificate":")") +
                     kCertificateInJson + "\"}");
    assert(commissioner.trusted_hub == "hub-\"x\"");
    assert(Contains(commissioner.reply.body, R"("hub_id":"hub-\"x\"")"));
}

}  // namespace

int main()
{
    AnswersTheCommissionerBeforeChangingNetworks();
    RemembersTheHostBeforeItGoesAnywhere();
    ADeliveryWithoutWifiLeavesTheNetworkAlone();
    SaysWhetherItIsAboutToChangeNetworks();
    NamesTheDeviceTheHostWillSeeEnrolling();
    ARefusedPayloadTrustsNothingAndJoinsNothing();
    ARejectedCertificateStopsBeforeTheNetwork();
    AlwaysAnswers();
    QuotesTheHostNameBackSafely();
    return 0;
}
