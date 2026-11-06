#pragma once
#include "configuration.h"
#if !MESHTASTIC_EXCLUDE_REPLYBOT
#include "SinglePortModule.h"
#include "mesh/generated/meshtastic/mesh.pb.h"

// Base-station ping bot: answers "test" (and /ping, /hello, /test) on the "Test" channel with the
// sender's ID and hop count. Enable with "USERPREFS_REPLYBOT_ENABLED": "true" in userPrefs.jsonc.
class ReplyBotModule : public SinglePortModule
{
  public:
    ReplyBotModule();
    bool wantPacket(const meshtastic_MeshPacket *p) override;
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

  protected:
    void sendReply(const meshtastic_MeshPacket &rx, const char *text);
};
#endif // MESHTASTIC_EXCLUDE_REPLYBOT
