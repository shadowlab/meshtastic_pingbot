#pragma once
#include <cstddef>
#include <cstdint>

// Pure text helpers for ReplyBotModule, kept outside its exclude guard so native tests can reach them.
namespace ReplyBotText
{

// True for a bare "test" (any case, optional trailing punctuation) or a leading /ping, /hello or /test.
bool isTrigger(const char *msg);

// True when channelName equals wanted, ignoring case. A null or empty name never matches.
bool channelMatches(const char *channelName, const char *wanted);

// Writes e.g. "📡 Hi !a0387fc0 -- We see you 7 Hops to Hayward." into out; hopsAway < 0 means unknown.
// stationName may be null or empty. Returns the length written (excluding the terminator).
size_t formatReply(char *out, size_t outLen, uint32_t fromNode, int hopsAway, const char *stationName);

} // namespace ReplyBotText
