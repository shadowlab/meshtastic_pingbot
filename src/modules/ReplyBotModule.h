#pragma once
#include "configuration.h"
#if !MESHTASTIC_EXCLUDE_REPLYBOT
#include "SinglePortModule.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include "modules/ReplyBotText.h"

// Base-station ping bot: answers "test" (and /ping, /hello, /test) on one channel (default "Test") with the
// sender's ID, hop count and this station's location. The paired phone sets both with "/replybot ..." commands.
// Enable with "USERPREFS_REPLYBOT_ENABLED": "true" in userPrefs.jsonc.
class ReplyBotModule : public SinglePortModule
{
  public:
    ReplyBotModule();
    bool wantPacket(const meshtastic_MeshPacket *p) override;
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

  protected:
    void handleCommand(const char *msg);
    const char *stationLocation() const;
    void notifyPhone(const char *format, ...);
    void sendReply(const meshtastic_MeshPacket &rx, const char *text);

    char channelName[ReplyBotText::MAX_CHANNEL_NAME_LEN + 1] = {};
    char location[ReplyBotText::MAX_LOCATION_LEN + 1] = {}; // empty: not set from the phone
};
#endif // MESHTASTIC_EXCLUDE_REPLYBOT
