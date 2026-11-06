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

// Longest base-station location the phone may set; keeps the reply well inside one packet.
static constexpr size_t MAX_LOCATION_LEN = 32;

enum class Command : uint8_t { None, Status, SetChannel, SetLocation, ClearLocation, Reset, Invalid };

// Parses "/replybot [status]", "/replybot reset", "/replybot channel <name>", "/replybot location <text>" and
// "/replybot location clear". For SetChannel/SetLocation the trimmed argument is copied into argOut; an empty or
// over-long argument is Invalid rather than truncated. Other "/replybot ..." text is Invalid; anything else is None.
Command parseCommand(const char *msg, char *argOut, size_t argOutLen);

// True when channelName equals wanted, ignoring case. A null or empty name never matches.
bool channelMatches(const char *channelName, const char *wanted);

// Writes e.g. "📡 Hi !a0387fc0 -- We see you 7 Hops in Hayward." into out; hopsAway < 0 means unknown.
// stationName is the base station's location and may be null or empty. Returns the length written (excluding the terminator).
size_t formatReply(char *out, size_t outLen, uint32_t fromNode, int hopsAway, const char *stationName);

} // namespace ReplyBotText
