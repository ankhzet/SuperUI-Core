/*
 * AiBotAIBridge.cpp — the C# <-> C++ TCP bridge for the autonomous AI bot.
 *
 * Split from the monolithic AiBotAI.cpp. THIS TU holds the bridge domain:
 *   - the non-blocking TCP transport (connect / disconnect / send / flush / recv)
 *   - HELLO / STATE / EVENT senders
 *   - the file-local minimal JSON extractors (JsonExtractFloat/Int/String)
 *   - BridgeProcessLine dispatch + every BridgeHandle* command handler
 *   - the C++→C# event senders (SendKillEvent / SendQuestUpdateEvent / SendLevelUpEvent /
 *     SendChatRecvEvent)
 *
 * The JsonExtract* statics are defined before BridgeProcessLine and every handler that
 * uses them (statics are file-local). All AiBotAI methods link across the sibling TUs;
 * cross-TU members called from here (MoveToDestination / ReGroundZ / ClearStoredPath /
 * StopMoving from Movement, ChooseQuestReward / TryAutoEquip(Bags) from Loot) are defined
 * in those siblings.
 */

#include "AiBotAIMain.h"
#include "SuiPossess.h"      // [SUI] free-view command waiver on the possessed drop
#include "Player.h"
#include <cstring>
#include <cstdio>
#include <cctype>   // tolower — [FOLLOW-CMD] lowercases the stored escort-override name
#include "Group.h"
#include "CreatureAI.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "PlayerBotMgr.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "World.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "Chat.h"
#include "BattleGround.h"
#include "TargetedMovementGenerator.h"
#include "QuestDef.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Server/Packets/Channel.h"
#include "ChannelMgr.h"
#include "Bag.h"
#include "PathFinder.h"
#include "MoveMap.h"

// ============================================================
// TCP BRIDGE — Phase 2
// ============================================================

void AiBotAI::BridgeConnect()
{
    if (m_bridgeConnected)
        return;
 
    m_bridgeSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_bridgeSocket == BRIDGE_INVALID_SOCKET)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: socket() failed", me->GetName());
        return;
    }
 
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(BRIDGE_PORT);
    inet_pton(AF_INET, BRIDGE_HOST, &addr.sin_addr);
 
    if (connect(m_bridgeSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: connect failed (will retry in %ums)",
            me->GetName(), m_bridgeReconnectDelay);
        BRIDGE_CLOSE_SOCKET(m_bridgeSocket);
        m_bridgeSocket = BRIDGE_INVALID_SOCKET;
        m_bridgeReconnectTimer = m_bridgeReconnectDelay;
        // Exponential backoff
        m_bridgeReconnectDelay = std::min(m_bridgeReconnectDelay * 2, (uint32)BRIDGE_RECONNECT_MAX);
        return;
    }
 
    // Set non-blocking
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(m_bridgeSocket, FIONBIO, &mode);
#else
    int flags = fcntl(m_bridgeSocket, F_GETFL, 0);
    fcntl(m_bridgeSocket, F_SETFL, flags | O_NONBLOCK);
#endif
 
    m_bridgeConnected = true;
    m_bridgeHelloSent = false;
    m_bridgeReconnectDelay = BRIDGE_RECONNECT_BASE;
    m_bridgeRecvLen = 0;
    m_bridgeSendBuf.clear();   // Session 36: never carry a stale queue across connections
 
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: connected to %s:%d",
        me->GetName(), BRIDGE_HOST, BRIDGE_PORT);
}


void AiBotAI::BridgeDisconnect()
{
    if (m_bridgeSocket != BRIDGE_INVALID_SOCKET)
    {
        BRIDGE_CLOSE_SOCKET(m_bridgeSocket);
        m_bridgeSocket = BRIDGE_INVALID_SOCKET;
    }
    m_bridgeConnected = false;
    m_bridgeHelloSent = false;
    m_bridgeRecvLen = 0;
    m_bridgeSendBuf.clear();   // Session 36: drop queued bytes; they belong to the dead socket
}

void AiBotAI::BridgeSend(const char* json)
{
    if (!m_bridgeConnected || m_bridgeSocket == BRIDGE_INVALID_SOCKET)
        return;
 
    size_t len = strlen(json);
 
    // Safety valve: if C# stops reading, the kernel send buffer fills and our
    // queue grows. Past the cap, drop the OLDEST data (resynced to a line
    // boundary so we never resume mid-message) rather than grow unbounded.
    if (m_bridgeSendBuf.size() + len + 1 > BRIDGE_SEND_BUF_MAX)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: send queue over %u bytes — dropping oldest (C# slow or down?)",
            me->GetName(), (uint32)BRIDGE_SEND_BUF_MAX);
        m_bridgeSendBuf.erase(0, m_bridgeSendBuf.size() / 2);
        size_t nl = m_bridgeSendBuf.find('\n');
        if (nl != std::string::npos)
            m_bridgeSendBuf.erase(0, nl + 1);   // realign to the next whole line
        else
            m_bridgeSendBuf.clear();
    }
 
    // Queue the whole line. JSON-lines protocol → one '\n' terminator.
    m_bridgeSendBuf.append(json, len);
    m_bridgeSendBuf.push_back('\n');
 
    // Common case: drain right away so event latency stays low.
    BridgeFlush();
}

void AiBotAI::BridgeFlush()
{
    if (!m_bridgeConnected || m_bridgeSocket == BRIDGE_INVALID_SOCKET)
        return;
 
    while (!m_bridgeSendBuf.empty())
    {
        ssize_t sent = send(m_bridgeSocket,
                            m_bridgeSendBuf.data(),
                            m_bridgeSendBuf.size(), 0);
 
        if (sent > 0)
        {
            m_bridgeSendBuf.erase(0, (size_t)sent);
            continue; // keep draining until empty or the socket pushes back
        }
 
        if (sent == 0)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-BRIDGE] %s: send returned 0 (peer closed), disconnecting", me->GetName());
            BridgeDisconnect();
            return;
        }
 
        // sent < 0
#ifdef _WIN32
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK)
            return; // kernel buffer full — keep remainder, retry next tick (NOT an error)
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return; // kernel buffer full — keep remainder, retry next tick (NOT an error)
        if (errno == EINTR)
            continue; // interrupted before any bytes sent — retry immediately
#endif
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: send error, disconnecting", me->GetName());
        BridgeDisconnect();
        return;
    }
}

void AiBotAI::BridgeSendHello()
{
    if (!m_bridgeConnected || m_bridgeHelloSent)
        return;

    char json[512];
    snprintf(json, sizeof(json),
        "{\"type\":\"HELLO\",\"payload\":{"
        "\"guid\":%u,\"name\":\"%s\",\"race\":%u,\"classId\":%u,"
        "\"level\":%u,\"mapId\":%u,\"zoneId\":%u,"
        "\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}}",
        me->GetGUIDLow(), me->GetName(), me->GetRace(), me->GetClass(),
        me->GetLevel(), me->GetMapId(), me->GetZoneId(),
        me->GetPositionX(), me->GetPositionY(), me->GetPositionZ());

    BridgeSend(json);

    // Restore tracked quest from server quest log (restart recovery)
    if (m_trackedQuestId == 0)
    {
        const auto& questMap = me->GetQuestStatusMap();
        for (const auto& pair : questMap)
        {
            if (pair.second.m_status == QUEST_STATUS_INCOMPLETE ||
                pair.second.m_status == QUEST_STATUS_COMPLETE)
            {
                m_trackedQuestId = pair.first;
                break;  // take the first active quest
            }
        }
    }

    m_bridgeHelloSent = true;
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: HELLO sent (guid=%u)", me->GetName(), me->GetGUIDLow());
}

// ============================================================
// BridgeSendState — REPLACEMENT (retire-the-pull build)
//
// REPLACES: the entire existing AiBotAI::BridgeSendState() in AiBotAIBridge.cpp.
//
// WHAT CHANGED vs the previous version (three things, all inside this one method —
// no header change, no new members):
//   1. Builds the FULL quest-log snapshot (every non-rewarded active log entry, with
//      status + mob/item counts) into a bounded std::string `questBlob` — the exact
//      same loop and pipe format BridgeHandleQueryQuestStatus used for QUEST_STATUS_ALL.
//   2. Emits it as a new STATE field  "quests":"<blob>"  so C# ctx.QuestLog becomes a
//      continuously-maintained mirror of the player's quest log on the 5s heartbeat —
//      no QUERY_QUEST_STATUS round-trip, no request/reply cache to go stale/partial/empty.
//   3. Bumps the STATE buffer  char json[1536] -> char json[4096]  to fit the blob.
//      The blob is JSON-safe by construction (only digits, ':', ',', '|'), so it needs
//      no escaping inside the STATE string, and the 20-slot quest-log cap keeps it well
//      under the buffer.
//
// The held-task echo fields (taskKind/taskActivity/taskCreature/taskDest*/taskKills) and
// every other STATE field are byte-for-byte unchanged.
//
// FOLLOW-ON (optional, not required for the fix): QUERY_QUEST_STATUS /
// BridgeHandleQueryQuestStatus / SendBridgeEvent("QUEST_STATUS_ALL") are now dead on the
// solo path. They can be left in place as harmless dead code (the C# solo path no longer
// sends QUERY and no longer has a QUEST_STATUS_ALL handler), or removed later. Leaving the
// C++ handler in keeps the group path functional if grouping is ever flipped on.
// ============================================================
void AiBotAI::BridgeSendState()
{
    if (!m_bridgeConnected)
        return;

    const char* taskStr = "IDLE";
    if (me->IsDead())
        taskStr = "DEAD";
    else if (me->IsInCombat())
        taskStr = "COMBAT";
    else if (m_currentTask.type == TASK_GRIND)
        taskStr = "GRINDING";
    else if (m_currentTask.type == TASK_TAXI)
        taskStr = "FLYING";
    else if (m_currentTask.type == TASK_MOVE_TO)
        taskStr = "MOVING";
    else if (me->IsMoving())
        taskStr = "MOVING";

    uint32 freeSlots = 0;
    for (int fi = INVENTORY_SLOT_ITEM_START; fi < INVENTORY_SLOT_ITEM_END; ++fi)
        if (!me->GetItemByPos(INVENTORY_SLOT_BAG_0, fi))
            ++freeSlots;
    for (int fi = INVENTORY_SLOT_BAG_START; fi < INVENTORY_SLOT_BAG_END; ++fi)
        if (Bag* pFBag = (Bag*)me->GetItemByPos(INVENTORY_SLOT_BAG_0, fi))
            if (pFBag->GetProto()->Class == ITEM_CLASS_CONTAINER && pFBag->GetProto()->SubClass == ITEM_SUBCLASS_CONTAINER)
                for (uint32 fj = 0; fj < pFBag->GetBagSize(); ++fj)
                    if (!me->GetItemByPos(fi, fj))
                        ++freeSlots;
    uint32 totalSlots = (INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START);
    for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
    {
        if (Bag* pBag = (Bag*)me->GetItemByPos(INVENTORY_SLOT_BAG_0, (uint8)i))
            if (pBag->GetProto()->Class == ITEM_CLASS_CONTAINER &&
                pBag->GetProto()->SubClass == ITEM_SUBCLASS_CONTAINER)
                totalSlots += pBag->GetBagSize();
    }

    // --- Min equipped durability % (feeds the C# repair trigger) ---
    // 100 = no damageable gear or all full. The lowest slot drives the decision;
    // a single 0-durability weapon tanks damage output, so min (not average) is right.
    // Mirrors the durability read in BridgeHandleRepairItems exactly.
    uint32 minDurabilityPct = 100;
    for (int i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        Item* item = me->GetItemByPos(INVENTORY_SLOT_BAG_0, (uint8)i);
        if (!item)
            continue;
        uint32 maxDur = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
        if (maxDur == 0)
            continue;   // rings/trinkets/etc. have no durability
        uint32 dur = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
        uint32 pct = (dur * 100) / maxDur;
        if (pct < minDurabilityPct)
            minDurabilityPct = pct;
    }

    // --- [PLAYERPARTY] escort echo (2026-07-07) + boss range (2026-07-08) ---
    // pparty: 1 = this bot's group contains a REAL player (FindPartyBoss resolves) → C# stands
    // down to the player-party hold and C++ owns the whole behaviour. ppdist: distance to that
    // boss — -1 no boss, 99999 boss on ANOTHER map (instance/boat), else 3D yards. Feeds the
    // C# hub-errand abort guard (boss >150yd / off-map → drop the rounds, resume follow).
    // Same cheap group walk, once per 5s STATE.
    // [MULTI-HUMAN] Both key on FindEscortBoss — null iff no real player is in the group
    // (the SAME truth value as FindPartyBoss, so pparty's meaning is unchanged), but ppdist
    // now measures the bot's OWN assigned human, which is exactly who the hub-errand abort
    // guard should watch: the bot returns to ITS human, not necessarily the leader.
    uint32 pparty = 0u;
    int ppdist = -1;
    if (Player* pBoss = FindEscortBoss())
    {
        pparty = 1u;
        ppdist = (pBoss->GetMapId() == me->GetMapId()) ? (int)me->GetDistance(pBoss) : 99999;
    }
    else if (me->GetSession() && !me->GetSession()->GetBot())
    {
        // [SUI] Enrolled REAL character with no boss to key on: its human is in
        // the free camera. Owner decree (2026-08-10): the personal party is
        // non-autonomous unless made explicit later — echo pparty=1 so the C#
        // brain holds Idle; movement comes only from CMSG_SUI_ORDER commands.
        pparty = 1u;
    }

    // --- Active quest status from server (authoritative) ---
    uint32 questStatus = 0;  // 0 = no tracked quest
    if (m_trackedQuestId > 0)
        questStatus = (uint32)me->GetQuestStatus(m_trackedQuestId);

    // --- §4 held-task echo: the kind C++ is ACTUALLY running + within-objective activity ---
    // Independent of taskStr above (display/status). The committed task KIND stays GRIND/MOVE_TO
    // through a fight, which is exactly what the C# kind-match needs. All derived from existing
    // state — no new members, no new tracking.
    const char* taskKindStr;
    switch (m_currentTask.type)
    {
        case TASK_GRIND:   taskKindStr = "GRIND";   break;
        case TASK_MOVE_TO: taskKindStr = "MOVE_TO"; break;
        case TASK_TAXI:    taskKindStr = "MOVE_TO"; break;   // in transit = a kind of travel
        default:           taskKindStr = "IDLE";    break;
    }

    // Within-objective activity — the headway signal C# reacts to (not a wall clock). Order is a
    // priority ladder: combat trumps everything; eating/drinking is a stationary recover; movement
    // is travel; a stationary grind is "between targets" (searching). Anything else = idle.
    const char* activityStr = "idle";
    if (me->IsInCombat())
        activityStr = "engaged";
    else if (me->HasAura(AB_SPELL_FOOD) || me->HasAura(AB_SPELL_DRINK))
        activityStr = "recovering";
    else if (me->IsMoving())
        activityStr = "traveling";
    else if (m_currentTask.type == TASK_GRIND)
        activityStr = "searching";   // committed to a grind, between targets — NOT a clock-based stall
    // else: idle

    // --- Full quest-log snapshot (RETIRES the QUERY_QUEST_STATUS pull) ---
    // Pushed on EVERY STATE so C# ctx.QuestLog is a continuously-maintained mirror of the player's
    // quest log — never a request/reply cache that can go stale, partial, or empty. Same pipe format
    // the old QUEST_STATUS_ALL used, so the C# parser is reused verbatim:
    //   questId:status:mob0,mob1,mob2,mob3:item0,item1,item2,item3 | questId:...
    // status: 1=COMPLETE, 3=INCOMPLETE (VMaNGOS enum). Only non-rewarded active log entries are
    // included (rewarded/turned-in are excluded — that is how a turn-in legitimately drops out). The
    // blob is built as a bounded std::string (the 20-slot log fits well under the buffer) and its
    // characters are JSON-safe (digits, ':', ',', '|'), so it needs no escaping inside the STATE string.
    std::string questBlob;
    {
        const auto& qMap = me->GetQuestStatusMap();
        for (const auto& pair : qMap)
        {
            const auto& qData = pair.second;

            if (qData.m_rewarded)
                continue;   // turned in — not an active log entry
            if (qData.m_status != QUEST_STATUS_INCOMPLETE &&
                qData.m_status != QUEST_STATUS_COMPLETE)
                continue;   // skip QUEST_STATUS_NONE / UNAVAILABLE

            if (!questBlob.empty())
                questBlob += "|";

            char qEntry[128];
            snprintf(qEntry, sizeof(qEntry), "%u:%u:%u,%u,%u,%u:%u,%u,%u,%u",
                pair.first,
                (uint32)qData.m_status,
                qData.m_creatureOrGOcount[0], qData.m_creatureOrGOcount[1],
                qData.m_creatureOrGOcount[2], qData.m_creatureOrGOcount[3],
                qData.m_itemcount[0], qData.m_itemcount[1],
                qData.m_itemcount[2], qData.m_itemcount[3]);
            questBlob += qEntry;
        }
    }

    char json[4096];
    snprintf(json, sizeof(json),
        "{\"type\":\"STATE\",\"payload\":{"
        "\"guid\":%u,\"health\":%u,\"maxHealth\":%u,"
        "\"mana\":%u,\"maxMana\":%u,\"level\":%u,"
        "\"mapId\":%u,\"zoneId\":%u,"
        "\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,"
        "\"inCombat\":%s,\"isDead\":%s,"
        "\"targetGuid\":%u,\"taskState\":\"%s\","
        "\"freeSlots\":%u,\"totalSlots\":%u,\"copper\":%u,\"durability\":%u,\"pparty\":%u,\"ppdist\":%d,"
        "\"taskKind\":\"%s\",\"taskActivity\":\"%s\","
        "\"taskCreature\":%u,\"taskDestX\":%.2f,\"taskDestY\":%.2f,\"taskDestZ\":%.2f,\"taskKills\":%d,"
        "\"quests\":\"%s\","
        "\"questId\":%u,\"questStatus\":%u,\"possessed\":%u}}",
        me->GetGUIDLow(),
        me->GetHealth(), me->GetMaxHealth(),
        me->GetPower(POWER_MANA), me->GetMaxPower(POWER_MANA),
        me->GetLevel(),
        me->GetMapId(), me->GetZoneId(),
        me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(),
        me->IsInCombat() ? "true" : "false",
        me->IsDead() ? "true" : "false",
        me->GetTargetGuid().IsEmpty() ? 0 : me->GetTargetGuid().GetCounter(),
        taskStr,
        freeSlots, totalSlots, me->GetMoney(), minDurabilityPct, pparty, ppdist,
        taskKindStr, activityStr,
        m_currentTask.creatureEntry, m_currentTask.x, m_currentTask.y, m_currentTask.z, m_currentTask.killCount,
        questBlob.c_str(),
        m_trackedQuestId, questStatus, m_possessed ? 1u : 0u);

    BridgeSend(json);
}

void AiBotAI::BridgeSendEvent(const char* eventType, const char* data)
{
    if (!m_bridgeConnected)
        return;

    char json[512];
    snprintf(json, sizeof(json),
        "{\"type\":\"EVENT\",\"payload\":{"
        "\"guid\":%u,\"event\":\"%s\",\"data\":\"%s\"}}",
        me->GetGUIDLow(), eventType, data);

    BridgeSend(json);
}

void AiBotAI::BridgeRecv()
{
    if (!m_bridgeConnected || m_bridgeSocket == BRIDGE_INVALID_SOCKET)
        return;

    // Read available data into buffer (non-blocking)
    int space = BRIDGE_RECV_BUF_SIZE - m_bridgeRecvLen - 1;
    if (space <= 0)
    {
        // Buffer full with no newline — discard
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: recv buffer overflow, clearing", me->GetName());
        m_bridgeRecvLen = 0;
        return;
    }

    ssize_t n = recv(m_bridgeSocket, m_bridgeRecvBuf + m_bridgeRecvLen, space, 0);
    if (n > 0)
    {
        m_bridgeRecvLen += n;
        m_bridgeRecvBuf[m_bridgeRecvLen] = '\0';

        // Process complete lines
        char* start = m_bridgeRecvBuf;
        char* newline;
        while ((newline = strchr(start, '\n')) != nullptr)
        {
            *newline = '\0';
            if (newline > start) // skip empty lines
                BridgeProcessLine(start);
            start = newline + 1;
        }

        // Shift remaining partial data to front
        int remaining = m_bridgeRecvLen - (int)(start - m_bridgeRecvBuf);
        if (remaining > 0 && start != m_bridgeRecvBuf)
            memmove(m_bridgeRecvBuf, start, remaining);
        m_bridgeRecvLen = remaining;
    }
    else if (n == 0)
    {
        // Clean disconnect
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: server closed connection", me->GetName());
        BridgeDisconnect();
    }
    else
    {
        // n < 0: check if it's just EAGAIN/EWOULDBLOCK (no data yet)
#ifdef _WIN32
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK)
#else
        if (errno != EAGAIN && errno != EWOULDBLOCK)
#endif
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: recv error, disconnecting", me->GetName());
            BridgeDisconnect();
        }
    }
}

// Minimal JSON field extraction — no library needed.
// Finds "key":value in a flat JSON object. Works for our simple payloads.
static bool JsonExtractFloat(const char* json, const char* key, float& out)
{
    // Search for "key": pattern
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ') p++;
    out = (float)atof(p);
    return true;
}

static bool JsonExtractInt(const char* json, const char* key, int& out)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ') p++;
    out = atoi(p);
    return true;
}

static bool JsonExtractString(const char* json, const char* key, char* out, int maxLen)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char* p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    const char* end = strchr(p, '"');
    if (!end) return false;
    int len = std::min((int)(end - p), maxLen - 1);
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

void AiBotAI::BridgeProcessLine(const char* line)
{
    // Extract "type" field
    char msgType[32] = {0};
    if (!JsonExtractString(line, "type", msgType, sizeof(msgType)))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: no 'type' in message", me->GetName());
        return;
    }

    // [SUI] While a real player drives this bot every mutating command would
    // execute under the human's feet (MOVE_TO would yank the mover). Drop with
    // an event so the C# supervisor sees an explicit answer, not a stall.
    // ...unless the human is COMMANDING it from the free view, where there are no feet to
    // execute under: the client's controller is the camera, its movement stream is parked, and
    // an RTS order is the only thing that can move this bot at all. Dropping the order here is
    // what made a commanded toon the one party member that ignored a move.
    if (m_possessed && strcmp(msgType, "PING") != 0 &&
        !SuiPossess::IsCommandedFromFreeView(me))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "[AIBOT-BRIDGE] %s: dropped %s (possessed)",
            me->GetName(), msgType);
        BridgeSendEvent("POSSESSED_DROP", msgType);
        return;
    }

    if (strcmp(msgType, "MOVE_TO") == 0)
        BridgeHandleMoveTo(line);
    else if (strcmp(msgType, "TELEPORT_TO") == 0)
        BridgeHandleTeleport(line);
    else if (strcmp(msgType, "SAY_TEXT") == 0)
        BridgeHandleSayText(line);
    else if (strcmp(msgType, "QUEST_INTERACT") == 0)
        BridgeHandleQuestInteract(line);
    else if (strcmp(msgType, "ABANDON_QUEST") == 0)
        BridgeHandleAbandonQuest(line);
    else if (strcmp(msgType, "LEARN_SPELL") == 0)
        BridgeHandleLearnSpell(line);
    else if (strcmp(msgType, "ATTACK_TARGET") == 0)
        BridgeHandleAttackTarget(line);
    else if (strcmp(msgType, "INTERACT_NPC") == 0)
        BridgeHandleInteractNpc(line);
    else if (strcmp(msgType, "SET_TASK") == 0)
        BridgeHandleSetTask(line);
    else if (strcmp(msgType, "COMBAT_DIRECTIVE") == 0)
        BridgeHandleCombatDirective(line);
    else if (strcmp(msgType, "TAKE_FLIGHT") == 0)
        BridgeHandleTakeFlight(line);
    else if (strcmp(msgType, "SELL_ITEMS") == 0)
        BridgeHandleSellItems(line);
    else if (strcmp(msgType, "REPAIR_AT_NPC") == 0)
        BridgeHandleRepairItems(line);
    else if (strcmp(msgType, "GEAR_UP") == 0)
        BridgeHandleGearUp(line);
    else if (strcmp(msgType, "RESURRECT") == 0)
        BridgeHandleResurrect(line);
    else if (strcmp(msgType, "TRAIN_AT_NPC") == 0)
        BridgeHandleTrain(line);
    else if (strcmp(msgType, "USE_GAMEOBJECT") == 0)
        BridgeHandleUseGameObject(line);
    else if (strcmp(msgType, "QUEST_CAST") == 0)
        BridgeHandleQuestCast(line);
    else if (strcmp(msgType, "FORM_GROUP") == 0)
        BridgeHandleFormGroup(line);
    else if (strcmp(msgType, "DISBAND_GROUP") == 0)
        BridgeHandleDisbandGroup(line);
    else if (strcmp(msgType, "SET_ESCORT") == 0)
        BridgeHandleSetEscort(line);
    else if (strcmp(msgType, "LOAD_ROTATION") == 0)
        BridgeHandleLoadRotation(line);
    else if (strcmp(msgType, "QUERY_QUEST_STATUS") == 0)
        BridgeHandleQueryQuestStatus(line);
    else if (strcmp(msgType, "PING") == 0)
        { /* do nothing, keepalive */ }
    else
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: unknown command '%s'", me->GetName(), msgType);
}

// ============================================================
// BridgeHandleTeleport — generic live-bot teleport primitive
//
// C# drives the teleport-assist: when a final-approach MOVE_TO to a trainer /
// vendor / repair NPC no_path's twice while the bot is already in the vicinity
// (a nav-dead pocket — building interior, bad mesh stitch at the NPC), C# saves
// the bot's current on-mesh position as a return anchor and sends TELEPORT_TO the
// NPC. The bot does its business at real proximity, then C# sends TELEPORT_TO the
// saved anchor to put it back where it came from. All round-trip state lives in C#
// (BotContext) — this handler is the dumb primitive: relocate a LIVE bot on the
// same map and ack.
//
// Uses NearTeleportTo (same-map instant relocation) — NOT the cross-map far-port,
// which defers behind a loading screen / IsBeingTeleported(). Cross-map is refused
// (the assist is always same-map). An optional max_dist caps the hop as a safety
// rail so a bad C# coord can't fling a live bot across the zone (the assist sends
// max_dist≈50; 0/absent = no cap, for a future long-range hearth).
//
// PLACEMENT: after BridgeHandleMoveTo in AiBotAI.cpp
// DISPATCH:  BridgeProcessLine → else if (strcmp(msgType,"TELEPORT_TO")==0) BridgeHandleTeleport(line);
// HEADER:    void BridgeHandleTeleport(const char* json);
// ============================================================
void AiBotAI::BridgeHandleTeleport(const char* json)
{
    if (!me || !me->IsInWorld())
        return;

    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json;

    int mapId = (int)me->GetMapId();   // default = current map (a missing mapId is same-map)
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float o = me->GetOrientation();    // default = keep facing
    float maxDist = 0.0f;              // 0 = no cap

    JsonExtractInt(payload, "mapId", mapId);
    bool haveX = JsonExtractFloat(payload, "x", x);
    bool haveY = JsonExtractFloat(payload, "y", y);
    bool haveZ = JsonExtractFloat(payload, "z", z);
    JsonExtractFloat(payload, "o", o);
    JsonExtractFloat(payload, "max_dist", maxDist);

    if (!haveX || !haveY || !haveZ)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[AIBOT-TELEPORT] %s: TELEPORT_TO bad payload (missing x/y/z)", me->GetName());
        BridgeSendEvent("TELEPORT_FAIL", "reason=bad_payload");
        return;
    }

    // Dead bots are owned by the death-recovery path (ghost-walk / graveyard port).
    // A teleport-assist must never fire on a ghost.
    if (me->IsDead() || me->GetDeathState() == DEAD)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-TELEPORT] %s: TELEPORT_TO refused — bot is dead", me->GetName());
        BridgeSendEvent("TELEPORT_FAIL", "reason=dead");
        return;
    }

    // Same-map only. NearTeleportTo is a same-map relocation; cross-map is the deferred
    // far-port (loading screen) and is out of scope for the assist.
    if ((uint32)mapId != me->GetMapId())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-TELEPORT] %s: TELEPORT_TO cross-map refused (current=%u target=%d)",
            me->GetName(), me->GetMapId(), mapId);
        BridgeSendEvent("TELEPORT_FAIL", "reason=cross_map");
        return;
    }

    // Safety rail: refuse a hop beyond the caller's cap (assist sends ~50yd). 0 = no cap.
    float dist = me->GetDistance2d(x, y);
    if (maxDist > 0.0f && dist > maxDist)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-TELEPORT] %s: TELEPORT_TO refused — %.1fyd > max_dist %.1f (to %.1f,%.1f,%.1f)",
            me->GetName(), dist, maxDist, x, y, z);
        char buf[160];
        snprintf(buf, sizeof(buf), "reason=too_far|dist=%.1f|max_dist=%.1f", dist, maxDist);
        BridgeSendEvent("TELEPORT_FAIL", buf);
        return;
    }

    // [GROUND] Re-ground the C#-supplied Z. The assist sends NPC spawn coords (already
    // grounded → no-op); the hearth sends homebind/hub coords — grounding here is what
    // keeps a hearth from re-floating a bot at a new coord. x/y untouched, so the 15yd
    // NPC-find at the assist target is unaffected.
    ReGroundZ(x, y, z, "teleport");

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-TELEPORT] %s: TELEPORT_TO (%.1f,%.1f,%.1f) o=%.2f map=%d — %.1fyd hop",
        me->GetName(), x, y, z, o, mapId, dist);

    // Clean reset: stop the failed approach, drop the stored path + task, then relocate.
    // The MOVE_TO that no_path'd is finished; nothing in-flight needs to survive (the
    // interaction that follows — TRAIN/SELL/REPAIR — doesn't read m_currentTask).
    StopMoving();
    ClearStoredPath();
    m_currentTask.Clear();

    // Fresh journey state so the NEXT real MOVE_TO re-arms its one-shot recoveries from
    // the new start poly.
    m_didBoundaryExit = false;
    m_didNavmeshSnap  = false;

    // Suppress the idle wander for a few seconds so the bot holds at the NPC until C#
    // fires the interaction (task is now IDLE; without this DoRandomWander could hop 15yd
    // off the trainer before TRAIN_AT_NPC arrives — recoverable via its 50yd fallback, but
    // cleaner to just stand still).
    m_wanderTimer = 5000;

    me->NearTeleportTo(x, y, z, o);

    // Ack carries the (now grounded) REQUESTED target — C# updates ctx.Pos to it so the
    // planner sees DistToTarget≈0 and fires the interaction next tick. Actual landed pos
    // logged so a teleport that didn't take is visible on the first run.
    char ack[160];
    snprintf(ack, sizeof(ack), "x=%.1f|y=%.1f|z=%.1f|map=%u", x, y, z, me->GetMapId());
    BridgeSendEvent("TELEPORT_ACK", ack);

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-TELEPORT] %s: NearTeleportTo issued — requested (%.1f,%.1f,%.1f), now reads (%.1f,%.1f,%.1f) map=%u",
        me->GetName(), x, y, z,
        me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), me->GetMapId());
}

// ============================================================
// BridgeHandleMoveTo — REPLACEMENT (wolf-meat fix, 2026-06-30)
//
// REPLACES: the entire existing AiBotAI::BridgeHandleMoveTo(const char* json) in
// AiBotAIBridge.cpp.
//
// ONLY CHANGE vs the previous version: three new optional fields parsed from the
// payload — alt_entry1/2/3 — stashed onto m_currentTask.altCreatureEntries[0..2].
// Everything else (mapId/x/y/z, the existing creature_entry/kill_count/grind_radius
// enrichment, the arrival-jitter block, the cross-map guard, the journey-state resets,
// the call into MoveToDestination) is byte-for-byte unchanged.
//
// WHY: an item-drop objective can have more than one creature that satisfies it with
// no real priority between them (Young Wolf + Timber Wolf both drop Tough Wolf Meat at
// the same drop chance, both spawning in the same field near the giver). C# now ships
// up to 3 tied alternates alongside the primary creature_entry. They do NOT change
// where the bot walks (the dispatch coordinate is still creature_entry's resolved
// point) — they widen what counts as a valid hit once the bot is out there:
// ScanApproachTarget's valid-kill union, SelectGrindTarget's match checks, and the
// kill-credit check in UpdateAI all now match m_currentTask.MatchesObjectiveEntry(...)
// instead of `entry == m_currentTask.creatureEntry`. Absent fields parse as 0 (JsonExtractInt's
// untouched-on-miss behavior) — a pre-this-fix C# brain or a kill-objective leg that never
// sends these keys leaves altCreatureEntries all 0, which MatchesObjectiveEntry treats as
// "no alternates" — today's exact behavior, byte-for-byte.
//
// REQUIRES: AiBotTaskData::altCreatureEntries[MAX_ALT_ENTRIES] + MatchesObjectiveEntry()
// added to AiBotAIMain.h (struct AiBotTaskData) — see that header's 2026-06-30 edit.
// ============================================================
void AiBotAI::BridgeHandleMoveTo(const char* json)
{
    float x = 0, y = 0, z = 0;
    int mapId = 0;

    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json;

    JsonExtractInt(payload, "mapId", mapId);
    JsonExtractFloat(payload, "x", x);
    JsonExtractFloat(payload, "y", y);
    JsonExtractFloat(payload, "z", z);

    // ── §4 optional objective enrichment ──
    // A kill-objective MOVE_TO carries creature_entry / grind_radius / kill_count so
    // C++ engages the mob during the approach (ScanApproachTarget) and hands off to
    // GRIND in place — never marching to the deep loader coord. Absent → all stay 0 →
    // a plain MOVE_TO (repositioning / travel / gather): arrives, emits TASK_COMPLETE.
    int entry = 0, killCount = 0;
    float grindRadius = 0.0f;
    JsonExtractInt(payload, "creature_entry", entry);
    JsonExtractInt(payload, "kill_count", killCount);
    JsonExtractFloat(payload, "grind_radius", grindRadius);

    // ── wolf-meat fix: tied item-drop alternates (2026-06-30) ──
    // Absent on a plain MOVE_TO and on every kill-objective leg — stays 0, meaning
    // "no alternate" (AiBotTaskData::MatchesObjectiveEntry only matches a non-zero slot).
    int altEntry1 = 0, altEntry2 = 0, altEntry3 = 0;
    JsonExtractInt(payload, "alt_entry1", altEntry1);
    JsonExtractInt(payload, "alt_entry2", altEntry2);
    JsonExtractInt(payload, "alt_entry3", altEntry3);

    if ((uint32)mapId != me->GetMapId())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: cross-map move not supported (current=%u, target=%d)",
            me->GetName(), me->GetMapId(), mapId);
        return;
    }

    // ── Arrival jitter (never path to the EXACT dest coord) ──
    // Pathing precisely TO certain coords (NPC spawn points, loader coords) lands the bot on a
    // bad poly / seam edge and trips the off-mesh strand (Ujekawab @ (-8933.5,-136.5): NOPATH
    // from on top of the dest). So for a PLAIN travel MOVE_TO we resolve the request to a
    // VALIDATED point 0.2–2.0yd off the real coord at a random angle: sample the ring, path-check
    // each candidate, take the first that isn't NOPATH. This dodges the bad poly AND fans bots out
    // so they don't stack on one pixel. Bounded tries → if the whole neighborhood is unmeshed we
    // fall back to the exact coord and let MoveToDestination's no_path / off-mesh recovery own it.
    //
    // SKIPPED for an enriched-objective MOVE_TO (entry != 0): that coord is a grind hint that
    // converts to grind-in-place at the mouth — it is never an exact-arrival target, so no jitter.
    if (entry == 0 && !me->IsInCombat())
    {
        float jx = x, jy = y, jz = z;
        bool found = false;
        for (int t = 0; t < AIBOT_ARRIVE_JITTER_TRIES; ++t)
        {
            float ang  = frand(0.0f, 2.0f * M_PI_F);
            float dist = frand(AIBOT_ARRIVE_JITTER_MIN, AIBOT_ARRIVE_JITTER_MAX);
            float cx = x + dist * cosf(ang);
            float cy = y + dist * sinf(ang);
            float cz = z;
            ReGroundZ(cx, cy, cz, "arrive-jitter");   // keep the candidate on the floor before path-checking

            PathInfo probe(me);
            probe.calculate(cx, cy, cz);
            if (!(probe.getPathType() & PATHFIND_NOPATH))
            {
                jx = cx; jy = cy; jz = cz;
                found = true;
                break;
            }
        }

        if (found)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-BRIDGE] %s: MOVE_TO jittered (%.1f,%.1f) -> (%.1f,%.1f) %.1fyd off (dodging exact-coord poly)",
                me->GetName(), x, y, jx, jy, me->GetDistance2d(jx, jy) > 0 ? hypotf(jx - x, jy - y) : 0.0f);
            x = jx; y = jy; z = jz;
        }
        else
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-BRIDGE] %s: MOVE_TO jitter found no pathable ring point in %d tries — using exact (%.1f,%.1f)",
                me->GetName(), AIBOT_ARRIVE_JITTER_TRIES, x, y);
        }
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: MOVE_TO map=%d (%.1f, %.1f, %.1f)%s",
        me->GetName(), mapId, x, y, z, entry ? " [objective]" : "");

    // Stash the objective hint on the task BEFORE MoveToDestination. MoveToDestination
    // only writes type/x/y/z, so these persist through every continuation leg until the
    // approach scan or an arrival hands off to GRIND (re-centering on the bot).
    m_currentTask.creatureEntry = (uint32)entry;
    m_currentTask.radius        = grindRadius;
    m_currentTask.killGoal      = killCount;
    m_currentTask.killCount     = 0;
    m_currentTask.altCreatureEntries[0] = (uint32)altEntry1;
    m_currentTask.altCreatureEntries[1] = (uint32)altEntry2;
    m_currentTask.altCreatureEntries[2] = (uint32)altEntry3;
    m_approachScanTimer         = 0;   // scan on the first tick of this journey

    // One MOVE_TO = one journey to (x,y,z). MoveToDestination walks the first leg;
    // MovementInform / the UpdateAI resume block continue it past each partial-path
    // horizon until arrival. Distance is no longer a failure mode.
    m_didBoundaryExit = false;   // fresh journey — allow one outbound seam-cross
    m_didNavmeshSnap  = false;   // fresh journey — allow one off-mesh-start snap
    MoveToDestination(x, y, z);
}
 

// ============================================================
// BridgeHandleLoadRotation — [ROTATION] load/replace/clear the custom slate (2026-07-16).
//
// Payload: {"profile":"priest_smite_v1","data":"spellId:prio:target:hpMin:hpMax:aura:present|..."}
// The pipe format is the house wire idiom (quest log, TRAIN_ACK). C# pre-sorts by
// priority; C++ preserves order and drops the priority field after parse (it exists on
// the wire only so a human can read a captured payload). Empty/absent data CLEARS the
// slate — vanilla class AI resumes next tick, the RotationSlate else-branch contract.
//
// SpellEntry resolution happens HERE, once: unknown spell IDs and spells the bot has
// not learned parse to pSpell=null and are skipped at tick time; the ROTATION_ACK
// reports loaded vs skipped counts so C# can log a bad profile loudly instead of the
// bot silently doing less than the profile says.
// ============================================================
void AiBotAI::BridgeHandleLoadRotation(const char* json)
{
    char profileBuf[64] = {0};
    JsonExtractString(json, "profile", profileBuf, sizeof(profileBuf));

    char dataBuf[2048] = {0};
    JsonExtractString(json, "data", dataBuf, sizeof(dataBuf));

    m_rotation.clear();
    m_rotationProfile = profileBuf;

    if (dataBuf[0] == '\0')
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-ROTATION] %s: slate CLEARED — vanilla class AI resumes", me->GetName());
        BridgeSendEvent("ROTATION_ACK", "profile=|loaded=0|skipped=0");
        return;
    }

    uint32 loaded = 0, skipped = 0;
    char* saveptr = nullptr;
    for (char* seg = strtok_r(dataBuf, "|", &saveptr); seg != nullptr; seg = strtok_r(nullptr, "|", &saveptr))
    {
        uint32 spellId = 0, auraId = 0;
        int prio = 0, target = 1, hpMin = 0, hpMax = 100, present = 0;
        if (sscanf(seg, "%u:%d:%d:%d:%d:%u:%d", &spellId, &prio, &target, &hpMin, &hpMax, &auraId, &present) != 7)
        {
            ++skipped;
            continue;
        }

        RotationInstruction inst;
        inst.spellId = spellId;
        inst.pSpell = me->HasSpell(spellId) ? sSpellMgr.GetSpellEntry(spellId) : nullptr;
        inst.target = (uint8)target;
        inst.hpMin = hpMin;
        inst.hpMax = hpMax;
        inst.auraId = auraId;
        inst.auraPresent = (present != 0);
        m_rotation.push_back(inst);

        if (inst.pSpell)
            ++loaded;
        else
        {
            ++skipped;
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-ROTATION] %s: spell %u in profile '%s' unknown/unlearned — instruction will be skipped",
                me->GetName(), spellId, profileBuf);
        }
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-ROTATION] %s: slate '%s' loaded — %u castable, %u skipped (of %u instructions)",
        me->GetName(), profileBuf, loaded, skipped, (uint32)m_rotation.size());

    char ack[160];
    snprintf(ack, sizeof(ack), "profile=%s|loaded=%u|skipped=%u", profileBuf, loaded, skipped);
    BridgeSendEvent("ROTATION_ACK", ack);
}

// ============================================================
// BridgeHandleSetEscort — [FOLLOW-CMD] "{bot} follow {player}" (2026-07-16)
//
// Sets/clears the escort override consumed by FindEscortBoss. C# recognizes the
// addressed party-chat command deterministically (BotBridgeService CHAT_RECV) and
// sends {"player_name": "Athren"} — or "" to revert to the GUIDLow-modulo auto
// split ("{bot} follow auto" / bare "{bot} follow"). The name is stored LOWERCASED;
// resolution against group members is case-insensitive at follow time, and a name
// that never resolves (offline / left / typo) silently falls back to the auto
// split, so a bad command can never strand a bot. Fire-and-forget: no ack event —
// C# speaks the party-chat confirmation itself.
// ============================================================
void AiBotAI::BridgeHandleSetEscort(const char* json)
{
    char nameBuf[64] = {0};
    JsonExtractString(json, "player_name", nameBuf, sizeof(nameBuf));   // absent/empty = clear

    std::string lowered;
    for (char const* p = nameBuf; *p; ++p)
        lowered.push_back((char)tolower((unsigned char)*p));

    m_escortOverrideName = lowered;
    if (m_escortOverrideName.empty())
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-PARTY] %s: escort override CLEARED (auto split)", me->GetName());
    else
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-PARTY] %s: escort override -> '%s'", me->GetName(), nameBuf);
}

void AiBotAI::BridgeHandleSayText(const char* json)
{
    char text[256] = {0};
    char target[64] = {0};
    char channel[64] = {0};
    int chatType = 0;

    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json;

    JsonExtractString(payload, "text", text, sizeof(text));
    JsonExtractInt(payload, "chatType", chatType);
    JsonExtractString(payload, "target", target, sizeof(target));
    JsonExtractString(payload, "channel", channel, sizeof(channel));

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: SAY_TEXT type=%d target=%s channel=%s: %s",
        me->GetName(), chatType, target, channel, text);

    if (strlen(text) == 0)
        return;

    if (chatType == 7 && strlen(target) > 0)
    {
        // WHISPER — build and send whisper packet directly to target player
        // Player class doesn't have Whisper(); we use ChatHandler::BuildChatPacket
        Player* pTarget = sObjectMgr.GetPlayer(target);
        if (pTarget && pTarget->GetSession())
        {
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, text, LANG_UNIVERSAL,
                CHAT_TAG_NONE, me->GetObjectGuid(), me->GetName(),
                pTarget->GetObjectGuid());
            pTarget->GetSession()->SendPacket(&data);

            // Send WHISPER_INFORM back to self so bot's chat log shows it
            WorldPacket data2;
            ChatHandler::BuildChatPacket(data2, CHAT_MSG_WHISPER_INFORM, text, LANG_UNIVERSAL,
                CHAT_TAG_NONE, me->GetObjectGuid(), me->GetName(),
                pTarget->GetObjectGuid());
            if (me->GetSession())
                me->GetSession()->SendPacket(&data2);
        }
        else
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: WHISPER target '%s' not found, falling back to SAY",
                me->GetName(), target);
            me->Say(text, LANG_UNIVERSAL);
        }
    }
    else if (chatType == 14 && strlen(channel) > 0)
    {
        // CHANNEL — send to named channel via Channel::Say
        if (ChannelMgr* cMgr = channelMgr(me->GetTeam()))
        {
            // GetJoinChannel returns existing or creates — channel should already exist
            if (Channel* chn = cMgr->GetJoinChannel(std::string(channel)))
            {
                chn->Say(me->GetObjectGuid(), text, LANG_UNIVERSAL, true);
            }
            else
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: channel '%s' not found, falling back to SAY",
                    me->GetName(), channel);
                me->Say(text, LANG_UNIVERSAL);
            }
        }
    }
    else if (chatType == 6)
    {
        me->Yell(text, LANG_UNIVERSAL);
    }
    else if (chatType == 1)
    {
        // PARTY — wire int 1 == CHAT_MSG_PARTY (0x01, VERIFIED SharedDefines.h).
        // Mirrors the whisper branch's packet-building idiom. Broadcast VERIFIED Group.h:
        //   void BroadcastPacket(WorldPacket* packet, bool ignorePlayersInBGRaid, int group=-1, ObjectGuid ignore = ObjectGuid());
        // Self receives its own line (like a real client); the C0 self-echo filter in
        // OnPacketReceived keeps it from re-entering the coordinator as a stimulus.
        if (Group* pGroup = me->GetGroup())
        {
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, text, LANG_UNIVERSAL,
                CHAT_TAG_NONE, me->GetObjectGuid(), me->GetName());
            pGroup->BroadcastPacket(&data, false);
        }
        else
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: PARTY requested but ungrouped, falling back to SAY",
                me->GetName());
            me->Say(text, LANG_UNIVERSAL);
        }
    }
    else
    {
        me->Say(text, LANG_UNIVERSAL);
    }
}

// ============================================================
// PHASE 2.5: Quest / Combat / Interaction bridge commands
// ============================================================

void AiBotAI::BridgeHandleQuestInteract(const char* json)
{
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json;

    int questId = 0, npcEntry = 0;
    char action[16] = {0};
    JsonExtractInt(payload, "quest_id", questId);
    JsonExtractInt(payload, "npc_entry", npcEntry);
    JsonExtractString(payload, "action", action, sizeof(action));

    if (questId <= 0 || npcEntry <= 0 || action[0] == '\0')
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[AIBOT-BRIDGE] %s: QUEST_INTERACT bad payload: action='%s' quest=%d npc=%d",
            me->GetName(), action, questId, npcEntry);
        return;
    }

    // ── Find the specific NPC by creature_template entry within 15 yards ──
    Creature* pNpc = nullptr;
    {
        std::list<Creature*> creatureList;
        me->GetCreatureListWithEntryInGrid(creatureList, (uint32)npcEntry, 15.0f);
        float bestDist = 999.0f;
        for (auto* pCreature : creatureList)
        {
            if (pCreature && pCreature->IsAlive())
            {
                float d = me->GetDistance(pCreature);
                if (d < bestDist)
                {
                    bestDist = d;
                    pNpc = pCreature;
                }
            }
        }
    }

    if (!pNpc)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[AIBOT-BRIDGE] %s: QUEST_INTERACT npc entry %d not found within 15yd",
            me->GetName(), npcEntry);

        char buf[128];
        snprintf(buf, sizeof(buf), "npc_not_found|quest_id=%d|npc_entry=%d", questId, npcEntry);
        BridgeSendEvent("QUEST_INTERACT_FAIL", buf);
        return;
    }

    Quest const* pQuest = sObjectMgr.GetQuestTemplate((uint32)questId);
    if (!pQuest)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[AIBOT-BRIDGE] %s: QUEST_INTERACT quest %d not found in quest_template",
            me->GetName(), questId);

        char buf[128];
        snprintf(buf, sizeof(buf), "quest_not_found|quest_id=%d", questId);
        BridgeSendEvent("QUEST_INTERACT_FAIL", buf);
        return;
    }

    // ══════════════════════════════════════════════
    //  ACCEPT
    // ══════════════════════════════════════════════
    if (strcmp(action, "accept") == 0)
    {

       // ── Idempotent accept (already-in-log) — REVISED 2026-06-30 ──────────────
        // GetQuestStatus() alone can't tell "still actively held" from "rewarded an
        // hour ago" — VMaNGOS sticks a turned-in quest's status at COMPLETE forever;
        // m_rewarded is the bit that actually means done (BridgeSendState's quest
        // blob already gates on it). Without this check, a stray re-ACCEPT for an
        // already-rewarded quest fell into the "idempotent ACK" branch below, told
        // C# the accept succeeded, and let it dispatch a brand-new objective for a
        // quest that will never again be turn-in-able — the no-op double-grind bug.
        QuestStatus existingStatus = me->GetQuestStatus((uint32)questId);
        if (existingStatus != QUEST_STATUS_NONE)
        {
            const auto& qMap = me->GetQuestStatusMap();
            auto it = qMap.find((uint32)questId);
            bool alreadyRewarded = (it != qMap.end()) && it->second.m_rewarded;

            if (alreadyRewarded)
            {
                sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                    "[AIBOT-BRIDGE] %s: accept quest %d refused — already rewarded (turned in)",
                    me->GetName(), questId);
                char buf[64];
                snprintf(buf, sizeof(buf), "already_rewarded|quest_id=%d", questId);
                BridgeSendEvent("QUEST_INTERACT_FAIL", buf);
                return;
            }

            // Genuinely still held (the case this guard was originally built for) —
            // a reward-granted chain follow-up or a re-pick that lost the Accepted
            // flag. ACK it like a normal accept so C# reconciles the batch.
            m_trackedQuestId = (uint32)questId;
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-BRIDGE] %s: accept quest %d already in log (status=%u) — idempotent ACK",
                me->GetName(), questId, (uint32)existingStatus);
            char buf[64];
            snprintf(buf, sizeof(buf), "%d", questId);
            BridgeSendEvent("QUEST_ACCEPT_ACK", buf);
            return;
        }

        if (!me->CanTakeQuest(pQuest, false))
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                "[AIBOT-BRIDGE] %s: QUEST_INTERACT accept quest %d — CanTakeQuest failed",
                me->GetName(), questId);
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                "[AIBOT-DEBUG] %s: quest %d FAILED — "
                "Status=%d ExclGrp=%d Class=%d Race=%d Level=%d Skill=%d "
                "Cond=%d Rep=%d PrevQ=%d Timed=%d NextC=%d PrevC=%d "
                "Bread=%d DepBread=%d Active=%d",
                me->GetName(), questId,
                me->SatisfyQuestStatus(pQuest, false) ? 1 : 0,
                me->SatisfyQuestExclusiveGroup(pQuest, false) ? 1 : 0,
                me->SatisfyQuestClass(pQuest, false) ? 1 : 0,
                me->SatisfyQuestRace(pQuest, false) ? 1 : 0,
                me->SatisfyQuestLevel(pQuest, false) ? 1 : 0,
                me->SatisfyQuestSkill(pQuest, false) ? 1 : 0,
                me->SatisfyQuestCondition(pQuest, false) ? 1 : 0,
                me->SatisfyQuestReputation(pQuest, false) ? 1 : 0,
                me->SatisfyQuestPreviousQuest(pQuest, false) ? 1 : 0,
                me->SatisfyQuestTimed(pQuest, false) ? 1 : 0,
                me->SatisfyQuestNextChain(pQuest, false) ? 1 : 0,
                me->SatisfyQuestPrevChain(pQuest, false) ? 1 : 0,
                me->SatisfyQuestBreadcrumbQuest(pQuest, false) ? 1 : 0,
                me->SatisfyQuestDependentBreadcrumbQuests(pQuest, false) ? 1 : 0,
                pQuest->IsActive() ? 1 : 0);

            char buf[128];
            snprintf(buf, sizeof(buf), "requirements_not_met|quest_id=%d", questId);
            BridgeSendEvent("QUEST_INTERACT_FAIL", buf);
            return;
        }

        if (!me->CanAddQuest(pQuest, true))
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                "[AIBOT-BRIDGE] %s: QUEST_INTERACT accept quest %d — CanAddQuest failed (log full?)",
                me->GetName(), questId);

            char buf[128];
            snprintf(buf, sizeof(buf), "quest_log_full|quest_id=%d", questId);
            BridgeSendEvent("QUEST_INTERACT_FAIL", buf);
            return;
        }

        me->AddQuest(pQuest, pNpc);
        m_trackedQuestId = (uint32)questId;

        // Session 27: zero-objective delivery/talk quests need explicit CompleteQuest so
        // CanRewardQuest passes at turn-in.
        if (pQuest->GetReqCreatureOrGOcount() == 0 && pQuest->GetReqItemsCount() == 0
            && me->GetQuestStatus((uint32)questId) != QUEST_STATUS_COMPLETE)
        {
            me->CompleteQuest((uint32)questId);
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-BRIDGE] %s: quest %d has no objectives — marked COMPLETE for turn-in",
                me->GetName(), questId);
        }

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: accepted quest %d '%s' from %s (entry %d)",
            me->GetName(), questId, pQuest->GetTitle().c_str(),
            pNpc->GetName(), npcEntry);

        char buf[64];
        snprintf(buf, sizeof(buf), "%d", questId);
        BridgeSendEvent("QUEST_ACCEPT_ACK", buf);
        SendQuestUpdateEvent(questId, "accepted");
    }
    // ══════════════════════════════════════════════
    //  COMPLETE (turn-in for reward)
    // ══════════════════════════════════════════════
    else if (strcmp(action, "complete") == 0)
    {
        QuestStatus status = me->GetQuestStatus((uint32)questId);
        if (status == QUEST_STATUS_NONE)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                "[AIBOT-BRIDGE] %s: QUEST_INTERACT complete quest %d — not in quest log",
                me->GetName(), questId);

            char buf[128];
            snprintf(buf, sizeof(buf), "quest_not_in_log|quest_id=%d", questId);
            BridgeSendEvent("QUEST_INTERACT_FAIL", buf);
            return;
        }

        // Pick the best CHOICE reward (gear upgrade by score, else highest vendor value).
        // Fixed rewards are granted regardless; this index only selects among "pick one" items.
        uint32 rewardChoice = ChooseQuestReward(pQuest);

        if (!me->CanRewardQuest(pQuest, rewardChoice, false))
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                "[AIBOT-BRIDGE] %s: QUEST_INTERACT complete quest %d — CanRewardQuest failed (status=%u choice=%u)",
                me->GetName(), questId, (uint32)status, rewardChoice);

            char buf[128];
            snprintf(buf, sizeof(buf), "cannot_reward|quest_id=%d", questId);
            BridgeSendEvent("QUEST_INTERACT_FAIL", buf);
            return;
        }

        // RewardQuest: canonical reward path (XP, money, rep, items, spell cast). The chosen
        // index now reflects ScoreItem, not a blind 0.
        me->RewardQuest(pQuest, rewardChoice, pNpc, true);
        m_trackedQuestId = 0;
        TryAutoEquipBags();
        TryAutoEquip();

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: completed quest %d '%s' at %s (entry %d, rewardChoice=%u)",
            me->GetName(), questId, pQuest->GetTitle().c_str(),
            pNpc->GetName(), npcEntry, rewardChoice);

        char buf[64];
        snprintf(buf, sizeof(buf), "%d", questId);
        BridgeSendEvent("QUEST_COMPLETE_ACK", buf);
        SendQuestUpdateEvent(questId, "rewarded");
    }
    else
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[AIBOT-BRIDGE] %s: QUEST_INTERACT unknown action '%s'",
            me->GetName(), action);
    }
}

void AiBotAI::BridgeHandleAbandonQuest(const char* json)
{
    int questId = 0;
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json;
    JsonExtractInt(payload, "quest_id", questId);

    if (questId <= 0)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: ABANDON_QUEST missing quest_id", me->GetName());
        return;
    }

    QuestStatus status = me->GetQuestStatus(questId);
    if (status == QUEST_STATUS_NONE)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: ABANDON_QUEST quest %d not in log", me->GetName(), questId);
        return;
    }

    me->SetQuestStatus(questId, QUEST_STATUS_NONE);
    if (m_trackedQuestId == (uint32)questId)  
        m_trackedQuestId = 0;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: abandoned quest %d", me->GetName(), questId);
    SendQuestUpdateEvent(questId, "abandoned");
}

void AiBotAI::BridgeHandleLearnSpell(const char* json)
{
    int spellId = 0;
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json;
    JsonExtractInt(payload, "spell_id", spellId);

    if (spellId <= 0)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: LEARN_SPELL missing spell_id", me->GetName());
        return;
    }

    if (me->HasSpell(spellId))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: LEARN_SPELL already knows %d", me->GetName(), spellId);
        return;
    }

    me->LearnSpell(spellId, false);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: learned spell %d", me->GetName(), spellId);
}

void AiBotAI::BridgeHandleTrain(const char* json)
{
    int npcEntry = 0;
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json;
    JsonExtractInt(payload, "npc_entry", npcEntry);

    if (npcEntry <= 0)
    {
        BridgeSendEvent("TRAIN_FAIL", "reason=missing_npc_entry");
        return;
    }

    // Stop movement so we don't walk away between search and train
    StopMoving();

    // ── Diagnostic: search at 15yd first, then wider if not found ──
    std::list<Creature*> creatureList;
    me->GetCreatureListWithEntryInGrid(creatureList, (uint32)npcEntry, 15.0f);

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-TRAIN] %s: searching for trainer entry %d — found %zu creatures within 15yd, bot at (%.1f, %.1f, %.1f)",
        me->GetName(), npcEntry, creatureList.size(),
        me->GetPositionX(), me->GetPositionY(), me->GetPositionZ());

    // Log every creature found (even if it fails the trainer check)
    for (auto* c : creatureList)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-TRAIN] %s:   candidate: '%s' entry=%u guid=%u alive=%d npc_flags=0x%X dist=%.1f (%.1f,%.1f,%.1f)",
            me->GetName(), c->GetName(), c->GetEntry(), c->GetGUIDLow(),
            c->IsAlive() ? 1 : 0,
            c->GetUInt32Value(UNIT_NPC_FLAGS),
            me->GetDistance(c),
            c->GetPositionX(), c->GetPositionY(), c->GetPositionZ());
    }

    Creature* pTrainer = nullptr;
    float bestDist = 999.0f;
    for (auto* c : creatureList)
    {
        if (c && c->IsAlive() && (c->GetUInt32Value(UNIT_NPC_FLAGS) & UNIT_NPC_FLAG_TRAINER))
        {
            float d = me->GetDistance(c);
            if (d < bestDist)
            {
                bestDist = d;
                pTrainer = c;
            }
        }
    }

    // ── Fallback: wider search if narrow failed ──
    if (!pTrainer)
    {
        std::list<Creature*> wideList;
        me->GetCreatureListWithEntryInGrid(wideList, (uint32)npcEntry, 50.0f);

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-TRAIN] %s: 15yd search failed — wide search (50yd) found %zu creatures",
            me->GetName(), wideList.size());

        for (auto* c : wideList)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-TRAIN] %s:   wide: '%s' entry=%u alive=%d flags=0x%X dist3d=%.1f pos=(%.1f,%.1f,%.1f)",
                me->GetName(), c->GetName(), c->GetEntry(), c->GetGUIDLow(),
                c->IsAlive() ? 1 : 0,
                c->GetUInt32Value(UNIT_NPC_FLAGS),
                me->GetDistance(c),
                c->GetPositionX(), c->GetPositionY(), c->GetPositionZ());

            if (c && c->IsAlive() && (c->GetUInt32Value(UNIT_NPC_FLAGS) & UNIT_NPC_FLAG_TRAINER))
            {
                pTrainer = c;
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-TRAIN] %s: found trainer in wide search at dist=%.1f — using it",
                    me->GetName(), me->GetDistance(c));
                break;
            }
        }
    }

    if (!pTrainer)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-TRAIN] %s: trainer entry %d not found within 50yd",
            me->GetName(), npcEntry);
        BridgeSendEvent("TRAIN_FAIL", "reason=trainer_not_found");
        return;
    }

    if (!pTrainer->IsTrainerOf(me, false))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-TRAIN] %s: NPC %d ('%s') is not a trainer for this class",
            me->GetName(), npcEntry, pTrainer->GetName());
        BridgeSendEvent("TRAIN_FAIL", "reason=wrong_class");
        return;
    }

    TrainerSpellData const* cSpells = pTrainer->GetTrainerSpells();
    TrainerSpellData const* tSpells = pTrainer->GetTrainerTemplateSpells();

    if (!cSpells && !tSpells)
    {
        BridgeSendEvent("TRAIN_FAIL", "reason=no_spells");
        return;
    }

    int totalLearned = 0;
    uint32 totalCost = 0;
    bool learnedAnything;

    do
    {
        learnedAnything = false;

        auto processSpellList = [&](TrainerSpellData const* spells)
        {
            if (!spells) return;
            for (auto const& itr : spells->spellList)
            {
                TrainerSpell const* tSpell = &itr.second;
                if (me->GetTrainerSpellState(tSpell) != TRAINER_SPELL_GREEN)
                    continue;

                SpellEntry const* spellEntry = sSpellMgr.GetSpellEntry(tSpell->spell);
                if (!spellEntry) continue;

                uint32 triggerSpell = spellEntry->EffectTriggerSpell[0];
                if (!triggerSpell) continue;

                if (sSpellMgr.IsPrimaryProfessionFirstRankSpell(triggerSpell))
                    continue;

                if (!me->IsSpellFitByClassAndRace(triggerSpell))
                    continue;

                uint32 spellCost = tSpell->spellCost;
                if (me->GetMoney() < spellCost)
                    continue;

                me->ModifyMoney(-(int32)spellCost);
                me->LearnSpell(triggerSpell, false);

                totalCost += spellCost;
                totalLearned++;
                learnedAnything = true;

                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-TRAIN] %s: learned spell %u (cost=%u copper)",
                    me->GetName(), triggerSpell, spellCost);
            }
        };

        processSpellList(cSpells);
        processSpellList(tSpells);

    } while (learnedAnything);

    if (totalLearned > 0)
    {
        ResetSpellData();
        PopulateSpellData();
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "learned=%d|cost=%u|gold=%u",
        totalLearned, totalCost, me->GetMoney());

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-TRAIN] %s: complete — %d spells, %u copper spent, %u remaining",
        me->GetName(), totalLearned, totalCost, me->GetMoney());

    BridgeSendEvent("TRAIN_ACK", buf);
}

// ============================================================
// SESSION 29: QUERY_QUEST_STATUS bridge command
//
// C# sends QUERY_QUEST_STATUS when entering Questing domain.
// C++ responds with QUEST_STATUS_ALL event containing every
// active quest in the player's log with status + progress.
//
// This is the authoritative source — straight from the C++
// QuestStatusMap in memory. No DB timing gaps.
//
// PLACEMENT: Add this method after BridgeHandleTrain in AiBotAI.cpp
// DISPATCH:  Add to BridgeProcessLine (see bottom of this file)
// ============================================================

void AiBotAI::BridgeHandleQueryQuestStatus(const char* json)
{
    // Iterate the player's quest status map — same map used by
    // BridgeHandleSellItems for quest item protection.
    const auto& questMap = me->GetQuestStatusMap();

    // Build a compact pipe-delimited payload:
    //   questId:status:mob1,mob2,mob3,mob4:item1,item2,item3,item4|questId:...
    //
    // status: 1=INCOMPLETE, 3=COMPLETE (VMaNGOS QUEST_STATUS enum)
    // Only include non-rewarded quests (active log entries).

    std::string payload;
    int count = 0;

    for (const auto& pair : questMap)
    {
        uint32 questId = pair.first;
        const auto& qData = pair.second;

        // Skip rewarded (turned-in) quests — we only want active log entries
        if (qData.m_rewarded)
            continue;

        // Skip QUEST_STATUS_NONE (0) and QUEST_STATUS_UNAVAILABLE (2)
        if (qData.m_status != QUEST_STATUS_INCOMPLETE &&
            qData.m_status != QUEST_STATUS_COMPLETE)
            continue;

        if (!payload.empty())
            payload += "|";

        char entry[128];
        snprintf(entry, sizeof(entry), "%u:%u:%u,%u,%u,%u:%u,%u,%u,%u",
            questId,
            (uint32)qData.m_status,
            qData.m_creatureOrGOcount[0], qData.m_creatureOrGOcount[1],
            qData.m_creatureOrGOcount[2], qData.m_creatureOrGOcount[3],
            qData.m_itemcount[0], qData.m_itemcount[1],
            qData.m_itemcount[2], qData.m_itemcount[3]);

        payload += entry;
        count++;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-BRIDGE] %s: QUERY_QUEST_STATUS — %d active quests in log",
        me->GetName(), count);

    BridgeSendEvent("QUEST_STATUS_ALL", payload.c_str());
}


void AiBotAI::BridgeHandleAttackTarget(const char* json)
{
    int guidLow = 0;
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json;
    JsonExtractInt(payload, "guid", guidLow);

    if (guidLow <= 0)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: ATTACK_TARGET missing guid", me->GetName());
        return;
    }

    // A creature ObjectGuid here is (HIGHGUID_UNIT, entry, counter) and Map::GetCreature
    // matches on the whole thing — the two-argument form below leaves entry 0, so it never
    // resolved anything and the "fallback" the old comment promised was never written. Every
    // caller that gets this right (the kill/victim paths in AiBotAIMain) passes the entry, so
    // senders now do too; the entry-less attempt is kept for any producer still omitting it.
    int entry = 0;
    JsonExtractInt(payload, "entry", entry);

    Creature* pCreature = entry > 0
        ? me->GetMap()->GetCreature(ObjectGuid(HIGHGUID_UNIT, uint32(entry), uint32(guidLow)))
        : me->GetMap()->GetCreature(ObjectGuid(HIGHGUID_UNIT, uint32(guidLow)));

    if (!pCreature)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: ATTACK_TARGET creature guid %d (entry %d) not found on map", me->GetName(), guidLow, entry);
        return;
    }

    if (!IsValidHostileTarget(pCreature))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: ATTACK_TARGET guid %d not valid hostile target", me->GetName(), guidLow);
        return;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: attacking %s (guid %d)",
        me->GetName(), pCreature->GetName(), guidLow);
    AttackStart(pCreature);
}

void AiBotAI::BridgeHandleInteractNpc(const char* json)
{
    int guidLow = 0;
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json;
    JsonExtractInt(payload, "guid", guidLow);

    if (guidLow <= 0)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: INTERACT_NPC missing guid", me->GetName());
        return;
    }

    Creature* pCreature = me->GetMap()->GetCreature(
        ObjectGuid(HIGHGUID_UNIT, uint32(guidLow)));

    if (!pCreature)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: INTERACT_NPC creature guid %d not found", me->GetName(), guidLow);
        return;
    }

    float dist = me->GetDistance(pCreature);
    if (dist > 10.0f)
    {
        // Too far — move closer first, then interact on arrival
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: INTERACT_NPC moving to %s (dist=%.1f)",
            me->GetName(), pCreature->GetName(), dist);
        StopMoving();
        float nx, ny, nz;
        pCreature->GetContactPoint(me, nx, ny, nz);
        MovePointRun(AIBOT_POINT_TASK_DEST, nx, ny, nz);
        return;
    }

    // Face the NPC
    me->SetFacingToObject(pCreature);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: interacting with %s (guid %d)",
        me->GetName(), pCreature->GetName(), guidLow);
    BridgeSendEvent("NPC_INTERACT", pCreature->GetName());
}

void AiBotAI::BridgeHandleSetTask(const char* json)
{
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json;

    char taskType[32] = {0};
    JsonExtractString(payload, "task", taskType, sizeof(taskType));

    if (strcmp(taskType, "GRIND") == 0)
    {
        m_currentTask.Clear();
        m_currentTask.type = TASK_GRIND;
        JsonExtractFloat(payload, "x", m_currentTask.x);
        JsonExtractFloat(payload, "y", m_currentTask.y);
        JsonExtractFloat(payload, "z", m_currentTask.z);
        JsonExtractFloat(payload, "radius", m_currentTask.radius);

        int entry = 0, goal = 0;
        JsonExtractInt(payload, "creature_entry", entry);
        JsonExtractInt(payload, "kill_count", goal);
        m_currentTask.creatureEntry = (uint32)entry;
        m_currentTask.killGoal = goal;
        m_currentTask.killCount = 0;

        if (m_currentTask.radius < 10.0f)
            m_currentTask.radius = 40.0f;  // sane default

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: SET_TASK GRIND entry=%u goal=%d at (%.1f,%.1f,%.1f) r=%.0f",
            me->GetName(), m_currentTask.creatureEntry,
            m_currentTask.killGoal,
            m_currentTask.x, m_currentTask.y, m_currentTask.z,
            m_currentTask.radius);

        // Immediately move to grind area if not already there
        float dist = me->GetDistance2d(m_currentTask.x, m_currentTask.y);
        if (dist > m_currentTask.radius)
        {
            StopMoving();
            MovePointRun(AIBOT_POINT_GRIND_PATROL,
                m_currentTask.x, m_currentTask.y, m_currentTask.z);
        }
    }
    else if (strcmp(taskType, "IDLE") == 0)
    {
        m_currentTask.Clear();
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: SET_TASK IDLE (clearing task)", me->GetName());
    }
    else if (strcmp(taskType, "PORT_HOME") == 0)
    {
        // [ESCAPE] (FINDING_010) Living-bot stranded escape. The C# wedge-streak escalation asks
        // for a port to the racial start when a bot has proven it can neither kill nor quest where
        // it stands (Everlook L18 / Badlands L21 census — the park→local-relocate ladder shuffles
        // it ~50yd forever). The DEAD path is FINDING_008's ghost port; this one is alive-only and
        // refuses in combat (threat/aggro transfer). Same-map uses the proven NearTeleportTo seam;
        // cross-continent uses the full Player::TeleportTo far-teleport (bots are real in-core
        // players, same machinery as any player worldport).
        float hx = 0.0f, hy = 0.0f, hz = 0.0f; int hmap = -1;
        bool haveHome = JsonExtractFloat(payload, "home_x", hx) && JsonExtractFloat(payload, "home_y", hy) &&
                        JsonExtractFloat(payload, "home_z", hz) && JsonExtractInt(payload, "home_map", hmap);
        if (haveHome && hmap >= 0 && me->IsAlive() && !me->IsInCombat())
        {
            float fx = me->GetPositionX(), fy = me->GetPositionY(), fz = me->GetPositionZ();
            uint32 fmap = me->GetMapId();
            m_currentTask.Clear();
            StopMoving();
            if ((int)me->GetMapId() == hmap)
            {
                ReGroundZ(hx, hy, hz, "port-home");
                me->NearTeleportTo(hx, hy, hz, me->GetOrientation());
            }
            else
                me->TeleportTo((uint32)hmap, hx, hy, hz, me->GetOrientation());
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT] %s PORT_HOME (stranded escape): (%.1f, %.1f, %.1f) map=%u -> (%.1f, %.1f, %.1f) map=%d",
                me->GetName(), fx, fy, fz, fmap, hx, hy, hz, hmap);
        }
        else
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT] %s PORT_HOME refused (home=%d map=%d alive=%d combat=%d)",
                me->GetName(), haveHome ? 1 : 0, hmap, me->IsAlive() ? 1 : 0, me->IsInCombat() ? 1 : 0);
    }
    else
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: SET_TASK unknown task type '%s'", me->GetName(), taskType);
    }
}

// ============================================================
// BridgeHandleCombatDirective — the per-member combat stamp (group focus-fire seam)
//
// The god-bot coordinator (C#) stamps each grouped member a combat directive each tick.
// v1 carries ONE mode — assist — plus the anchor's low GUID:
//   {"type":"COMBAT_DIRECTIVE","payload":{"mode":"assist","anchor_guid":123}}
//   • mode=="assist" + anchor_guid>0 → focus-fire the anchor's live victim (resolved in
//     TeamPlay::ResolveCombatTarget; the anchor itself — anchor_guid==self — falls through
//     to normal selection, so the team assists IT).
//   • mode=="none" / absent / anchor_guid<=0 → clear (revert to solo selection).
//
// FORWARD-TOLERANT BY CONSTRUCTION: parsed with JsonExtract*, the same idiom as every other
// inbound command in this file. A later key (role / interrupt_guid / move_to_guid) from a
// newer C# brain is simply not looked up here — a new key, never a contract break.
//
// No ack — a fire-and-forget stamp (like SET_TASK). Liveness IS the re-stamp cadence
// (the coordinator re-stamps every brain tick, idempotent; ungroup/anchor-death → mode=none).
// ============================================================
void AiBotAI::BridgeHandleCombatDirective(const char* json)
{
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json;

    char mode[16] = {0};
    JsonExtractString(payload, "mode", mode, sizeof(mode));

    int anchorGuid = 0;
    JsonExtractInt(payload, "anchor_guid", anchorGuid);

    if (strcmp(mode, "assist") == 0 && anchorGuid > 0)
    {
        m_combatDirective.mode          = COMBAT_MODE_ASSIST;
        m_combatDirective.anchorGuidLow = (uint32)anchorGuid;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-TEAMPLAY] %s: COMBAT_DIRECTIVE mode=assist anchor=%u%s",
            me->GetName(), m_combatDirective.anchorGuidLow,
            ((uint32)anchorGuid == me->GetGUIDLow()) ? " (self — I am the anchor)" : "");
    }
    else
    {
        m_combatDirective.Clear();
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-TEAMPLAY] %s: COMBAT_DIRECTIVE cleared (mode='%s')",
            me->GetName(), mode[0] ? mode : "none");
    }
}

void AiBotAI::BridgeHandleTakeFlight(const char* json)
{
    int sourceNode = 0, destNode = 0;
 
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json;
 
    JsonExtractInt(payload, "sourceNode", sourceNode);
    JsonExtractInt(payload, "destNode", destNode);
 
    if (sourceNode <= 0 || destNode <= 0)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: TAKE_FLIGHT missing sourceNode or destNode", me->GetName());
        BridgeSendEvent("FLIGHT_FAILED", "missing sourceNode or destNode");
        return;
    }
 
    if (me->IsDead())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,    
            "[AIBOT-BRIDGE] %s: TAKE_FLIGHT rejected — bot is dead", me->GetName());
        BridgeSendEvent("FLIGHT_FAILED", "bot is dead");
        return;
    }
 
    if (me->IsInCombat())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: TAKE_FLIGHT rejected — bot is in combat", me->GetName());
        BridgeSendEvent("FLIGHT_FAILED", "bot is in combat");
        return;
    }
 
    // Validate source node exists
    TaxiNodesEntry const* srcNode = sObjectMgr.GetTaxiNodeEntry((uint32)sourceNode);
    if (!srcNode)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: TAKE_FLIGHT invalid sourceNode %d", me->GetName(), sourceNode);
        BridgeSendEvent("FLIGHT_FAILED", "invalid sourceNode");
        return;
    }
 
    // Validate destination node exists
    TaxiNodesEntry const* dstNode = sObjectMgr.GetTaxiNodeEntry((uint32)destNode);
    if (!dstNode)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: TAKE_FLIGHT invalid destNode %d", me->GetName(), destNode);
        BridgeSendEvent("FLIGHT_FAILED", "invalid destNode");
        return;
    }
 
    // Validate a path exists between source and dest
    uint32 pathId = 0, pathCost = 0;
    sObjectMgr.GetTaxiPath((uint32)sourceNode, (uint32)destNode, pathId, pathCost);
    if (!pathId)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: TAKE_FLIGHT no path from %d to %d", me->GetName(), sourceNode, destNode);
        BridgeSendEvent("FLIGHT_FAILED", "no path between nodes");
        return;
    }
 
    // Ensure bot knows both taxi nodes (unlock them)
    me->GetTaxi().SetTaximaskNode((uint32)sourceNode);
    me->GetTaxi().SetTaximaskNode((uint32)destNode);
 
    // Check if bot can afford the flight
    if (me->GetMoney() < pathCost)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: TAKE_FLIGHT not enough money (have %u, need %u copper)",
            me->GetName(), me->GetMoney(), pathCost);
 
        char failJson[256];
        snprintf(failJson, sizeof(failJson),
            "{\"type\":\"EVENT\",\"payload\":{"
            "\"guid\":%u,\"event\":\"FLIGHT_FAILED\","
            "\"reason\":\"not_enough_money\","
            "\"have\":%u,\"need\":%u,\"cost\":%u}}",
            me->GetGUIDLow(), me->GetMoney(), pathCost, pathCost);
        BridgeSend(failJson);
        return;
    }
 
    // Must be on same map as source node
    if (srcNode->map_id != me->GetMapId())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: TAKE_FLIGHT source node %d is on map %u, bot is on map %u",
            me->GetName(), sourceNode, srcNode->map_id, me->GetMapId());
        BridgeSendEvent("FLIGHT_FAILED", "source node on different map");
        return;
    }
 
    // Stop any current movement/combat
    StopMoving();
    if (me->IsMounted())
        me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
 
 
    // Set task state so UpdateAI doesn't interfere during flight
    m_currentTask.Clear();
    m_currentTask.type = TASK_TAXI;
    m_currentTask.taxiSourceNode = (uint32)sourceNode;
    m_currentTask.taxiDestNode = (uint32)destNode;
 
    // Build the node vector and activate the flight path
    // nocheck = true skips the "do you know this node" validation
    std::vector<uint32> nodes;
    nodes.push_back((uint32)sourceNode);
    nodes.push_back((uint32)destNode);
 
    bool success = me->ActivateTaxiPathTo(nodes, nullptr, 0, true);
 
    if (success)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: TAKE_FLIGHT activated path %u → %u (cost %u copper)",
            me->GetName(), sourceNode, destNode, pathCost);
        BridgeSendEvent("FLIGHT_STARTED", "");
    }
    else
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: TAKE_FLIGHT ActivateTaxiPathTo failed (%d → %d)",
            me->GetName(), sourceNode, destNode);
        m_currentTask.Clear();
        BridgeSendEvent("FLIGHT_FAILED", "ActivateTaxiPathTo returned false");
    }
}

// ============================================================
// BridgeHandleSellItems — v4 (2026-07-28: consumable policy — food dumps, weak pots sell)
//
// CHANGE from v3:
//   The v2/v3 "keep up to MAX_KEEP_PER_CONSUMABLE(40) of each item-id" rule kept every
//   distinct food/drink/potion stack, so a bag with 30 different half-stacks freed nothing.
//   Replaced with a role split:
//     - FOOD & DRINK (subclass FOOD): the bot has unlimited food (autonomous DrinkAndEat
//       conjures/uses its own), so looted food is never needed → SELL ALL, any level.
//     - POTIONS / elixirs / scrolls / bandages: not consumed by the bot YET, but keep the
//       level-appropriate ones staged for when potion-use lands. Sell only ranks the bot has
//       outgrown — RequiredLevel more than AIBOT_CONSUMABLE_STALE_LEVELS below current level.
//
// CHANGES from v2/v3 (retained): gear sold on UPGRADE status not rarity (v3); non-upgrade bags
// vendorable; quest/consumable protections; "nothing_to_sell" flag in SELL_ACK.
//
// REQUIRES: AIBOT_SELL_KEEP_UPGRADE_LEVELS 5, AIBOT_CONSUMABLE_STALE_LEVELS 12  in AiBotAIMain.h
// ============================================================

void AiBotAI::BridgeHandleSellItems(const char* json)
{
    if (!me || !me->IsAlive() || !me->IsInWorld())
        return;

    // [SUI] Never autosell a REAL account's character. AiBotAI is only ever
    // attached to socket-less bot sessions, and possessed bots reject bridge
    // commands upstream — but this is the requirement's hard wall: any unit
    // whose session has a live client keeps its inventory untouchable.
    if (me->GetSession() && !me->GetSession()->GetBot())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-SELL] %s: refused — real account character", me->GetName());
        BridgeSendEvent("SELL_FAIL", "reason=real_account_protected");
        return;
    }

    int npcEntry = 0, keepQuality = 0;
    JsonExtractInt(json, "npc_entry", npcEntry);
    JsonExtractInt(json, "keep_quality", keepQuality);
    if (npcEntry <= 0)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-SELL] %s: SELL_ITEMS missing npc_entry", me->GetName());
        BridgeSendEvent("SELL_FAIL", "reason=missing_npc_entry");
        return;
    }
    if (keepQuality <= 0) keepQuality = 2;

    // --- Find vendor NPC ---
    std::list<Creature*> creatureList;
    me->GetCreatureListWithEntryInGrid(creatureList, (uint32)npcEntry, 15.0f);

    Creature* pVendor = nullptr;
    float bestDist = 999.0f;
    for (Creature* c : creatureList)
    {
        if (c && c->IsAlive() && (c->GetUInt32Value(UNIT_NPC_FLAGS) & UNIT_NPC_FLAG_VENDOR))
        {
            float d = me->GetDistance(c);
            if (d < bestDist)
            {
                bestDist = d;
                pVendor = c;
            }
        }
    }
    if (!pVendor)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-SELL] %s: no vendor with entry %d within 15yd", me->GetName(), npcEntry);
        BridgeSendEvent("SELL_FAIL", "reason=vendor_not_found");
        return;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-SELL] %s: === BEGIN selling at %s (entry=%u) keepQuality=%d ===",
        me->GetName(), pVendor->GetName(), pVendor->GetEntry(), keepQuality);

    // --- Build set of quest-required item IDs ---
    std::set<uint32> questItemIds;
    const auto& questMap = me->GetQuestStatusMap();
    for (const auto& pair : questMap)
    {
        if (pair.second.m_status != QUEST_STATUS_INCOMPLETE && pair.second.m_status != QUEST_STATUS_COMPLETE)
            continue;
        Quest const* pQuest = sObjectMgr.GetQuestTemplate(pair.first);
        if (!pQuest) continue;
        for (int j = 0; j < QUEST_OBJECTIVES_COUNT; ++j)
        {
            if (pQuest->ReqItemId[j] > 0)
                questItemIds.insert(pQuest->ReqItemId[j]);
        }
        if (pQuest->GetSrcItemId() > 0)
            questItemIds.insert(pQuest->GetSrcItemId());
    }

    // Find the largest equipped bag size (for bag-selling: only sell bags
    // that are NOT bigger than any equipped bag, i.e. not an upgrade)
    uint32 largestEquippedBagSize = 0;
    for (int b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
    {
        Item* pBagItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, (uint8)b);
        if (!pBagItem) continue;
        ItemPrototype const* bp = pBagItem->GetProto();
        if (bp && bp->Class == ITEM_CLASS_CONTAINER && bp->SubClass == ITEM_SUBCLASS_CONTAINER)
        {
            if (bp->ContainerSlots > largestEquippedBagSize)
                largestEquippedBagSize = bp->ContainerSlots;
        }
    }
    // Also check if there are any empty bag equip slots (if so, ANY bag is an upgrade)
    bool hasEmptyBagSlot = false;
    for (int b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
    {
        if (!me->GetItemByPos(INVENTORY_SLOT_BAG_0, (uint8)b))
        {
            hasEmptyBagSlot = true;
            break;
        }
    }

    uint32 totalCopper = 0;
    uint32 soldCount = 0;

    // --- Vendor surplus quest-gather items (over-loot cleanup) ---
    for (uint32 qItemId : questItemIds)
    {
        uint32 need = QuestRequiredCountFor(qItemId);
        if (need == 0)
            continue;
        uint32 have = me->GetItemCount(qItemId, false);
        if (have <= need)
            continue;
        uint32 surplus = have - need;
        ItemPrototype const* sp = sObjectMgr.GetItemPrototype(qItemId);
        uint32 money = (sp ? sp->SellPrice : 0) * surplus;
        if (money)
        {
            me->ModifyMoney((int32)money);
            totalCopper += money;
        }
        me->DestroyItemCount(qItemId, surplus, true);
        soldCount += surplus;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-SELL] %s:   quest-surplus [%s] (id=%u) kept %u, sold %u for %uc",
            me->GetName(), sp && sp->Name1 ? sp->Name1 : "?", qItemId, need, surplus, money);
    }

    // --- Sell helper: checks one item, sells if appropriate ---
    auto trySellItem = [&](uint8 bag, uint8 slot)
    {
        Item* pItem = me->GetItemByPos(bag, slot);
        if (!pItem) return;

        ItemPrototype const* proto = pItem->GetProto();
        if (!proto) return;

        // Keep: no sell price (hearthstone, etc.)
        if (proto->SellPrice == 0) return;
        // Keep: quest-class items
        if (proto->Class == ITEM_CLASS_QUEST) return;
        if (proto->StartQuest > 0) return;
        if (proto->Bonding == BIND_QUEST_ITEM || proto->Bonding == BIND_QUEST_ITEM1) return;
        if (questItemIds.count(proto->ItemId) > 0) return;

        // --- BAGS: sell unequipped bags that aren't upgrades ---
        if (proto->Class == ITEM_CLASS_CONTAINER || proto->Class == ITEM_CLASS_QUIVER)
        {
            if (hasEmptyBagSlot) return;
            if (proto->ContainerSlots > largestEquippedBagSize) return;
            // else: duplicate/downgrade bag → sell
        }
        // --- CONSUMABLES: food dumps, weak potions sell, current-rank potions kept (v4) ---
        else if (proto->Class == ITEM_CLASS_CONSUMABLE)
        {
            // Food & drink (subclass FOOD): the bot has unlimited food (autonomous DrinkAndEat),
            // so looted food is never needed → sell all, any level. (§verify subclass FOOD == 5.)
            if (proto->SubClass == 5)   // 5 = Food & Drink (1.12 consumable subclass DBC id; this fork defines no ITEM_SUBCLASS_FOOD name)
            {
                // fall through to sell
            }
            else
            {
                // Potions / elixirs / scrolls / bandages — not consumed by the bot YET. Keep the
                // level-appropriate ones staged for when potion-use lands; sell only ranks the bot
                // has outgrown. This is the "don't sell EVERYTHING" guard — the current tier stays.
                if (me->GetLevel() <= proto->RequiredLevel + AIBOT_CONSUMABLE_STALE_LEVELS)
                    return;   // level-appropriate → keep
                // else: superseded rank → sell (fall through)
            }
        }
        // --- GEAR (weapons/armor): UPGRADE-AWARE, quality-blind (v3) ---
        else if (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR)
        {
            // Keep 1: epics+ — rare, high value, never vendor-trash a purple.
            if (proto->Quality >= (uint32)ITEM_QUALITY_EPIC) return;

            uint16 dest = 0;
            InventoryResult can = me->CanEquipItem(NULL_SLOT, dest, pItem, true);
            if (can == EQUIP_ERR_OK)
            {
                // Usable now — sell UNLESS it out-scores what's worn (safety net for a real upgrade
                // that slipped past TryAutoEquip when bags were full).
                uint8 tgt = (uint8)(dest & 0xFF);
                Item* worn = me->GetItemByPos(INVENTORY_SLOT_BAG_0, tgt);
                float newS = ScoreItem(proto, tgt);
                float oldS = worn ? ScoreItem(worn->GetProto(), tgt) : 0.0f;
                if (newS > oldS) return;   // Keep 2: genuine upgrade
                // else: usable non-upgrade → sell
            }
            else
            {
                // Keep 3: grow-into upgrade — right class/proficiency, under-level, usable within
                // AIBOT_SELL_KEEP_UPGRADE_LEVELS. (RequiredLevel — verify the field name on this core.)
                if (proto->RequiredLevel > me->GetLevel() &&
                    proto->RequiredLevel <= me->GetLevel() + AIBOT_SELL_KEEP_UPGRADE_LEVELS)
                    return;
                // else: never-usable (wrong class/prof/race) or too far off → sell
            }
        }
        // --- MISC / trade goods / recipes: keep the quality threshold (conservative — see note) ---
        // NOTE (2026-07-28): recipes (ITEM_CLASS_RECIPE), trade goods (ITEM_CLASS_TRADE_GOODS) and
        // unopenable lockboxes still fall here and are kept by rarity. A combat grind bot never
        // crafts/enchants, so these are pure fodder — but they're also exactly what an AH would list
        // for real value, so the sell-vs-hold call is deferred to the AH-value model (see
        // SuperUiBots_ARCHITECTURE, the AH warning). Flip to sell-by-default here when that lands (or
        // sooner, if slots beat the lost AH value).
        else
        {
            if (proto->Quality >= (uint32)keepQuality) return;
        }

        uint32 money = proto->SellPrice * pItem->GetCount();

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-SELL] %s:   selling [%s] (id=%u q=%u x%u) for %uc",
            me->GetName(), proto->Name1 ? proto->Name1 : "?",
            proto->ItemId, proto->Quality, pItem->GetCount(), money);

        // DestroyItem handles RemoveItem + RemoveFromUpdateQueueOf + SetState
        // in the correct order. Must grab money BEFORE destroy invalidates pItem.
        me->ModifyMoney((int32)money);
        me->DestroyItem(bag, slot, true);

        totalCopper += money;
        soldCount++;
    };

    // --- Iterate backpack ---
    for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        trySellItem(INVENTORY_SLOT_BAG_0, (uint8)i);

    // --- Iterate extra bags ---
    for (int b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
    {
        Bag* pBag = (Bag*)me->GetItemByPos(INVENTORY_SLOT_BAG_0, (uint8)b);
        if (!pBag || pBag->GetProto()->Class != ITEM_CLASS_CONTAINER)
            continue;
        for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
            trySellItem((uint8)b, (uint8)j);
    }

    uint32 freeSlots = 0;
    for (int fi = INVENTORY_SLOT_ITEM_START; fi < INVENTORY_SLOT_ITEM_END; ++fi)
        if (!me->GetItemByPos(INVENTORY_SLOT_BAG_0, fi))
            ++freeSlots;
    for (int fi = INVENTORY_SLOT_BAG_START; fi < INVENTORY_SLOT_BAG_END; ++fi)
        if (Bag* pFBag = (Bag*)me->GetItemByPos(INVENTORY_SLOT_BAG_0, fi))
            if (pFBag->GetProto()->Class == ITEM_CLASS_CONTAINER && pFBag->GetProto()->SubClass == ITEM_SUBCLASS_CONTAINER)
                for (uint32 fj = 0; fj < pFBag->GetBagSize(); ++fj)
                    if (!me->GetItemByPos(fi, fj))
                        ++freeSlots;

    // Build SELL_ACK — include "nothing_to_sell" flag when bags are full but
    // nothing was vendorable, so C# can set a cooldown and break the loop.
    char eventData[196];
    if (soldCount == 0 && freeSlots == 0)
    {
        snprintf(eventData, sizeof(eventData),
            "sold=0|copper_earned=0|free_slots=0|copper_total=%u|nothing_to_sell=1",
            me->GetMoney());
    }
    else
    {
        snprintf(eventData, sizeof(eventData),
            "sold=%u|copper_earned=%u|free_slots=%u|copper_total=%u",
            soldCount, totalCopper, freeSlots, me->GetMoney());
    }

    BridgeSendEvent("SELL_ACK", eventData);

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-SELL] %s: === DONE === sold %u items for %uc, %u free slots",
        me->GetName(), soldCount, totalCopper, freeSlots);
}

// ============================================================
// BridgeHandleRepairItems — Session 32
//
// C# sends REPAIR_AT_NPC after SELL_ACK when the vendor has
// UNIT_NPC_FLAG_REPAIR. Repairs all equipped gear + bags.
//
// Pattern mirrors BridgeHandleSellItems: find NPC by entry
// within 15yd, validate flag, do work, emit ACK/FAIL.
//
// DISPATCH:  Add to BridgeProcessLine:
//              else if (strcmp(msgType, "REPAIR_AT_NPC") == 0)
//                  BridgeHandleRepairItems(line);
// HEADER:    Add to AiBotAI.h:
//              void BridgeHandleRepairItems(const char* json);
// ============================================================
 
void AiBotAI::BridgeHandleRepairItems(const char* json)
{
    if (!me || !me->IsAlive() || !me->IsInWorld())
        return;
 
    int npcEntry = 0;
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json;
    JsonExtractInt(payload, "npc_entry", npcEntry);
 
    if (npcEntry <= 0)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-REPAIR] %s: REPAIR_AT_NPC missing npc_entry", me->GetName());
        BridgeSendEvent("REPAIR_FAIL", "reason=missing_npc_entry");
        return;
    }
 
    // --- Find repair NPC within 15yd ---
    std::list<Creature*> creatureList;
    me->GetCreatureListWithEntryInGrid(creatureList, (uint32)npcEntry, 15.0f);
 
    Creature* pRepairNpc = nullptr;
    float bestDist = 999.0f;
    for (Creature* c : creatureList)
    {
        if (c && c->IsAlive() &&
            (c->GetUInt32Value(UNIT_NPC_FLAGS) & UNIT_NPC_FLAG_REPAIR))
        {
            float d = me->GetDistance(c);
            if (d < bestDist)
            {
                bestDist = d;
                pRepairNpc = c;
            }
        }
    }
 
    if (!pRepairNpc)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-REPAIR] %s: no repair NPC with entry %d within 15yd",
            me->GetName(), npcEntry);
        BridgeSendEvent("REPAIR_FAIL", "reason=npc_not_found");
        return;
    }
 
    // --- Check if bot has any damaged gear before attempting repair ---
    bool hasDamage = false;
    for (int i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        Item* item = me->GetItemByPos(INVENTORY_SLOT_BAG_0, (uint8)i);
        if (item && item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY) > 0 &&
            item->GetUInt32Value(ITEM_FIELD_DURABILITY) <
            item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY))
        {
            hasDamage = true;
            break;
        }
    }
 
    if (!hasDamage)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-REPAIR] %s: no damaged gear — nothing to repair", me->GetName());
        char eventData[128];
        snprintf(eventData, sizeof(eventData), "cost=0|copper_total=%u", me->GetMoney());
        BridgeSendEvent("REPAIR_ACK", eventData);
        return;
    }
 
    // --- Repair all gear ---
    // DurabilityRepairAll(bool cost, float discountMod) → returns total copper spent
    //   cost=true: deduct gold from player
    //   discountMod=1.0: no faction reputation discount
    //   Returns 0 if can't afford any repairs
    uint32 totalCost = me->DurabilityRepairAll(true, 1.0f);
 
    if (totalCost == 0 && hasDamage)
    {
        // Gear is damaged but nothing was repaired — can't afford
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-REPAIR] %s: gear is damaged but can't afford repair (gold=%u)",
            me->GetName(), me->GetMoney());
        BridgeSendEvent("REPAIR_FAIL", "reason=not_enough_gold");
        return;
    }
 
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-REPAIR] %s: repaired all gear at %s (entry=%u) — cost %uc, %uc remaining",
        me->GetName(), pRepairNpc->GetName(), pRepairNpc->GetEntry(),
        totalCost, me->GetMoney());
 
    char eventData[128];
    snprintf(eventData, sizeof(eventData), "cost=%u|copper_total=%u", totalCost, me->GetMoney());
    BridgeSendEvent("REPAIR_ACK", eventData);
}

// ============================================================
// BridgeHandleGearUp — one-shot prep for an already-spawned bot.
//
// Re-uses every existing helper the codebase already has — no reinvented
// logic, no duplicated levelling / talent / spec / gear pipelines. Wired to
// the bridge so a fleet bot that was originally loaded at level 1 can be
// promoted without a server restart.
//
// Order matters — PopulateSpellData rebuilds the bot's internal spell
// reference list from me->GetSpellMap(), so every LearnSpell call must run
// BEFORE it. Same constraint applies to AddAllSpellReagents (iterates the
// populated list) and UpdateSkillsToMaxSkillsForLevel (uses GetLevel()).
// The sequence below keeps the level set early, all learning before any
// rebuild, then refreshes the derived caches last.
//
// Payload (all optional):
//   level       uint  target level (default 60)
//   mount_item  uint  item entry to AddItemToInventory (default 0 = skip)
//   riding      bool  teach Apprentice + Journeyman Riding (default true)
//
// C# side mirrors this in BotBridgeService.SendGearUpAsync. Same defaults.
//
// Dispatch: BridgeProcessLine → else if (strcmp(msgType,"GEAR_UP")==0) BridgeHandleGearUp(line);
// HEADER:    void BridgeHandleGearUp(const char* json);
// ============================================================
void AiBotAI::BridgeHandleGearUp(const char* json)
{
    if (!me || !me->IsInWorld())
        return;

    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json;

    int  targetLevel = 60;
    int  mountItem   = 0;
    int  teachRidingRaw = 1;          // default: yes
    JsonExtractInt(payload, "level",      targetLevel);
    JsonExtractInt(payload, "mount_item", mountItem);
    JsonExtractInt(payload, "riding",     teachRidingRaw);
    bool teachRiding = (teachRidingRaw != 0);

    if (targetLevel < 1)   targetLevel = 1;
    if (targetLevel > 80)  targetLevel = 80;

    // 1. Level up to target — gives the level field a sane value for every
    //    subsequent helper (skill max uses it; premade spec templates key on it).
    if (me->GetLevel() != targetLevel)
    {
        me->GiveLevel((uint32)targetLevel);
        me->InitTalentForLevel();
        me->SetUInt32Value(PLAYER_XP, 0);
    }

    // 2. Skill max — depends on the new level. Run BEFORE learning spells so
    //    weapon/armor/etc cap to the right value while the spell map is still
    //    small (no chance of PopulateSpellData clobbering).
    me->UpdateSkillsToMaxSkillsForLevel();

    // 3. Learn spells. PopulateSpellData rebuilds the bot's internal spell
    //    reference list from me->GetSpellMap() at the end — every LearnSpell
    //    call must be before that rebuild or the spell is invisible to the AI.
    LearnPremadeSpecForClass();

    if (teachRiding)
    {
        if (!me->HasSpell(33388)) me->LearnSpell(33388u, false);   // Apprentice Riding
        if (targetLevel >= 60 && !me->HasSpell(33391))
            me->LearnSpell(33391u, false);                          // Journeyman Riding
        // The upgrade flag unlocks the 100% speed modifier on 60% mounts.
        me->SetCharacterFlag(CHARACTER_FLAG_MOUNT_UPGRADED, true);
    }

    // 4. Role + gear. AutoAssignRole is harmless if m_role is already set.
    if (m_role == ROLE_INVALID)
        AutoAssignRole();
    AutoEquipGear(sWorld.getConfig(CONFIG_UINT32_BATTLE_BOT_AUTO_EQUIP));

    // 5. Rebuild the AI's spell reference list NOW — after every LearnSpell
    //    call. AddAllSpellReagents iterates this list, so it must be populated
    //    first.
    ResetSpellData();
    PopulateSpellData();
    AddAllSpellReagents();

    // 6. Optional mount item. AddItemToInventory (inherited from
    //    CombatBotBaseAI) auto-equips it; the bot can summon from it on
    //    next idle tick.
    if (mountItem > 0)
        AddItemToInventory((uint32)mountItem, 1);

    me->SetHealthPercent(100.0f);
    me->SetPowerPercent(me->GetPowerType(), 100.0f);

    // Persist (the in-memory state is great, but the playerbot table is
    // the source of truth on next server restart). SaveToDB on Player
    // flushes all sub-tables.
    me->SaveToDB();

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-GEARUP] %s: level=%d, spec learned, gear equipped, skills maxed, riding=%s, mount_item=%d",
        me->GetName(), targetLevel, teachRiding ? "yes" : "no", mountItem);
}

void AiBotAI::BridgeHandleUseGameObject(const char* json)
{
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json;

    int goEntryInt = 0;
    JsonExtractInt(payload, "go_entry", goEntryInt);
    uint32 goEntry = (uint32)goEntryInt;

    if (!goEntry)
    {
        BridgeSendEvent("USE_GO_FAIL", "reason=bad_payload");
        return;
    }

    // Find nearest spawned GO of this entry within 15yd
    std::list<GameObject*> goList;
    me->GetGameObjectListWithEntryInGrid(goList, goEntry, 15.0f);

    GameObject* obj = nullptr;
    float closestDist = 999.0f;
    for (auto* go : goList)
    {
        if (!go->isSpawned()) continue;
        if (go->getLootState() != GO_READY) continue;
        float dist = me->GetDistance(go);
        if (dist < closestDist)
        {
            closestDist = dist;
            obj = go;
        }
    }

    if (!obj)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "reason=not_found|go_entry=%u", goEntry);
        BridgeSendEvent("USE_GO_FAIL", buf);
        return;
    }

    if (closestDist > 10.0f)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "reason=too_far|go_entry=%u|dist=%d", goEntry, (int)closestDist);
        BridgeSendEvent("USE_GO_FAIL", buf);
        return;
    }

    // [GO-INTERACT CREDIT] Credit any GO-interact quest objective for this GO (a negative
    // ReqCreatureOrGO). Looting alone NEVER fires this — a pure-interact GO (lever, brazier,
    // fire) advances only via CastedCreatureOrGO. Harmless for non-interact GOs: if no quest
    // in the log requires this GO entry, it's a no-op.
    // NOTE: verify the CastedCreatureOrGO signature against your Player.h (this core:
    //   void Player::CastedCreatureOrGO(uint32 entry, ObjectGuid guid, uint32 spell_id)).
    me->CastedCreatureOrGO(obj->GetEntry(), obj->GetObjectGuid(), 0);

    // Get loot template ID from GO info
    uint32 lootId = obj->GetGOInfo()->GetLootId();
    if (!lootId)
    {
        // No loot — still a valid use (some GOs give quest credit on interact)
        obj->SetLootState(GO_JUST_DEACTIVATED);
        char buf[128];
        snprintf(buf, sizeof(buf), "go_entry=%u|items=", goEntry);
        BridgeSendEvent("USE_GO_ACK", buf);
        return;
    }

    // Generate loot from gameobject_loot_template
    Loot& loot = obj->loot;
    loot.clear();
    loot.FillLoot(lootId, LootTemplates_Gameobject, me, false);

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-GO] %s: USE_GAMEOBJECT entry=%u lootId=%u → %zu items, %u copper",
        me->GetName(), goEntry, lootId, loot.items.size(), loot.gold);

    // Take gold
    uint32 gold = loot.gold;
    if (gold > 0)
    {
        me->ModifyMoney((int32)gold);
        me->LootMoney((int32)gold, &loot);
        loot.gold = 0;
    }

    // Take items — same pattern as DoAutoLoot
    me->AutoStoreLoot(loot);

    // Build loot summary for bridge events
    std::string itemStr;
    uint32 itemsLooted = 0;
    for (size_t i = 0; i < loot.items.size(); ++i)
    {
        LootItem& item = loot.items[i];
        if (item.is_looted)
        {
            itemsLooted++;
            if (!itemStr.empty()) itemStr += ",";
            itemStr += std::to_string(item.itemid) + ":" + std::to_string(item.count);
        }
    }

    // Despawn — GO will respawn on its timer
    loot.clear();
    obj->SetLootState(GO_JUST_DEACTIVATED);

    // Auto-equip bags first, then gear
    TryAutoEquipBags();
    TryAutoEquip();

    // Emit LOOT event — same format as DoAutoLoot so C# quest item tracking works unchanged
    std::string lootData = "gold=" + std::to_string(gold);
    if (!itemStr.empty())
        lootData += "|items=" + itemStr;
    BridgeSendEvent("LOOT", lootData.c_str());

    // Emit USE_GO_ACK so C# QuestingDomain knows the interaction succeeded
    std::string ackData = "go_entry=" + std::to_string(goEntry) + "|items=" + itemStr;
    BridgeSendEvent("USE_GO_ACK", ackData.c_str());

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-GO] %s: === DONE === %u items stored, %u copper, GO despawned",
        me->GetName(), itemsLooted, gold);
}

// ============================================================
// BridgeHandleQuestCast — [CLASS-QUEST] cast a quest spell on a target creature.
//
// Drives "cast spell S on creature C" objectives (quest_template ReqSpellCast) — the
// priest heal-an-NPC, and the same shape for other class-quest "do X to this NPC" steps.
//   payload: { spell_id, entry (creature_template to find nearby) | guid, count?, radius? }
//
// The bot casts through the REAL spell path (me->CastSpell), so — because the bot is a
// real Player — the core's quest hook should credit the objective exactly as it does for a
// human. This handler only STARTS the cast + ACKs; the C# planner confirms completion by
// re-syncing the quest log on the next STATE (same as a grind leg).
//
// If a live test shows the cast alone does NOT credit (the same surprise USE_GAMEOBJECT had
// with GO-interact), uncomment the explicit CastedCreatureOrGO line below.
//
// DISPATCH: BridgeProcessLine -> else if (strcmp(msgType,"QUEST_CAST")==0) BridgeHandleQuestCast(line);
// HEADER:   void BridgeHandleQuestCast(const char* json);
// ============================================================
void AiBotAI::BridgeHandleQuestCast(const char* json)
{
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json;

    int spellIdInt = 0, entryInt = 0, guidInt = 0, countInt = 1;
    float radius = 15.0f;
    JsonExtractInt(payload, "spell_id", spellIdInt);
    JsonExtractInt(payload, "entry", entryInt);
    JsonExtractInt(payload, "guid", guidInt);
    JsonExtractInt(payload, "count", countInt);
    JsonExtractFloat(payload, "radius", radius);

    uint32 spellId = (uint32)spellIdInt;
    if (!spellId)
    {
        BridgeSendEvent("QUEST_CAST_FAIL", "reason=bad_payload");
        return;
    }
    if (radius <= 0.0f || radius > 60.0f)
        radius = 15.0f;

    // --- Resolve the target: by guid if the planner gave one, else nearest ALIVE of `entry`. ---
    Creature* pTarget = nullptr;
    if (guidInt > 0)
    {
        pTarget = me->GetMap()->GetCreature(ObjectGuid(HIGHGUID_UNIT, uint32(guidInt)));
    }
    else if (entryInt > 0)
    {
        std::list<Creature*> creatureList;
        me->GetCreatureListWithEntryInGrid(creatureList, (uint32)entryInt, radius);
        float closest = 999.0f;
        for (auto* c : creatureList)
        {
            if (!c || !c->IsAlive())
                continue;
            float d = me->GetDistance(c);
            if (d < closest)
            {
                closest = d;
                pTarget = c;
            }
        }
    }

    if (!pTarget)
    {
        char buf[160];
        snprintf(buf, sizeof(buf), "reason=target_not_found|entry=%d|spell=%u", entryInt, spellId);
        BridgeSendEvent("QUEST_CAST_FAIL", buf);
        return;
    }

    // --- Range / LOS guard: the planner should have walked us in first; don't cast blind. ---
    float dist = me->GetDistance(pTarget);
    if (dist > 30.0f || !me->IsWithinLOSInMap(pTarget))
    {
        char buf[160];
        snprintf(buf, sizeof(buf), "reason=too_far|entry=%d|spell=%u|dist=%d", entryInt, spellId, (int)dist);
        BridgeSendEvent("QUEST_CAST_FAIL", buf);
        return;
    }

    // --- Cast. Trigger the cast when the bot doesn't "know" the spell (item-granted quest
    //     spells, e.g. a provided rod); otherwise cast it for real so cast time / cost apply.
    //     Called as a plain statement so it compiles whether CastSpell returns void or a result. ---
    me->StopMoving();
    me->SetFacingToObject(pTarget);

    bool triggered = !me->HasSpell(spellId);
    me->CastSpell(pTarget, spellId, triggered);

    // If the live test shows the cast alone doesn't credit the objective, uncomment:
    // me->CastedCreatureOrGO(pTarget->GetEntry(), pTarget->GetObjectGuid(), spellId);

    char ack[160];
    snprintf(ack, sizeof(ack), "entry=%u|spell=%u|guid=%u",
        pTarget->GetEntry(), spellId, pTarget->GetGUIDLow());
    BridgeSendEvent("QUEST_CAST_ACK", ack);

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-QCAST] %s: cast spell %u on %s (entry=%u, dist=%.1f) — objective should credit on cast complete",
        me->GetName(), spellId, pTarget->GetName(), pTarget->GetEntry(), dist);
}

// ============================================================
// METHOD 2: BridgeHandleFormGroup — NEW METHOD
//
// Add after BridgeHandleUseGameObject in AiBotAI.cpp.
// Creates a WoW Group with this bot as leader, adds followers.
// Uses NEED_BEFORE_GREED so only eligible players see roll windows.
// ============================================================
 
void AiBotAI::BridgeHandleFormGroup(const char* json)
{
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json;
 
    // Parse member_guids array: "member_guids":[5,8,12]
    const char* arrStart = strstr(payload, "\"member_guids\"");
    if (!arrStart)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[AIBOT-GROUP] %s: FORM_GROUP missing member_guids", me->GetName());
        BridgeSendEvent("FORM_GROUP_FAIL", "missing member_guids");
        return;
    }
 
    arrStart = strchr(arrStart, '[');
    if (!arrStart)
    {
        BridgeSendEvent("FORM_GROUP_FAIL", "malformed member_guids");
        return;
    }
    arrStart++; // skip '['
 
    const char* arrEnd = strchr(arrStart, ']');
    if (!arrEnd)
    {
        BridgeSendEvent("FORM_GROUP_FAIL", "malformed member_guids");
        return;
    }
 
    // Extract GUIDs from comma-separated list
    std::vector<uint32> memberGuids;
    const char* p = arrStart;
    while (p < arrEnd)
    {
        while (p < arrEnd && (*p == ' ' || *p == ',')) p++;
        if (p >= arrEnd) break;
        uint32 guid = (uint32)atoi(p);
        if (guid > 0)
            memberGuids.push_back(guid);
        while (p < arrEnd && *p != ',') p++;
    }
 
    if (memberGuids.empty())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[AIBOT-GROUP] %s: FORM_GROUP no valid member GUIDs", me->GetName());
        BridgeSendEvent("FORM_GROUP_FAIL", "no valid guids");
        return;
    }
 
    // [PLAYERPARTY] Never let the god-bot yank this bot out of a REAL player's party
    // (2026-07-07). A stale C# coordinator decision must not disband a human's escort
    // mid-quest — the human outranks the coordinator, always. C# also stands down off the
    // pparty STATE echo, so this refusal is the belt to that suspender.
    if (FindPartyBoss())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-GROUP] %s: FORM_GROUP refused — this bot is in a REAL player's party",
            me->GetName());
        BridgeSendEvent("FORM_GROUP_FAIL", "in_player_party");
        return;
    }

    // If already in a group, leave it first
    if (Group* oldGroup = me->GetGroup())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-GROUP] %s: already in a group, leaving first", me->GetName());
        oldGroup->RemoveMember(me->GetObjectGuid(), 0);
    }
 
    // Create a new Group with this bot as leader
    Group* group = new Group;
    if (!group->Create(me->GetObjectGuid(), me->GetName()))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[AIBOT-GROUP] %s: Group::Create failed", me->GetName());
        delete group;
        BridgeSendEvent("FORM_GROUP_FAIL", "create_failed");
        return;
    }
 
    // NEED_BEFORE_GREED: only eligible players see the roll window.
    // StartLootRoll checks CanUseItem — priests won't roll on plate, etc.
    group->SetLootMethod(NEED_BEFORE_GREED);
 
    uint32 added = 0;
    for (uint32 memberGuid : memberGuids)
    {
        Player* pMember = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, memberGuid));
        if (!pMember)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-GROUP] %s: FORM_GROUP member GUID %u not found online",
                me->GetName(), memberGuid);
            continue;
        }
 
        // [PLAYERPARTY] Skip a member currently escorting a REAL player — pulling him out
        // of the human's party to form a bot group is exactly the yank the leader guard
        // above refuses for ourselves (2026-07-07).
        bool memberInPlayerParty = false;
        if (Group* memberOldGroup = pMember->GetGroup())
        {
            for (GroupReference* mItr = memberOldGroup->GetFirstMember(); mItr != nullptr; mItr = mItr->next())
                if (Player* pOther = mItr->getSource())
                    if (pOther->GetSession() && !pOther->GetSession()->GetBot())
                    {
                        memberInPlayerParty = true;
                        break;
                    }
        }
        if (memberInPlayerParty)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-GROUP] %s: FORM_GROUP skipping %s (GUID %u) — escorting a REAL player",
                me->GetName(), pMember->GetName(), memberGuid);
            continue;
        }

        // If member is already in a group, remove them first
        if (Group* memberOldGroup = pMember->GetGroup())
        {
            memberOldGroup->RemoveMember(pMember->GetObjectGuid(), 0);
        }
 
        if (!group->AddMember(pMember->GetObjectGuid(), pMember->GetName()))
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-GROUP] %s: FORM_GROUP AddMember failed for %s (GUID %u)",
                me->GetName(), pMember->GetName(), memberGuid);
            continue;
        }
 
        added++;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-GROUP] %s: added %s (GUID %u) to group",
            me->GetName(), pMember->GetName(), memberGuid);
    }
 
    if (added == 0)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[AIBOT-GROUP] %s: FORM_GROUP no members added, disbanding", me->GetName());
        group->Disband();
        BridgeSendEvent("FORM_GROUP_FAIL", "no_members_added");
        return;
    }
 
    char eventData[128];
    snprintf(eventData, sizeof(eventData), "members=%u|leader=%u", added + 1, me->GetGUIDLow());
    BridgeSendEvent("FORM_GROUP_ACK", eventData);
 
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-GROUP] %s: group formed — %u members, loot=NEED_BEFORE_GREED",
        me->GetName(), added + 1);
}

// ============================================================
// METHOD 3: BridgeHandleDisbandGroup — NEW METHOD
//
// Add after BridgeHandleFormGroup.
// ============================================================
 
void AiBotAI::BridgeHandleDisbandGroup(const char* json)
{
    Group* group = me->GetGroup();
    if (!group)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-GROUP] %s: DISBAND_GROUP but not in a group", me->GetName());
        BridgeSendEvent("GROUP_DISBANDED", "was_not_grouped");
        return;
    }

    // [PLAYERPARTY] Never disband a REAL player's party from the wire (2026-07-07) — the
    // human formed it, only the human unforms it. Same rationale as the FORM_GROUP guard.
    if (FindPartyBoss())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-GROUP] %s: DISBAND_GROUP refused — this is a REAL player's party",
            me->GetName());
        BridgeSendEvent("GROUP_DISBAND_FAIL", "in_player_party");
        return;
    }
 
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-GROUP] %s: disbanding group", me->GetName());
 
    group->Disband();
    BridgeSendEvent("GROUP_DISBANDED", "");
}

void AiBotAI::BridgeHandleResurrect(const char* json)
{
    if (!me)
        return;

    // A graveyard self-rez is already in flight: we've teleported the ghost and will rez it
    // ourselves once the teleport lands (see UpdateAI). Ignore ANY rez command until then —
    // a stray plain RESURRECT here would rez us mid-teleport, back in the death pocket (the loop).
    if (m_pendingGraveyardRez)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT] %s: RESURRECT ignored — graveyard self-rez in flight", me->GetName());
        return;
    }

    if (!me->IsDead() && me->GetDeathState() != DEAD)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: RESURRECT but bot is not dead, ignoring", me->GetName());
        BridgeSendEvent("RESPAWN", "");  // tell C# we're alive anyway
        return;
    }

    int atGraveyard = 0;
    const char* payload = strstr(json, "\"payload\"");
    if (!payload) payload = json;
    JsonExtractInt(payload, "at_graveyard", atGraveyard);

    // ── Death-loop / death-march escape: GHOST PORT, then SELF-rez once it lands ──
    // We port the *ghost* (non-combatable — zero aggro) with NearTeleportTo, stay dead, arm
    // m_pendingGraveyardRez, and emit GRAVEYARD_PORT (tells C# the port was accepted so it
    // stops its deadline). UpdateAI then resurrects us the moment the teleport has actually
    // applied — NOT on a C# roundtrip — so we can never rez in the kill pocket before moving.
    if (atGraveyard)
    {
        float dx = me->GetPositionX(), dy = me->GetPositionY(), dz = me->GetPositionZ();
        uint32 mapId = me->GetMapId();

        // [HEARTH] (FINDING_008) Optional racial-start override. A persistent death loop the nearest
        // graveyard can't break (the graveyard is adjacent to the killer camp — SneakyShock's 307-death
        // Wetlands loop) ports the ghost to the bot's RACIAL START instead. C# only sends this when the
        // home is on THIS map (same-map NearTeleportTo — identical proven ghost-port seam as below); a
        // cross-continent home simply omits it and falls through to the normal graveyard logic. Reuses
        // the whole m_pendingGraveyardRez self-rez path, so the ghost is invulnerable in transit.
        float hx = 0.0f, hy = 0.0f, hz = 0.0f; int hmap = -1;
        if (JsonExtractFloat(payload, "home_x", hx) && JsonExtractFloat(payload, "home_y", hy) &&
            JsonExtractFloat(payload, "home_z", hz) && JsonExtractInt(payload, "home_map", hmap) &&
            hmap == (int)mapId)
        {
            ReGroundZ(hx, hy, hz, "rez-home");
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT] %s HEARTH port (ghost): (%.1f, %.1f, %.1f) -> racial start (%.1f, %.1f, %.1f) map=%u",
                me->GetName(), dx, dy, dz, hx, hy, hz, mapId);
            me->NearTeleportTo(hx, hy, hz, me->GetOrientation());
            m_pendingGraveyardRez = true;
            m_graveRezX = hx; m_graveRezY = hy; m_graveRezZ = hz;
            m_graveRezMap = mapId; m_graveRezWaitMs = 8000;
            BridgeSendEvent("GRAVEYARD_PORT", "");
            return;
        }

        // Primary: zone-linked nearest graveyard for the death position.
        WorldSafeLocsEntry const* grave = sObjectMgr.GetClosestGraveYard(dx, dy, dz, mapId, me->GetTeam());

        // Fallback: the death pos resolves to a zone with NO graveyard link (areaId 0 /
        // out-of-bounds pocket — the Odugi/Haxixaw 200+ death loop). GetClosestGraveYard
        // returns null there, and the OLD code rez'd in place => infinite re-death. Probe
        // outward in rings; a probe point that lands in a REAL adjacent zone makes
        // GetClosestGraveYard return that zone's nearest valid, same-faction, level-
        // appropriate graveyard. Keep the hit closest to the actual death position.
        if (!grave)
        {
            static const float kRings[] = { 150.0f, 350.0f, 600.0f, 1000.0f };
            float bestSq = 0.0f;
            for (float r : kRings)
            {
                for (int a = 0; a < 8; ++a)
                {
                    float ang = (float)a * (M_PI_F / 4.0f);
                    float px = dx + r * cosf(ang);
                    float py = dy + r * sinf(ang);
                    WorldSafeLocsEntry const* g =
                        sObjectMgr.GetClosestGraveYard(px, py, dz, mapId, me->GetTeam());
                    if (!g)
                        continue;
                    float gdx = g->x - dx, gdy = g->y - dy;
                    float dsq = gdx * gdx + gdy * gdy;
                    if (!grave || dsq < bestSq) { grave = g; bestSq = dsq; }
                }
                if (grave)
                    break;   // nearest ring with any hit wins
            }
            if (grave)
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT] %s graveyard port: death pos in dead zone (areaId 0?) — "
                    "probe found valid graveyard %.0fyd away", me->GetName(), sqrtf(bestSq));
        }

        // Last resort: still nothing AND spawn point is on this map — port to spawn
        // (always a valid, level-appropriate starter loc). Never rez-in-place into the pocket.
        if (!grave && m_spawnMapId == mapId)
        {
            // [GROUND] Spawn coords are authored, but ground them anyway so a bad spawn row
            // can't float the ghost. Local copy → snap → stash the grounded value so the
            // UpdateAI landed-check measures against the real target.
            float sx = m_spawnX, sy = m_spawnY, sz = m_spawnZ;
            ReGroundZ(sx, sy, sz, "rez-spawn");

            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT] %s graveyard port: no graveyard near death pos — porting ghost to spawn (%.1f,%.1f,%.1f)",
                me->GetName(), sx, sy, sz);
            me->NearTeleportTo(sx, sy, sz, m_spawnO);
            m_pendingGraveyardRez = true;
            m_graveRezX = sx; m_graveRezY = sy; m_graveRezZ = sz;
            m_graveRezMap = mapId; m_graveRezWaitMs = 8000;
            BridgeSendEvent("GRAVEYARD_PORT", "");
            return;
        }

        if (!grave)
        {
            // Genuinely nowhere on this map (cross-map spawn + no graveyard) — degrade to the
            // old in-place rez, but log loudly so this rare case is visible if it ever bites.
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                "[AIBOT] %s graveyard port: NO valid destination on map %u — rez in place (STUCK RISK)",
                me->GetName(), mapId);
            me->ResurrectPlayer(0.5f);
            me->CombatStop(true);
            me->SpawnCorpseBones();
            BridgeSendEvent("RESPAWN", "");
            BridgeSendState();
            return;
        }

        // [GROUND] Graveyard rows are authored on the ground (no-op in the normal case), but
        // snapping catches a bad/edge graveyard entry. const grave → local copy.
        float gx = grave->x, gy = grave->y, gz = grave->z;
        ReGroundZ(gx, gy, gz, "rez-grave");

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT] %s graveyard port (ghost): (%.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f) map=%u",
            me->GetName(), dx, dy, dz, gx, gy, gz, mapId);

        me->NearTeleportTo(gx, gy, gz, me->GetOrientation());

        // Ghost stays DEAD on purpose; the teleport applies next tick. Arm the self-rez:
        // UpdateAI resurrects us once we've actually arrived at the graveyard. GRAVEYARD_PORT
        // just tells C# the port was accepted (stop its deadline) — the RESPAWN follows from us.
        m_pendingGraveyardRez = true;
        m_graveRezX = gx; m_graveRezY = gy; m_graveRezZ = gz;
        m_graveRezMap = mapId; m_graveRezWaitMs = 8000;
        BridgeSendEvent("GRAVEYARD_PORT", "");
        return;
    }

    // ── Plain rez (at the bot's current pos — which, post-port, IS the graveyard) ──
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT] %s resurrecting at (%.1f, %.1f, %.1f)",
        me->GetName(), me->GetPositionX(), me->GetPositionY(), me->GetPositionZ());

    me->ResurrectPlayer(0.5f);
    me->CombatStop(true);   // insurance: death cleared combat; never resume rez in-combat
    me->SpawnCorpseBones();

    BridgeSendEvent("RESPAWN", "");
    BridgeSendState();

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT] %s resurrected at 50%% HP", me->GetName());
}


// ============================================================
// EVENT SENDERS — C++ → C# notifications
// ============================================================

void AiBotAI::SendKillEvent(uint32 creatureEntry, uint32 creatureGuidLow)
{
    if (!m_bridgeConnected)
        return;

    char json[256];
    snprintf(json, sizeof(json),
        "{\"type\":\"EVENT\",\"payload\":{"
        "\"guid\":%u,\"event\":\"KILL\","
        "\"creature_entry\":%u,\"creature_guid\":%u}}",
        me->GetGUIDLow(), creatureEntry, creatureGuidLow);
    BridgeSend(json);
}

void AiBotAI::SendQuestUpdateEvent(uint32 questId, const char* status)
{
    if (!m_bridgeConnected)
        return;

    char json[256];
    snprintf(json, sizeof(json),
        "{\"type\":\"EVENT\",\"payload\":{"
        "\"guid\":%u,\"event\":\"QUEST_UPDATE\","
        "\"quest_id\":%u,\"status\":\"%s\"}}",
        me->GetGUIDLow(), questId, status);
    BridgeSend(json);
}

void AiBotAI::SendLevelUpEvent(uint32 newLevel)
{
    if (!m_bridgeConnected)
        return;

    char json[128];
    snprintf(json, sizeof(json),
        "{\"type\":\"EVENT\",\"payload\":{"
        "\"guid\":%u,\"event\":\"LEVEL_UP\",\"new_level\":%u}}",
        me->GetGUIDLow(), newLevel);
    BridgeSend(json);
}

// ── C0 (§5.1): minimal JSON string escape for outbound chat text. Chat is the one
// bridge lane carrying arbitrary player-typed text; a `"` or `\` in a message corrupts
// the newline-delimited JSON framing. Escapes `"` `\`; control chars (<0x20) become a
// space (in-game chat can't contain meaningful ones). Truncates safely on small dst.
static void JsonEscapeInto(char* dst, size_t dstSize, const char* src)
{
    size_t o = 0;
    if (!dst || dstSize == 0) return;
    for (const char* p = src ? src : ""; *p && o + 2 < dstSize; ++p)
    {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\')
        {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        }
        else if (c < 0x20)
            dst[o++] = ' ';
        else
            dst[o++] = (char)c;
    }
    dst[o] = '\0';
}

void AiBotAI::SendChatRecvEvent(const char* senderName, const char* message, const char* chatType, const char* channelName, uint32 senderGuidLow)
{
    if (!m_bridgeConnected)
        return;

    // Escaped copies (§5.1): sender, message, channel_name are player-influenced text.
    // chatType is compiler-controlled ("say"/"whisper"/"channel"/"party") — no escape needed.
    char senderEsc[128];
    char messageEsc[1024];   // 511-char message cap upstream; worst case doubles under escaping
    char channelEsc[128];
    JsonEscapeInto(senderEsc, sizeof(senderEsc), senderName);
    JsonEscapeInto(messageEsc, sizeof(messageEsc), message);
    JsonEscapeInto(channelEsc, sizeof(channelEsc), channelName ? channelName : "");

    char json[1024];
    int n = snprintf(json, sizeof(json),
        "{\"type\":\"EVENT\",\"payload\":{"
        "\"guid\":%u,\"event\":\"CHAT_RECV\","
        "\"sender\":\"%s\",\"sender_guid\":%u,\"message\":\"%s\","
        "\"chat_type\":\"%s\",\"channel_name\":\"%s\"}}",
        me->GetGUIDLow(), senderEsc, senderGuidLow, messageEsc, chatType, channelEsc);

    // A truncated line is broken JSON that poisons the newline framing — drop it instead.
    if (n < 0 || n >= (int)sizeof(json))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: CHAT_RECV dropped — escaped payload exceeds buffer (%d)",
            me->GetName(), n);
        return;
    }
    BridgeSend(json);
}