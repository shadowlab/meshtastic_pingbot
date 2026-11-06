/**
 * Unit tests for the ReplyBot text helpers in src/modules/ReplyBotText.cpp:
 *  - ReplyBotText::isTrigger()      which chat messages wake the base-station ping bot
 *  - ReplyBotText::channelMatches() which channel the bot is allowed to listen and reply on
 *  - ReplyBotText::formatReply()    the exact reply text sent back to the mesh
 *
 * Why: the bot sits on a busy shared channel. isTrigger() must fire on a bare "test" as people
 * actually type it (any case, trailing punctuation, phone-added whitespace) but must stay silent on
 * ordinary chat that merely contains the word, or the bot spams the channel. channelMatches() is the
 * only thing keeping the bot off every channel but the configured one. formatReply() is the
 * user-facing contract ("📡 Hi !a0387fc0 -- We see you 7 Hops to Hayward.") and must stay
 * singular/plural-correct and degrade cleanly when the hop count or station name is unknown.
 *
 * Regressions guarded: replying to "testing 123" or "this is a test"; ignoring "Test" or "test!";
 * matching a channel by prefix ("Testing") or with an empty configured name; a reply that drops the
 * !xxxxxxxx node ID, says "1 Hops", prints a negative hop count, or overruns its buffer.
 */

#include "TestUtil.h"
#include "modules/ReplyBotText.h"
#include <cstring>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_isTrigger_bareTestAnyCase()
{
    TEST_ASSERT_TRUE(ReplyBotText::isTrigger("test"));
    TEST_ASSERT_TRUE(ReplyBotText::isTrigger("Test"));
    TEST_ASSERT_TRUE(ReplyBotText::isTrigger("TEST"));
}

static void test_isTrigger_bareTestWithPunctuationAndWhitespace()
{
    TEST_ASSERT_TRUE(ReplyBotText::isTrigger("  test  "));
    TEST_ASSERT_TRUE(ReplyBotText::isTrigger("test!"));
    TEST_ASSERT_TRUE(ReplyBotText::isTrigger("Test?"));
    TEST_ASSERT_TRUE(ReplyBotText::isTrigger("test...\n"));
}

static void test_isTrigger_chatContainingTestIgnored()
{
    TEST_ASSERT_FALSE(ReplyBotText::isTrigger("testing"));
    TEST_ASSERT_FALSE(ReplyBotText::isTrigger("test 123"));
    TEST_ASSERT_FALSE(ReplyBotText::isTrigger("this is a test"));
    TEST_ASSERT_FALSE(ReplyBotText::isTrigger("tes"));
}

static void test_isTrigger_slashCommands()
{
    TEST_ASSERT_TRUE(ReplyBotText::isTrigger("/ping"));
    TEST_ASSERT_TRUE(ReplyBotText::isTrigger("/hello there"));
    TEST_ASSERT_TRUE(ReplyBotText::isTrigger("/TEST"));
    TEST_ASSERT_FALSE(ReplyBotText::isTrigger("/pingpong"));
}

static void test_isTrigger_nullAndEmpty()
{
    TEST_ASSERT_FALSE(ReplyBotText::isTrigger(nullptr));
    TEST_ASSERT_FALSE(ReplyBotText::isTrigger(""));
    TEST_ASSERT_FALSE(ReplyBotText::isTrigger("   "));
}

static void test_channelMatches_exactIgnoringCase()
{
    TEST_ASSERT_TRUE(ReplyBotText::channelMatches("Test", "Test"));
    TEST_ASSERT_TRUE(ReplyBotText::channelMatches("test", "Test"));
}

static void test_channelMatches_rejectsOtherNames()
{
    TEST_ASSERT_FALSE(ReplyBotText::channelMatches("LongFast", "Test"));
    TEST_ASSERT_FALSE(ReplyBotText::channelMatches("Testing", "Test"));
    TEST_ASSERT_FALSE(ReplyBotText::channelMatches("Tes", "Test"));
}

static void test_channelMatches_emptyOrNullNeverMatches()
{
    TEST_ASSERT_FALSE(ReplyBotText::channelMatches("", ""));
    TEST_ASSERT_FALSE(ReplyBotText::channelMatches("Test", ""));
    TEST_ASSERT_FALSE(ReplyBotText::channelMatches(nullptr, "Test"));
    TEST_ASSERT_FALSE(ReplyBotText::channelMatches("Test", nullptr));
}

static void test_formatReply_matchesRequestedFormat()
{
    char out[128];
    size_t n = ReplyBotText::formatReply(out, sizeof(out), 0xa0387fc0, 7, "Hayward");
    TEST_ASSERT_EQUAL_STRING("📡 Hi !a0387fc0 -- We see you 7 Hops to Hayward.", out);
    TEST_ASSERT_EQUAL(strlen(out), n);
}

static void test_formatReply_singularHopAndZeroPadding()
{
    char out[128];
    ReplyBotText::formatReply(out, sizeof(out), 0x0000abcd, 1, "Hayward");
    TEST_ASSERT_EQUAL_STRING("📡 Hi !0000abcd -- We see you 1 Hop to Hayward.", out);
    ReplyBotText::formatReply(out, sizeof(out), 0x0000abcd, 0, "Hayward");
    TEST_ASSERT_EQUAL_STRING("📡 Hi !0000abcd -- We see you 0 Hops to Hayward.", out);
}

static void test_formatReply_unknownHops()
{
    char out[128];
    ReplyBotText::formatReply(out, sizeof(out), 0xa0387fc0, -1, "Hayward");
    TEST_ASSERT_EQUAL_STRING("📡 Hi !a0387fc0 -- We see you at Hayward (hop count unknown).", out);
}

static void test_formatReply_noStationName()
{
    char out[128];
    ReplyBotText::formatReply(out, sizeof(out), 0xa0387fc0, 3, "");
    TEST_ASSERT_EQUAL_STRING("📡 Hi !a0387fc0 -- We see you 3 Hops away.", out);
    ReplyBotText::formatReply(out, sizeof(out), 0xa0387fc0, -1, nullptr);
    TEST_ASSERT_EQUAL_STRING("📡 Hi !a0387fc0 -- We see you (hop count unknown).", out);
}

static void test_formatReply_truncatesWithinBuffer()
{
    char out[12];
    memset(out, 'x', sizeof(out));
    size_t n = ReplyBotText::formatReply(out, sizeof(out), 0xa0387fc0, 7, "Hayward");
    TEST_ASSERT_EQUAL(sizeof(out) - 1, n);
    TEST_ASSERT_EQUAL('\0', out[sizeof(out) - 1]);
    TEST_ASSERT_EQUAL(0, ReplyBotText::formatReply(nullptr, 0, 1, 1, "x"));
}

extern "C" {
void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_isTrigger_bareTestAnyCase);
    RUN_TEST(test_isTrigger_bareTestWithPunctuationAndWhitespace);
    RUN_TEST(test_isTrigger_chatContainingTestIgnored);
    RUN_TEST(test_isTrigger_slashCommands);
    RUN_TEST(test_isTrigger_nullAndEmpty);
    RUN_TEST(test_channelMatches_exactIgnoringCase);
    RUN_TEST(test_channelMatches_rejectsOtherNames);
    RUN_TEST(test_channelMatches_emptyOrNullNeverMatches);
    RUN_TEST(test_formatReply_matchesRequestedFormat);
    RUN_TEST(test_formatReply_singularHopAndZeroPadding);
    RUN_TEST(test_formatReply_unknownHops);
    RUN_TEST(test_formatReply_noStationName);
    RUN_TEST(test_formatReply_truncatesWithinBuffer);
    exit(UNITY_END());
}

void loop() {}
}
