#include "ReplyBotText.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace ReplyBotText
{

static bool isBlank(char c)
{
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

static bool startsWithWord(const char *msg, const char *word)
{
    const size_t n = strlen(word);
    return strncasecmp(msg, word, n) == 0 && (msg[n] == '\0' || isBlank(msg[n]));
}

bool isTrigger(const char *msg)
{
    if (!msg)
        return false;
    while (isBlank(*msg))
        msg++;

    if (startsWithWord(msg, "/ping") || startsWithWord(msg, "/hello") || startsWithWord(msg, "/test"))
        return true;

    // Bare "test" must be the whole message so ordinary chat containing the word stays quiet.
    if (strncasecmp(msg, "test", 4) != 0)
        return false;
    for (const char *p = msg + 4; *p; p++) {
        if (!isBlank(*p) && *p != '.' && *p != '!' && *p != '?')
            return false;
    }
    return true;
}

bool channelMatches(const char *channelName, const char *wanted)
{
    if (!channelName || !wanted || wanted[0] == '\0')
        return false;
    return strcasecmp(channelName, wanted) == 0;
}

size_t formatReply(char *out, size_t outLen, uint32_t fromNode, int hopsAway, const char *stationName)
{
    if (!out || outLen == 0)
        return 0;

    const bool haveName = stationName && stationName[0] != '\0';
    int n;
    if (hopsAway >= 0) {
        const char *unit = (hopsAway == 1) ? "Hop" : "Hops";
        if (haveName)
            n = snprintf(out, outLen, "📡 Hi !%08x -- We see you %d %s to %s.", fromNode, hopsAway, unit, stationName);
        else
            n = snprintf(out, outLen, "📡 Hi !%08x -- We see you %d %s away.", fromNode, hopsAway, unit);
    } else {
        if (haveName)
            n = snprintf(out, outLen, "📡 Hi !%08x -- We see you at %s (hop count unknown).", fromNode, stationName);
        else
            n = snprintf(out, outLen, "📡 Hi !%08x -- We see you (hop count unknown).", fromNode);
    }

    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return (static_cast<size_t>(n) < outLen) ? static_cast<size_t>(n) : outLen - 1;
}

} // namespace ReplyBotText
