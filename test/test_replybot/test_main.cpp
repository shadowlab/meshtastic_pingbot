/**
 * Unit tests for the ReplyBot text helpers in src/modules/ReplyBotText.cpp:
 *  - ReplyBotText::isTrigger()      which chat messages wake the base-station ping bot
 *  - ReplyBotText::channelMatches() which channel the bot is allowed to listen and reply on
 *  - ReplyBotText::formatReply()    the exact reply text sent back to the mesh
 *  - ReplyBotText::parseCommand()   the "/replybot ..." commands the paired phone uses to retarget the bot
 *
 * Why: the bot sits on a busy shared channel. isTrigger() must fire on a bare "test" as people
 * actually type it (any case, trailing punctuation, phone-added whitespace) but must stay silent on
 * ordinary chat that merely contains the word, or the bot spams the channel. channelMatches() is the
 * only thing keeping the bot off every channel but the configured one. formatReply() is the
 * user-facing contract ("📡 Hi !a0387fc0 -- We see you 7 Hops to Hayward.") and must stay
 * singular/plural-correct and degrade cleanly when the hop count or station name is unknown.
 * parseCommand() decides what gets persisted as the served channel, so it must never yield an empty
 * name or one longer than the 11 bytes meshtastic_ChannelSettings::name can hold.
 *
 * Regressions guarded: replying to "testing 123" or "this is a test"; ignoring "Test" or "test!";
 * matching a channel by prefix ("Testing") or with an empty configured name; a reply that drops the
 * !xxxxxxxx node ID, says "1 Hops", prints a negative hop count, or overruns its buffer; a channel
 * command that stores an empty, untrimmed or over-long name, or that treats "/replybotx" or ordinary
 * chat as a command.
 *
 * Under [env:coverage-replybot] (USERPREFS_REPLYBOT_ENABLED=1) the suite also drives ReplyBotModule
 * itself through handleReceived(), with a mock router capturing what it transmits. That pins the
 * behaviour the helpers cannot: the bot answers only broadcasts on the served channel and replies
 * into that same channel; it never answers DMs, other channels, its own traffic, or a sender still in
 * cooldown; "/replybot" commands are honoured only from a locally attached client (RX_SRC_USER) and
 * never from the mesh, are acknowledged with a ClientNotification, and the chosen channel survives a
 * module restart via /prefs/replybot.txt. Regressions guarded: a mesh user retargeting the bot, the
 * bot answering on a channel it was moved off, or a retarget that reverts on reboot.
 */

#include "MeshTypes.h" // BEFORE TestUtil.h
#include "TestUtil.h"
#include "mesh/NodeDB.h"
#include "modules/ReplyBotText.h"
#include <cstring>
#include <unity.h>

#if !MESHTASTIC_EXCLUDE_REPLYBOT
#include "Channels.h"
#include "FSCommon.h"
#include "SPILock.h"
#include "UptimeClock.h"
#include "airtime.h"
#include "mesh/RadioInterface.h"
#include "mesh/Router.h"
#include "modules/ReplyBotModule.h"
#include "support/MockMeshService.h"
#include <memory>
#include <string>
#include <vector>

class ReplyBotModuleTestShim : public ReplyBotModule
{
  public:
    using ReplyBotModule::channelName;
    static void setSource(RxSource src) { currentRequestSource = src; }
};

namespace
{

// sendLocal() refuses to send with no interface attached, so the mock router carries a stub one.
class StubRadioInterface : public RadioInterface
{
  public:
    ErrorCode send(meshtastic_MeshPacket *p) override
    {
        packetPool.release(p);
        return ERRNO_OK;
    }
    uint32_t getPacketTime(uint32_t, bool) override { return 100; }
};

class MockRouter : public Router
{
  public:
    MockRouter() { addInterface(std::unique_ptr<RadioInterface>(new StubRadioInterface())); }

    ErrorCode send(meshtastic_MeshPacket *p) override
    {
        sentPackets.push_back(*p);
        packetPool.release(p);
        return ERRNO_OK;
    }

    // The broadcast loopback copy lands here; nothing drains fromRadioQueue in tests.
    void enqueueReceivedMessage(meshtastic_MeshPacket *p) override { packetPool.release(p); }

    std::vector<meshtastic_MeshPacket> sentPackets;
};

constexpr ChannelIndex kPrimaryCh = 0;
constexpr ChannelIndex kTestCh = 1;
constexpr ChannelIndex kRescueCh = 2;
constexpr uint32_t kClockBaseMs = 60 * 60 * 1000;

ReplyBotModuleTestShim *mod = nullptr;
MockMeshService *mockSvc = nullptr;
MockRouter *mockRouter = nullptr;
AirTime *testAirTime = nullptr;

void installChannel(ChannelIndex index, meshtastic_Channel_Role role, const char *name)
{
    if (channelFile.channels_count < (pb_size_t)(index + 1))
        channelFile.channels_count = index + 1;
    meshtastic_Channel &ch = channelFile.channels[index];
    ch = meshtastic_Channel_init_zero;
    ch.index = index;
    ch.has_settings = true;
    ch.role = role;
    strncpy(ch.settings.name, name, sizeof(ch.settings.name) - 1);
    ch.settings.psk.size = 1;
    ch.settings.psk.bytes[0] = 1;
}

void removeSavedChannel()
{
    spiLock->lock();
    FSCom.remove("/prefs/replybot.txt");
    spiLock->unlock();
}

// A text packet as handleReceived() sees it after decode: channel is the table index.
meshtastic_MeshPacket makeText(NodeNum from, NodeNum to, ChannelIndex ch, const char *text, uint8_t hopStart = 7,
                               uint8_t hopLimit = 0)
{
    meshtastic_MeshPacket p = meshtastic_MeshPacket_init_zero;
    p.from = from;
    p.to = to;
    p.channel = ch;
    p.id = 0x1000 + from;
    p.hop_start = hopStart;
    p.hop_limit = hopLimit;
    p.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p.decoded.has_bitfield = true;
    p.decoded.payload.size = strlen(text);
    memcpy(p.decoded.payload.bytes, text, p.decoded.payload.size);
    return p;
}

void deliver(const meshtastic_MeshPacket &p, RxSource src = RX_SRC_RADIO)
{
    ReplyBotModuleTestShim::setSource(src);
    mod->handleReceived(p);
}

std::string sentText(size_t i)
{
    const auto &d = mockRouter->sentPackets[i].decoded;
    return std::string(reinterpret_cast<const char *>(d.payload.bytes), d.payload.size);
}

} // namespace

void setUp(void)
{
    Time::resetMonotonicForTests();
    Time::setTestMillis(kClockBaseMs);
    Time::serviceMonotonic();

    testAirTime = new AirTime();
    airTime = testAirTime;
    mockSvc = new MockMeshService();
    service = mockSvc;
    mockRouter = new MockRouter();
    router = mockRouter;

    channelFile = meshtastic_ChannelFile_init_zero;
    installChannel(kPrimaryCh, meshtastic_Channel_Role_PRIMARY, "Home");
    installChannel(kTestCh, meshtastic_Channel_Role_SECONDARY, "Test");
    installChannel(kRescueCh, meshtastic_Channel_Role_SECONDARY, "Rescue");
    channels.onConfigChanged();

    strncpy(owner.long_name, "Hayward", sizeof(owner.long_name) - 1);
    removeSavedChannel();
    mod = new ReplyBotModuleTestShim();
}

void tearDown(void)
{
    delete mod;
    mod = nullptr;
    removeSavedChannel();

    // sendToMesh() copies a queue status to the phone on every send; release them for LSan.
    if (mockSvc) {
        meshtastic_MeshPacket *p;
        while ((p = mockSvc->getForPhone()) != nullptr)
            mockSvc->releaseToPool(p);
    }
    service = nullptr;
    delete mockSvc;
    mockSvc = nullptr;
    router = nullptr;
    delete mockRouter;
    mockRouter = nullptr;
    airTime = nullptr;
    delete testAirTime;
    testAirTime = nullptr;
    Time::useRealClock();
}

static void test_module_repliesIntoTheTestChannel()
{
    deliver(makeText(0xa0387fc0, NODENUM_BROADCAST, kTestCh, "Test"));
    TEST_ASSERT_EQUAL(1, mockRouter->sentPackets.size());
    const meshtastic_MeshPacket &reply = mockRouter->sentPackets[0];
    TEST_ASSERT_EQUAL_UINT32(NODENUM_BROADCAST, reply.to);
    TEST_ASSERT_EQUAL_UINT32(kTestCh, reply.channel);
    TEST_ASSERT_EQUAL_STRING("📡 Hi !a0387fc0 -- We see you 7 Hops to Hayward.", sentText(0).c_str());
}

static void test_module_ignoresOtherChannelsAndDMs()
{
    deliver(makeText(0x0a000001, NODENUM_BROADCAST, kPrimaryCh, "test"));
    deliver(makeText(0x0a000002, nodeDB->getNodeNum(), kTestCh, "test"));
    deliver(makeText(0x0a000003, NODENUM_BROADCAST, kTestCh, "testing 123"));
    TEST_ASSERT_EQUAL(0, mockRouter->sentPackets.size());
}

static void test_module_ignoresItsOwnTraffic()
{
    deliver(makeText(nodeDB->getNodeNum(), NODENUM_BROADCAST, kTestCh, "test"));
    TEST_ASSERT_EQUAL(0, mockRouter->sentPackets.size());
}

static void test_module_cooldownIsPerSender()
{
    deliver(makeText(0x0b000001, NODENUM_BROADCAST, kTestCh, "test"));
    Time::advanceTestMillis(30 * 1000);
    deliver(makeText(0x0b000001, NODENUM_BROADCAST, kTestCh, "test"));
    TEST_ASSERT_EQUAL_MESSAGE(1, mockRouter->sentPackets.size(), "a repeat inside 60 s must be dropped");

    deliver(makeText(0x0b000002, NODENUM_BROADCAST, kTestCh, "test"));
    TEST_ASSERT_EQUAL_MESSAGE(2, mockRouter->sentPackets.size(), "another sender is not throttled");

    Time::advanceTestMillis(31 * 1000);
    deliver(makeText(0x0b000001, NODENUM_BROADCAST, kTestCh, "test"));
    TEST_ASSERT_EQUAL_MESSAGE(3, mockRouter->sentPackets.size(), "the first sender is answered again after 60 s");
}

static void test_module_phoneCommandRetargetsAndPersists()
{
    deliver(makeText(nodeDB->getNodeNum(), NODENUM_BROADCAST, kPrimaryCh, "/replybot channel Rescue"), RX_SRC_USER);
    TEST_ASSERT_EQUAL_STRING("Rescue", mod->channelName);
    TEST_ASSERT_EQUAL_UINT32(1, mockSvc->notificationCount);
    TEST_ASSERT_EQUAL_MESSAGE(0, mockRouter->sentPackets.size(), "a command is not itself answered on the mesh");

    deliver(makeText(0x0c000001, NODENUM_BROADCAST, kTestCh, "test"));
    TEST_ASSERT_EQUAL_MESSAGE(0, mockRouter->sentPackets.size(), "the old channel must go quiet");
    deliver(makeText(0x0c000002, NODENUM_BROADCAST, kRescueCh, "test"));
    TEST_ASSERT_EQUAL(1, mockRouter->sentPackets.size());
    TEST_ASSERT_EQUAL_UINT32(kRescueCh, mockRouter->sentPackets[0].channel);

    // A fresh instance is what a reboot builds; it must come back on the saved channel.
    delete mod;
    mod = new ReplyBotModuleTestShim();
    TEST_ASSERT_EQUAL_STRING("Rescue", mod->channelName);

    deliver(makeText(nodeDB->getNodeNum(), NODENUM_BROADCAST, kPrimaryCh, "/replybot reset"), RX_SRC_USER);
    TEST_ASSERT_EQUAL_STRING("Test", mod->channelName);
}

static void test_module_meshCannotRunCommands()
{
    deliver(makeText(0x0d000001, NODENUM_BROADCAST, kTestCh, "/replybot channel Rescue"));
    deliver(makeText(0x0d000001, nodeDB->getNodeNum(), kTestCh, "/replybot channel Rescue"));
    TEST_ASSERT_EQUAL_STRING("Test", mod->channelName);
    TEST_ASSERT_EQUAL_UINT32(0, mockSvc->notificationCount);
}

static void test_module_invalidCommandIsReportedNotApplied()
{
    deliver(makeText(nodeDB->getNodeNum(), NODENUM_BROADCAST, kPrimaryCh, "/replybot channel WayTooLongName"), RX_SRC_USER);
    TEST_ASSERT_EQUAL_STRING("Test", mod->channelName);
    TEST_ASSERT_EQUAL_UINT32(1, mockSvc->notificationCount);
}
#else
void setUp(void) {}
void tearDown(void) {}
#endif // !MESHTASTIC_EXCLUDE_REPLYBOT

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

static void test_parseCommand_setChannelTrimsName()
{
    char name[16];
    TEST_ASSERT_TRUE(ReplyBotText::parseCommand("/replybot channel Rescue", name, sizeof(name)) ==
                     ReplyBotText::Command::SetChannel);
    TEST_ASSERT_EQUAL_STRING("Rescue", name);
    TEST_ASSERT_TRUE(ReplyBotText::parseCommand("  /ReplyBot   CHANNEL   Ham Net  \n", name, sizeof(name)) ==
                     ReplyBotText::Command::SetChannel);
    TEST_ASSERT_EQUAL_STRING("Ham Net", name);
}

static void test_parseCommand_channelNameLengthBounds()
{
    char name[16];
    // 11 bytes is the most meshtastic_ChannelSettings::name holds; 12 must be refused, not truncated.
    TEST_ASSERT_TRUE(ReplyBotText::parseCommand("/replybot channel ABCDEFGHIJK", name, sizeof(name)) ==
                     ReplyBotText::Command::SetChannel);
    TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJK", name);
    TEST_ASSERT_TRUE(ReplyBotText::parseCommand("/replybot channel ABCDEFGHIJKL", name, sizeof(name)) ==
                     ReplyBotText::Command::Invalid);
    TEST_ASSERT_TRUE(ReplyBotText::parseCommand("/replybot channel   ", name, sizeof(name)) == ReplyBotText::Command::Invalid);
    char tiny[4];
    TEST_ASSERT_TRUE(ReplyBotText::parseCommand("/replybot channel Test", tiny, sizeof(tiny)) == ReplyBotText::Command::Invalid);
}

static void test_parseCommand_statusResetAndInvalid()
{
    char name[16];
    TEST_ASSERT_TRUE(ReplyBotText::parseCommand("/replybot", name, sizeof(name)) == ReplyBotText::Command::Status);
    TEST_ASSERT_TRUE(ReplyBotText::parseCommand("/replybot status", name, sizeof(name)) == ReplyBotText::Command::Status);
    TEST_ASSERT_TRUE(ReplyBotText::parseCommand("/replybot reset ", name, sizeof(name)) == ReplyBotText::Command::ResetChannel);
    TEST_ASSERT_TRUE(ReplyBotText::parseCommand("/replybot reset now", name, sizeof(name)) == ReplyBotText::Command::Invalid);
    TEST_ASSERT_TRUE(ReplyBotText::parseCommand("/replybot frobnicate", name, sizeof(name)) == ReplyBotText::Command::Invalid);
}

static void test_parseCommand_nonCommandsIgnored()
{
    char name[16];
    TEST_ASSERT_TRUE(ReplyBotText::parseCommand("test", name, sizeof(name)) == ReplyBotText::Command::None);
    TEST_ASSERT_TRUE(ReplyBotText::parseCommand("/replybotx channel A", name, sizeof(name)) == ReplyBotText::Command::None);
    TEST_ASSERT_TRUE(ReplyBotText::parseCommand(nullptr, name, sizeof(name)) == ReplyBotText::Command::None);
}

extern "C" {
void setup()
{
    initializeTestEnvironment();
    // Built in both envs so the suite's declared writes do not depend on which one ran it.
    nodeDB = new NodeDB();
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
    RUN_TEST(test_parseCommand_setChannelTrimsName);
    RUN_TEST(test_parseCommand_channelNameLengthBounds);
    RUN_TEST(test_parseCommand_statusResetAndInvalid);
    RUN_TEST(test_parseCommand_nonCommandsIgnored);
#if !MESHTASTIC_EXCLUDE_REPLYBOT
    RUN_TEST(test_module_repliesIntoTheTestChannel);
    RUN_TEST(test_module_ignoresOtherChannelsAndDMs);
    RUN_TEST(test_module_ignoresItsOwnTraffic);
    RUN_TEST(test_module_cooldownIsPerSender);
    RUN_TEST(test_module_phoneCommandRetargetsAndPersists);
    RUN_TEST(test_module_meshCannotRunCommands);
    RUN_TEST(test_module_invalidCommandIsReportedNotApplied);
#endif
    exit(UNITY_END());
}

void loop() {}
}
