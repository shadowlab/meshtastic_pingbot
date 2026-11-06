#pragma once
#include <cstddef>
#include <cstdint>

// Pure text helpers for ReplyBotModule, kept outside its exclude guard so native tests can reach them.
namespace ReplyBotText
{

// True for a bare "test" (any case, optional trailing punctuation) or a leading /ping, /hello or /test.
bool isTrigger(const char *msg);

// Longest channel name the firmware stores (meshtastic_ChannelSettings::name is char[12]).
static constexpr size_t MAX_CHANNEL_NAME_LEN = 11;

enum class Command : uint8_t { None, Status, SetChannel, ResetChannel, Invalid };

// Parses "/replybot", "/replybot status", "/replybot reset" and "/replybot channel <name>". For SetChannel the
// trimmed name is copied into channelOut. Anything else starting with /replybot is Invalid; other text is None.
Command parseCommand(const char *msg, char *channelOut, size_t channelOutLen);

// True when channelName equals wanted, ignoring case. A null or empty name never matches.
bool channelMatches(const char *channelName, const char *wanted);

// Writes e.g. "📡 Hi !a0387fc0 -- We see you 7 Hops to Hayward." into out; hopsAway < 0 means unknown.
// stationName may be null or empty. Returns the length written (excluding the terminator).
size_t formatReply(char *out, size_t outLen, uint32_t fromNode, int hopsAway, const char *stationName);

} // namespace ReplyBotText
