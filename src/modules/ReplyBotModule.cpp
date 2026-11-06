#include "configuration.h"
#if !MESHTASTIC_EXCLUDE_REPLYBOT

#include "Channels.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "ReplyBotModule.h"
#include "ReplyBotText.h"
#include "Throttle.h"
#include "mesh/MeshTypes.h"
#include "modules/RoutingModule.h"

#include <cstring>

#ifndef USERPREFS_REPLYBOT_CHANNEL_NAME
#define USERPREFS_REPLYBOT_CHANNEL_NAME "Test"
#endif

#ifdef USERPREFS_REPLYBOT_STATION_NAME
static_assert(sizeof(USERPREFS_REPLYBOT_STATION_NAME) <= sizeof(meshtastic_User::long_name),
              "USERPREFS_REPLYBOT_STATION_NAME is longer than a node long name");
#endif

struct ReplyBotCooldownEntry {
    uint32_t from = 0;
    uint32_t lastMs = 0;
};

static constexpr uint8_t REPLYBOT_COOLDOWN_SLOTS = 8;
static constexpr uint32_t REPLYBOT_COOLDOWN_MS = 60 * 1000;

static ReplyBotCooldownEntry replybotCooldown[REPLYBOT_COOLDOWN_SLOTS];
static uint8_t replybotCooldownIdx = 0;

// Returns true if this sender was answered within cooldownMs; otherwise records now and returns false.
static bool replybotRateLimited(uint32_t from, uint32_t cooldownMs)
{
    for (auto &e : replybotCooldown) {
        if (e.from == from) {
            if (Throttle::isWithinTimespanMs(e.lastMs, cooldownMs))
                return true;
            e.lastMs = millis();
            return false;
        }
    }
    replybotCooldown[replybotCooldownIdx].from = from;
    replybotCooldown[replybotCooldownIdx].lastMs = millis();
    replybotCooldownIdx = (replybotCooldownIdx + 1) % REPLYBOT_COOLDOWN_SLOTS;
    return false;
}

static const char *replybotStationName()
{
#ifdef USERPREFS_REPLYBOT_STATION_NAME
    return USERPREFS_REPLYBOT_STATION_NAME;
#else
    return owner.long_name;
#endif
}

// Promiscuous so channel broadcasts reach us, not just packets addressed to this node.
ReplyBotModule::ReplyBotModule() : SinglePortModule("replybot", meshtastic_PortNum_TEXT_MESSAGE_APP)
{
    isPromiscuous = true;
}

bool ReplyBotModule::wantPacket(const meshtastic_MeshPacket *p)
{
    return p && p->which_payload_variant == meshtastic_MeshPacket_decoded_tag && p->decoded.portnum == ourPortNum;
}

ProcessMessage ReplyBotModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    // Our own phone's messages (and our own echoes) must never trigger a reply to ourselves.
    if (isFromUs(&mp) || mp.decoded.payload.size == 0)
        return ProcessMessage::CONTINUE;

    // Only broadcasts on the configured channel are answered; DMs and other channels are left alone.
    if (!isBroadcast(mp.to) || mp.channel >= channels.getNumChannels() ||
        !ReplyBotText::channelMatches(channels.getName(mp.channel), USERPREFS_REPLYBOT_CHANNEL_NAME))
        return ProcessMessage::CONTINUE;

    char buf[sizeof(mp.decoded.payload.bytes) + 1];
    const size_t n = mp.decoded.payload.size < sizeof(buf) - 1 ? mp.decoded.payload.size : sizeof(buf) - 1;
    memcpy(buf, mp.decoded.payload.bytes, n);
    buf[n] = '\0';

    if (!ReplyBotText::isTrigger(buf))
        return ProcessMessage::CONTINUE;

    if (replybotRateLimited(mp.from, REPLYBOT_COOLDOWN_MS)) {
        LOG_DEBUG("ReplyBot: cooldown active for 0x%08x", mp.from);
        return ProcessMessage::CONTINUE;
    }

    const int hopsAway = getHopsAway(mp);
    char reply[sizeof(meshtastic_User::long_name) + 64];
    ReplyBotText::formatReply(reply, sizeof(reply), mp.from, hopsAway, replybotStationName());
    LOG_INFO("ReplyBot: answering 0x%08x (%d hops) on ch %u", mp.from, hopsAway, (unsigned)mp.channel);
    sendReply(mp, reply);
    return ProcessMessage::CONTINUE;
}

void ReplyBotModule::sendReply(const meshtastic_MeshPacket &rx, const char *text)
{
    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p)
        return;

    p->to = NODENUM_BROADCAST;
    p->channel = rx.channel;
    p->want_ack = false;
    // Size the reply's hop limit to the hops the request took so far-away senders still hear it.
    if (routingModule)
        p->hop_limit = routingModule->getHopLimitForResponse(rx);

    size_t len = strlen(text);
    if (len > sizeof(p->decoded.payload.bytes))
        len = sizeof(p->decoded.payload.bytes);
    p->decoded.payload.size = len;
    memcpy(p->decoded.payload.bytes, text, len);
    service->sendToMesh(p);
}
#endif // MESHTASTIC_EXCLUDE_REPLYBOT
