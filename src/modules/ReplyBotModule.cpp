#include "configuration.h"
#if !MESHTASTIC_EXCLUDE_REPLYBOT

#include "Channels.h"
#include "FSCommon.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "ReplyBotModule.h"
#include "SPILock.h"
#include "SafeFile.h"
#include "Throttle.h"
#include "UptimeClock.h"
#include "gps/RTC.h"
#include "mesh/MeshTypes.h"
#include "modules/RoutingModule.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#ifndef USERPREFS_REPLYBOT_CHANNEL_NAME
#define USERPREFS_REPLYBOT_CHANNEL_NAME "Test"
#endif
static_assert(sizeof(USERPREFS_REPLYBOT_CHANNEL_NAME) - 1 <= ReplyBotText::MAX_CHANNEL_NAME_LEN,
              "USERPREFS_REPLYBOT_CHANNEL_NAME is longer than a channel name");

#ifdef USERPREFS_REPLYBOT_LOCATION
static_assert(sizeof(USERPREFS_REPLYBOT_LOCATION) - 1 <= ReplyBotText::MAX_LOCATION_LEN,
              "USERPREFS_REPLYBOT_LOCATION is longer than ReplyBotText::MAX_LOCATION_LEN");
#endif

static constexpr const char *REPLYBOT_CHANNEL_FILE = "/prefs/replybot.txt";
static constexpr const char *REPLYBOT_LOCATION_FILE = "/prefs/replybot_loc.txt";

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
            e.lastMs = Time::getMillis();
            return false;
        }
    }
    replybotCooldown[replybotCooldownIdx].from = from;
    replybotCooldown[replybotCooldownIdx].lastMs = Time::getMillis();
    replybotCooldownIdx = (replybotCooldownIdx + 1) % REPLYBOT_COOLDOWN_SLOTS;
    return false;
}

// Reads a saved setting into out; false (out untouched) when absent, empty, over maxLen or holding a NUL.
static bool replybotLoad(const char *path, char *out, size_t maxLen)
{
#ifdef FSCom
    char stored[ReplyBotText::MAX_LOCATION_LEN + 1] = {};
    const size_t cap = maxLen < sizeof(stored) - 1 ? maxLen : sizeof(stored) - 1;
    size_t n = 0;
    spiLock->lock();
    auto file = FSCom.open(path, FILE_O_READ);
    if (file) {
        n = file.read(reinterpret_cast<uint8_t *>(stored), cap + 1);
        file.close();
    }
    spiLock->unlock();
    if (n == 0 || n > cap || memchr(stored, '\0', n) != nullptr)
        return false;
    memcpy(out, stored, n);
    out[n] = '\0';
    return true;
#else
    return false;
#endif
}

static bool replybotSave(const char *path, const char *value)
{
#ifdef FSCom
    spiLock->lock();
    FSCom.mkdir("/prefs");
    spiLock->unlock();

    SafeFile f(path, true);
    spiLock->lock();
    f.write(reinterpret_cast<const uint8_t *>(value), strlen(value));
    spiLock->unlock();
    return f.close();
#else
    return false;
#endif
}

static void replybotForget(const char *path)
{
#ifdef FSCom
    spiLock->lock();
    if (FSCom.exists(path))
        FSCom.remove(path);
    spiLock->unlock();
#endif
}

// Promiscuous so channel broadcasts reach us, not just packets addressed to this node.
ReplyBotModule::ReplyBotModule() : SinglePortModule("replybot", meshtastic_PortNum_TEXT_MESSAGE_APP)
{
    isPromiscuous = true;
    // MeshModule::setup() is never invoked; modules are constructed after the filesystem is mounted.
    strncpy(channelName, USERPREFS_REPLYBOT_CHANNEL_NAME, sizeof(channelName) - 1);
    replybotLoad(REPLYBOT_CHANNEL_FILE, channelName, ReplyBotText::MAX_CHANNEL_NAME_LEN);
    replybotLoad(REPLYBOT_LOCATION_FILE, location, ReplyBotText::MAX_LOCATION_LEN);
    LOG_INFO("ReplyBot: serving channel '%s' as '%s'", channelName, stationLocation());
}

// The phone-set location wins, then the build-time one, then the node's long name.
const char *ReplyBotModule::stationLocation() const
{
    if (location[0])
        return location;
#ifdef USERPREFS_REPLYBOT_LOCATION
    return USERPREFS_REPLYBOT_LOCATION;
#else
    return owner.long_name;
#endif
}

void ReplyBotModule::notifyPhone(const char *format, ...)
{
    meshtastic_ClientNotification *cn = clientNotificationPool.allocZeroed();
    if (!cn)
        return;
    cn->level = meshtastic_LogRecord_Level_INFO;
    cn->time = getValidTime(RTCQualityFromNet);
    va_list args;
    va_start(args, format);
    vsnprintf(cn->message, sizeof(cn->message), format, args);
    va_end(args);
    LOG_INFO("ReplyBot: %s", cn->message);
    service->sendClientNotification(cn);
}

void ReplyBotModule::handleCommand(const char *msg)
{
    static const char *const notSaved = " (not saved, reverts on reboot)";
    char arg[ReplyBotText::MAX_LOCATION_LEN + 1] = {};
    switch (ReplyBotText::parseCommand(msg, arg, sizeof(arg))) {
    case ReplyBotText::Command::None:
        return;
    case ReplyBotText::Command::Status:
        notifyPhone("ReplyBot answers on channel '%s' as '%s'", channelName, stationLocation());
        return;
    case ReplyBotText::Command::SetChannel: {
        strncpy(channelName, arg, sizeof(channelName) - 1);
        const bool saved = replybotSave(REPLYBOT_CHANNEL_FILE, channelName);
        notifyPhone("ReplyBot now answers on channel '%s'%s", channelName, saved ? "" : notSaved);
        return;
    }
    case ReplyBotText::Command::SetLocation: {
        strncpy(location, arg, sizeof(location) - 1);
        const bool saved = replybotSave(REPLYBOT_LOCATION_FILE, location);
        notifyPhone("ReplyBot location set to '%s'%s", location, saved ? "" : notSaved);
        return;
    }
    case ReplyBotText::Command::ClearLocation:
        memset(location, 0, sizeof(location));
        replybotForget(REPLYBOT_LOCATION_FILE);
        notifyPhone("ReplyBot location cleared, using '%s'", stationLocation());
        return;
    case ReplyBotText::Command::Reset:
        memset(channelName, 0, sizeof(channelName));
        strncpy(channelName, USERPREFS_REPLYBOT_CHANNEL_NAME, sizeof(channelName) - 1);
        memset(location, 0, sizeof(location));
        replybotForget(REPLYBOT_CHANNEL_FILE);
        replybotForget(REPLYBOT_LOCATION_FILE);
        notifyPhone("ReplyBot reset: channel '%s' as '%s'", channelName, stationLocation());
        return;
    case ReplyBotText::Command::Invalid:
        notifyPhone("ReplyBot: /replybot channel <name> (max %u chars), /replybot location <text>|clear (max %u), "
                    "/replybot reset, /replybot status",
                    (unsigned)ReplyBotText::MAX_CHANNEL_NAME_LEN, (unsigned)ReplyBotText::MAX_LOCATION_LEN);
        return;
    }
}

bool ReplyBotModule::wantPacket(const meshtastic_MeshPacket *p)
{
    return p && p->which_payload_variant == meshtastic_MeshPacket_decoded_tag && p->decoded.portnum == ourPortNum;
}

ProcessMessage ReplyBotModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    if (mp.decoded.payload.size == 0)
        return ProcessMessage::CONTINUE;

    char buf[sizeof(mp.decoded.payload.bytes) + 1];
    const size_t n = mp.decoded.payload.size < sizeof(buf) - 1 ? mp.decoded.payload.size : sizeof(buf) - 1;
    memcpy(buf, mp.decoded.payload.bytes, n);
    buf[n] = '\0';

    // Commands are accepted only from a client connected to this node (phone app, serial, TCP API).
    if (currentRequestSource == RX_SRC_USER) {
        handleCommand(buf);
        return ProcessMessage::CONTINUE;
    }

    // Our own echoes must never trigger a reply to ourselves.
    if (isFromUs(&mp))
        return ProcessMessage::CONTINUE;

    // Only broadcasts on the configured channel are answered; DMs and other channels are left alone.
    if (!isBroadcast(mp.to) || mp.channel >= channels.getNumChannels() ||
        !ReplyBotText::channelMatches(channels.getName(mp.channel), channelName))
        return ProcessMessage::CONTINUE;

    if (!ReplyBotText::isTrigger(buf))
        return ProcessMessage::CONTINUE;

    if (replybotRateLimited(mp.from, REPLYBOT_COOLDOWN_MS)) {
        LOG_DEBUG("ReplyBot: cooldown active for 0x%08x", mp.from);
        return ProcessMessage::CONTINUE;
    }

    const int hopsAway = getHopsAway(mp);
    char reply[sizeof(meshtastic_User::long_name) + 64];
    ReplyBotText::formatReply(reply, sizeof(reply), mp.from, hopsAway, stationLocation());
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
