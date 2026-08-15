/*
 * AiBotAIMain.cpp — Autonomous AI bot for VMaNGOS 1.12.1
 *
 * Split from the monolithic AiBotAI.cpp. THIS TU holds the engine entry points:
 * the lifecycle overrides (OnSessionLoaded / OnPlayerLogin / OnPacketReceived /
 * MovementInform) and the UpdateAI main loop that orchestrates everything else.
 *
 * The behaviour it drives lives in sibling TUs (all member functions of AiBotAI,
 * so they link across files transparently):
 *   combat   → AiBotAICombat.cpp     bridge → AiBotAIBridge.cpp
 *   movement → AiBotAIMovement.cpp    loot   → AiBotAILoot.cpp
 *   grind    → AiBotAIGrind.cpp
 */

#include "AiBotAIMain.h"
#include "Server/Packet.h"   // NullClientPacket — the typed empty client packet the group accept/decline handlers take
#include "AiBotAITeamPlay.h"   // [TEAMPLAY] ResolveCombatTarget — the group focus-fire resolver
#include "SuiPossess.h"      // [SUI] possessed-bot-as-boss precedence in FindPartyBoss
#include "Player.h"
#include <cstring>
#include <cstdio>
#include <cctype>   // tolower — [FOLLOW-CMD] case-insensitive escort-name match
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
// LIFECYCLE
// ============================================================

void AiBotAI::OnPlayerLogin()
{
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] OnPlayerLogin for %s (guid %u)",
        me ? me->GetName() : "NULL",
        me ? me->GetGUIDLow() : 0);

    // Bots always accept whispers (the runtime equivalent of a GM's ".whispers on").
    // PlayerBotMgr sessions are synthetic (no realmd row) and may carry elevated
    // security; the core hides higher-security characters from lower-security
    // whisperers ("That player doesn't exist") UNLESS the target accepts whispers.
    // A social-layer bot must be whisperable by everyone, always.
    me->SetAcceptWhispers(true);

    if (!m_initialized)
        me->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SPAWNING);

        // Persist character to DB so it survives restarts
        me->SaveToDB();
}

// [SUI] Ctrl+RightClick waypoint chain. An idle bot starts the first leg right
// away through the normal bridge MOVE_TO path (chunked pathfinding included); a
// bot already walking an ordered leg appends, and arrival chains the next leg.
void AiBotAI::SuiQueueWaypoint(float x, float y, float z)
{
    if (m_currentTask.type == TASK_MOVE_TO)
    {
        m_suiWaypoints.push_back({x, y, z});
        return;
    }
    char json[160];
    snprintf(json, sizeof(json),
        "{\"type\":\"MOVE_TO\",\"payload\":{\"mapId\":%u,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}}",
        me->GetMapId(), x, y, z);
    BridgeProcessLine(json);
}

AiBotAI* AiBotAI::AttachToRealCharacter(Player* owner)
{
    if (!owner || !owner->IsInWorld())
        return nullptr;

    AiBotAI* ai = new AiBotAI(owner->GetRace(), owner->GetClass(), owner->GetLevel(),
        owner->GetMapId(), owner->GetInstanceId(),
        owner->GetPositionX(), owner->GetPositionY(), owner->GetPositionZ(),
        owner->GetOrientation());
    ai->SetPlayer(owner);
    ai->m_ownedDummyEntry = std::make_unique<PlayerBotEntry>();
    ai->botEntry = ai->m_ownedDummyEntry.get();

    // The fabricated-bot login work must not touch a real character: no premade spec,
    // no auto-equip, no skill-maxing, no reagent grants, no heal, no channel joins.
    // Role + spell data are the AI-internal pieces UpdateAI needs from that block.
    ai->AutoAssignRole();
    ai->ResetSpellData();
    ai->PopulateSpellData();
    ai->m_freshSpawn = false;
    ai->m_initialized = true;
    ai->m_lastKnownLevel = owner->GetLevel();

    owner->SetAI(ai);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT] %s (guid %u) real character enrolled as fleet AI",
        owner->GetName(), owner->GetGUIDLow());
    return ai;
}

bool AiBotAI::OnSessionLoaded(PlayerBotEntry* entry, WorldSession* sess)
{
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT] OnSessionLoaded GUID=%u race=%u class=%u level=%u name='%s'",
        entry->playerGUID, m_spawnRace, m_spawnClass, m_spawnLevel,
        m_spawnName.c_str());

    // Check if this character already exists in the DB (restart vs first spawn)
    auto result = CharacterDatabase.PQuery(
        "SELECT 1 FROM `characters` WHERE `guid` = '%u'", entry->playerGUID);

    if (result)
    {
        // [SUI] HARD WALL: never adopt a character owned by a REAL account. The
        // brain auto-register once swallowed an enrolled real character
        // (Tesfff, 2026-08-10); logging it in on a synthetic bot account lets
        // SaveToDB stamp that account over the owner and the character
        // vanishes from their account list. Whatever a registry row says, a
        // real-account character is refused here.
        if (auto acctResult = CharacterDatabase.PQuery(
                "SELECT `account` FROM `characters` WHERE `guid` = '%u'", entry->playerGUID))
        {
            uint32 ownerAccount = acctResult->Fetch()[0].GetUInt32();
            if (LoginDatabase.PQuery(
                    "SELECT 1 FROM `account` WHERE `id` = '%u'", ownerAccount))
            {
                sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                    "[AIBOT] REFUSING to spawn guid %u as a bot: character belongs to REAL account %u",
                    entry->playerGUID, ownerAccount);
                return false;
            }
        }

        // RESTART PATH: character exists with gear/spells/skills intact
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT] Existing character found for GUID=%u, using LoginPlayer",
            entry->playerGUID);
        sess->LoginPlayer(entry->playerGUID);
        return true;
    }

    // FIRST SPAWN PATH: no character exists, create fresh
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT] No existing character for GUID=%u, spawning fresh",
        entry->playerGUID);

    CharacterDatabase.PExecute("DELETE FROM `character_spell` WHERE `guid` = '%u'", entry->playerGUID);
    CharacterDatabase.PExecute("DELETE FROM `character_skills` WHERE `guid` = '%u'", entry->playerGUID);
    CharacterDatabase.PExecute("DELETE FROM `character_reputation` WHERE `guid` = '%u'", entry->playerGUID);
    CharacterDatabase.PExecute("DELETE FROM `character_homebind` WHERE `guid` = '%u'", entry->playerGUID);
    CharacterDatabase.PExecute("DELETE FROM `character_action` WHERE `guid` = '%u'", entry->playerGUID);
    sObjectMgr.DeletePlayerFromCache(entry->playerGUID);

    m_freshSpawn = true;

    if (!SpawnNewPlayer(sess, m_spawnClass, m_spawnRace, m_spawnMapId,
        m_spawnInstanceId, m_spawnX, m_spawnY, m_spawnZ, m_spawnO, nullptr, m_spawnName))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[AIBOT] SpawnNewPlayer FAILED for GUID=%u", entry->playerGUID);
        return false;
    }

    // First-spawn bots skip OnPlayerLogin (that path is LoginPlayer-only), so its
    // SetAcceptWhispers never runs here — set it now so a fresh bot is whisperable
    // immediately. (This is the WHISPER half; the name-lookup half is fixed by creating
    // the character under m_spawnName above, so it registers correctly for /w AND /invite.)
    if (me)
        me->SetAcceptWhispers(true);

    if (!m_spawnName.empty() && me)
    {
        std::string oldName = me->GetName();
        sObjectMgr.DeletePlayerFromCache(me->GetGUIDLow());
        me->SetName(m_spawnName);
        sObjectMgr.InsertPlayerInCache(me);
        entry->name = m_spawnName;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] Renamed %s -> %s (GUID=%u)",
            oldName.c_str(), m_spawnName.c_str(), me->GetGUIDLow());
    }

    if (m_spawnLevel > 1 && me)
    {
        me->GiveLevel(m_spawnLevel);
        me->SetUInt32Value(PLAYER_XP, 0);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] Set level %u for %s (GUID=%u)",
            m_spawnLevel, me->GetName(), me->GetGUIDLow());
    }

    return true;
}

void AiBotAI::OnPacketReceived(WorldPacket const* packet)
{
    // ── Session 31: Smart need/greed loot roll ──
    // CombatBotBaseAI auto-passes on SMSG_LOOT_START_ROLL.
    // We intercept first and make an intelligent decision using ScoreItem.
    if (packet->GetOpcode() == SMSG_LOOT_START_ROLL)
    {
        if (!me || !me->IsInWorld() || !me->GetGroup())
        {
            CombatBotBaseAI::OnPacketReceived(packet);
            return;
        }
 
        try
        {
            WorldPacket pkt(*packet);
 
            // Packet layout (from Group::SendLootStartRoll):
            //   uint64 lootedTargetGUID
            //   uint32 itemSlot
            //   uint32 itemId
            //   uint32 randomSuffix (unused)
            //   uint32 randomPropId
            //   uint32 countDown
            uint64 rawGuid;
            uint32 itemSlot, itemId, randomSuffix, randomPropId, countDown;
            pkt >> rawGuid >> itemSlot >> itemId >> randomSuffix >> randomPropId >> countDown;
 
            ObjectGuid lootedTarget(rawGuid);
            ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);
 
            RollVote vote = ROLL_GREED; // default: greed (always better than pass for vendor gold)
 
            if (proto)
            {
                bool canEquip = false;
                bool isUpgrade = false;
 
                if (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR)
                {
                    // Check class/race/level requirements
                    bool classOk = (proto->AllowableClass == 0 ||
                                    (proto->AllowableClass & me->GetClassMask()));
                    bool raceOk  = (proto->AllowableRace == 0 ||
                                    (proto->AllowableRace & me->GetRaceMask()));
                    bool levelOk = ((uint32)proto->RequiredLevel <= me->GetLevel());
 
                    if (classOk && raceOk && levelOk)
                    {
                        canEquip = true;
 
                        // Map InventoryType → equipment slot for ScoreItem comparison
                        uint8 targetSlot = 255;
                        switch (proto->InventoryType)
                        {
                            case INVTYPE_HEAD:           targetSlot = EQUIPMENT_SLOT_HEAD; break;
                            case INVTYPE_NECK:           targetSlot = EQUIPMENT_SLOT_NECK; break;
                            case INVTYPE_SHOULDERS:      targetSlot = EQUIPMENT_SLOT_SHOULDERS; break;
                            case INVTYPE_CHEST:
                            case INVTYPE_ROBE:           targetSlot = EQUIPMENT_SLOT_CHEST; break;
                            case INVTYPE_WAIST:          targetSlot = EQUIPMENT_SLOT_WAIST; break;
                            case INVTYPE_LEGS:           targetSlot = EQUIPMENT_SLOT_LEGS; break;
                            case INVTYPE_FEET:           targetSlot = EQUIPMENT_SLOT_FEET; break;
                            case INVTYPE_WRISTS:         targetSlot = EQUIPMENT_SLOT_WRISTS; break;
                            case INVTYPE_HANDS:          targetSlot = EQUIPMENT_SLOT_HANDS; break;
                            case INVTYPE_FINGER:         targetSlot = EQUIPMENT_SLOT_FINGER1; break;
                            case INVTYPE_TRINKET:        targetSlot = EQUIPMENT_SLOT_TRINKET1; break;
                            case INVTYPE_CLOAK:          targetSlot = EQUIPMENT_SLOT_BACK; break;
                            case INVTYPE_WEAPON:
                            case INVTYPE_2HWEAPON:
                            case INVTYPE_WEAPONMAINHAND: targetSlot = EQUIPMENT_SLOT_MAINHAND; break;
                            case INVTYPE_SHIELD:
                            case INVTYPE_WEAPONOFFHAND:
                            case INVTYPE_HOLDABLE:       targetSlot = EQUIPMENT_SLOT_OFFHAND; break;
                            case INVTYPE_RANGED:
                            case INVTYPE_THROWN:
                            case INVTYPE_RANGEDRIGHT:    targetSlot = EQUIPMENT_SLOT_RANGED; break;
                            default: break;
                        }
 
                        if (targetSlot != 255)
                        {
                            float newScore = ScoreItem(proto, targetSlot);
                            float oldScore = 0.0f;
 
                            Item* currentItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, targetSlot);
                            if (currentItem && currentItem->GetProto())
                                oldScore = ScoreItem(currentItem->GetProto(), targetSlot);
 
                            if (newScore > oldScore)
                                isUpgrade = true;
                        }
                    }
                }
 
                vote = (canEquip && isUpgrade) ? ROLL_NEED : ROLL_GREED;
 
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-ROLL] %s: [%s] (id=%u q=%u) → %s%s",
                    me->GetName(),
                    proto->Name1 ? proto->Name1 : "?",
                    itemId, proto->Quality,
                    vote == ROLL_NEED ? "NEED" : "GREED",
                    isUpgrade ? " (upgrade!)" : "");
            }
            else
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-ROLL] %s: unknown item %u → GREED",
                    me->GetName(), itemId);
            }
 
            // Submit vote via the same packet path as a real player clicking Need/Greed
            auto data = std::make_unique<WorldPackets::Loot::LootRoll>();
            data->lootedTarget = lootedTarget;
            data->itemSlot = itemSlot;
            data->rollType = vote;
            me->GetSession()->QueuePacket(std::move(data));
        }
        catch (...)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                "[AIBOT-ROLL] %s: parse failed, falling back to base class (pass)",
                me->GetName());
            CombatBotBaseAI::OnPacketReceived(packet);
        }
 
        return; // Don't let base class auto-pass
    }
 
    // ── Existing: Intercept incoming chat messages (say, whisper, channel) ──
    if (packet->GetOpcode() == SMSG_MESSAGECHAT)
    {
        try
        {
            WorldPacket pkt(*packet); // copy so we can read
            uint8 chatType;
            uint32 lang;
            pkt >> chatType >> lang;
 
            if (chatType == CHAT_MSG_SAY || chatType == CHAT_MSG_WHISPER || chatType == CHAT_MSG_PARTY)
            {
                ObjectGuid senderGuid;
                pkt >> senderGuid;
 
                // SAY, YELL, PARTY have a SECOND copy of senderGuid
                if (chatType == CHAT_MSG_SAY || chatType == CHAT_MSG_YELL || chatType == CHAT_MSG_PARTY)
                {
                    ObjectGuid dupGuid;
                    pkt >> dupGuid;
                }
 
                // ── C0 self-echo filter (D16, §5.1): a bot hears its own Say/Party broadcast.
                // Forwarding it would let the bot converse with itself (reply loop). Skip entirely.
                if (senderGuid == me->GetObjectGuid())
                {
                    CombatBotBaseAI::OnPacketReceived(packet);
                    return;
                }
 
                uint32 textLen;
                pkt >> textLen;
                if (textLen > 0 && textLen < 512)
                {
                    std::string message;
                    pkt >> message;
 
                    std::string senderName = "Unknown";
                    if (Player* pSender = sObjectMgr.GetPlayer(senderGuid))
                        senderName = pSender->GetName();
 
                    const char* typeStr = (chatType == CHAT_MSG_WHISPER) ? "whisper"
                                        : (chatType == CHAT_MSG_PARTY)   ? "party"
                                        : "say";
                    // sender_guid: GUID low when the sender is a player, else 0 (§5.1)
                    uint32 senderGuidLow = senderGuid.IsPlayer() ? senderGuid.GetCounter() : 0;
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: CHAT_RECV [%s] from %s: %s",
                        me->GetName(), typeStr, senderName.c_str(), message.c_str());
                    SendChatRecvEvent(senderName.c_str(), message.c_str(), typeStr, nullptr, senderGuidLow);
                }
            }
            else if (chatType == CHAT_MSG_CHANNEL)
            {
                std::string channelName;
                pkt >> channelName;
 
                uint32 playerRank;
                pkt >> playerRank;
 
                ObjectGuid senderGuid;
                pkt >> senderGuid;
 
                // ── C0 self-echo filter (D16, §5.1): channel Say echoes back to the speaker too.
                if (senderGuid == me->GetObjectGuid())
                {
                    CombatBotBaseAI::OnPacketReceived(packet);
                    return;
                }
 
                uint32 textLen;
                pkt >> textLen;
                if (textLen > 0 && textLen < 512)
                {
                    std::string message;
                    pkt >> message;
 
                    std::string senderName = "Unknown";
                    if (Player* pSender = sObjectMgr.GetPlayer(senderGuid))
                        senderName = pSender->GetName();
 
                    // sender_guid: GUID low when the sender is a player, else 0 (§5.1)
                    uint32 senderGuidLow = senderGuid.IsPlayer() ? senderGuid.GetCounter() : 0;
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: CHAT_RECV [channel:%s] from %s: %s",
                        me->GetName(), channelName.c_str(), senderName.c_str(), message.c_str());
                    SendChatRecvEvent(senderName.c_str(), message.c_str(), "channel", channelName.c_str(), senderGuidLow);
                }
            }
        }
        catch (...)
        {
            // Packet parse failed — not critical, just skip
        }
    }
 
    // ── [PLAYERPARTY] Group invite (2026-07-07): accept from a REAL player, decline the rest ──
    // A human /invite is the entire control plane for escort mode — no UI, no bridge command.
    // Accept iff the pending group's LEADER is a real (non-bot) session; decline otherwise so a
    // stray bot-sourced invite can't graft this bot into an unmanaged group (the god-bot's own
    // grouping is direct Group::Create/AddMember — BridgeHandleFormGroup — and never sends
    // invite packets, so declining here cannot touch TeamAuto formation). Handled EXPLICITLY,
    // before the base fall-through, so behaviour never depends on CombatBotBaseAI's own invite
    // policy. On accept, ResolveDoctrine sees the real-player group next behaviour tick and
    // swaps to PlayerParty; STATE echoes pparty=1 and C# stands down.
    if (packet->GetOpcode() == SMSG_GROUP_INVITE)
    {
        bool accept = false;
        if (Group* pInviteGroup = me->GetGroupInvite())
        {
            if (Player* pLeader = sObjectMgr.GetPlayer(pInviteGroup->GetLeaderGuid()))
                accept = pLeader->GetSession() && !pLeader->GetSession()->GetBot();
        }

        if (accept)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-PARTY] %s: group invite from a REAL player — accepting (escort mode arms next tick)",
                me->GetName());
            NullClientPacket data(CMSG_GROUP_ACCEPT);   // zero-payload typed packet (Server/Packet.h)
            me->GetSession()->HandleGroupAcceptOpcode(data);
            BridgeSendEvent("PARTY_JOIN", "");
        }
        else
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-PARTY] %s: group invite is not from a real player — declining",
                me->GetName());
            NullClientPacket data(CMSG_GROUP_DECLINE);
            me->GetSession()->HandleGroupDeclineOpcode(data);
        }
        return;   // handled — never fall through to the base class's own invite policy
    }

    CombatBotBaseAI::OnPacketReceived(packet);
}

void AiBotAI::MovementInform(uint32 MovementType, uint32 Data)
{
    if (MovementType == POINT_MOTION_TYPE)
    {
        if (Data == AIBOT_POINT_WANDER)
        {
            // Arrived at wander destination — go idle, will wander again after timer
            me->GetMotionMaster()->MoveIdle();
        }
        else if (Data == AIBOT_POINT_STALEMATE_NUDGE)
        {
            // Stalemate hop landed — go idle; HandleCombatStalemate re-evaluates next tick.
            me->GetMotionMaster()->MoveIdle();
        }
        else if (Data == AIBOT_POINT_OVERPULL_FLEE)
        {
            // Retreat hop landed — go idle; HandleOverpullRetreat re-evaluates next tick.
            me->GetMotionMaster()->MoveIdle();
        }
        else if (Data == AIBOT_POINT_TASK_DEST)
        {
            // [SUI] While a human drives this bot directly, the journey must not
            // chain under them: TryBegin's stop finalizes the old spline, Finalize
            // fires this callback, and the next chunk would re-open a movespline
            // that HandleMovementOpcodes favours over every packet the human sends.
            // Commanded-from-the-free-view stays exempt — waypoint chains are
            // supposed to chain there, and the parked client sends no movement.
            if (m_possessed && !SuiPossess::IsCommandedFromFreeView(me))
                return;

            // More chunks remaining in the current (possibly partial) leg?
            if (!m_pathWaypoints.empty() &&
                m_pathIndex < (uint32)m_pathWaypoints.size() - 1)
            {
                StartNextPathChunk();
                return;
            }

            // This leg's chunks are exhausted. If it was a PARTIAL leg that stopped
            // short of the true destination, re-query and walk the next leg —
            // continuation, no distance ceiling. stopCurrentMovement = false because
            // we're inside the motion callback (MovePoint is safe here; a full
            // StopMoving()/MotionMaster::Clear() is not).
            if (m_currentTask.type == TASK_MOVE_TO)
            {
                float dist = me->GetDistance2d(m_currentTask.x, m_currentTask.y);
                if (dist > 3.0f)
                {
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT-PATH] %s: leg exhausted %.0fyd from dest — continuing journey",
                        me->GetName(), dist);
                    ClearStoredPath();
                    MoveToDestination(m_currentTask.x, m_currentTask.y, m_currentTask.z, false);
                    return;
                }
            }

            // §4: enriched objective MOVE_TO reached the deep coord with no scan hit
            // (mobs were deeper than scan range during the walk) — grind here in place
            // instead of falsely reporting "arrived" (merged step: a TASK_COMPLETE here
            // would mean objective-done with zero kills). Bare MOVE_TO emits "arrived".
            if (m_currentTask.type == TASK_MOVE_TO && m_currentTask.creatureEntry != 0)
            {
                ConvertMoveToGrindInPlace();
                return;
            }

            // Path complete (arrived, or it was a short single-MovePoint path).
            // Stamp the exact arrival coord into the event so C# refreshes ctx.Pos NOW
            // instead of waiting up to one 5s STATE cycle — otherwise a just-arrived bot
            // still reads stale-far and the planner re-issues MOVE_TO instead of interacting.
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] %s arrived at task destination", me->GetName());
            char arrBuf[96];
            snprintf(arrBuf, sizeof(arrBuf), "MOVE_TO arrived|x=%.1f|y=%.1f|z=%.1f",
                     me->GetPositionX(), me->GetPositionY(), me->GetPositionZ());
            BridgeSendEvent("TASK_COMPLETE", arrBuf);
            m_currentTask.Clear();
            ClearStoredPath();

            // [SUI] Queued RTS waypoints: chain into the next leg from inside the
            // motion callback — MoveToDestination with stopCurrentMovement=false,
            // the same rule the PARTIAL-leg continuation above follows.
            if (!m_suiWaypoints.empty())
            {
                std::array<float, 3> next = m_suiWaypoints.front();
                m_suiWaypoints.pop_front();
                if (m_suiPatrolLoop)
                    m_suiWaypoints.push_back(next);   // patrol: the route cycles
                m_currentTask.type = TASK_MOVE_TO;
                m_currentTask.x = next[0];
                m_currentTask.y = next[1];
                m_currentTask.z = next[2];
                MoveToDestination(next[0], next[1], next[2], false);
            }
        }
        else if (Data == AIBOT_POINT_GRIND_PATROL)
        {
            me->GetMotionMaster()->MoveIdle();
            // Arrived at patrol point within grind area — will pick next target or patrol again
        }
    }
}

// ============================================================
// MAIN UPDATE LOOP — COMPLETE-METHOD REPLACEMENT
//
// Drop-in replacement for AiBotAI::UpdateAI in AiBotAIMain.cpp. Identical to the
// deployed overpull/§4/[GRAVE-SELFREZ] version EXCEPT the TASK_GRIND post-freeze block,
// which now SELF-UNSTICKS the objective grind instead of handing back GRIND_BLOCKED|
// overpull_dwell (the kobold-camp livelock fix).
//
// Why: a dense field of QUEST mobs vetoed every pull (OverpullGuard: >AIBOT_OVERPULL_SOLO
// within AIBOT_PULL_DENSITY_RADIUS). The objective grind then handed back overpull_dwell,
// which C# always answered with a forced single-kill detour + re-issue of the SAME
// objective → re-freeze on the SAME camp. One kill per ~3s roundtrip until the fail/
// deadline machinery shelved the quest and the bot wandered off to easier work, beside an
// ocean of valid mobs. Now BOTH grind modes self-unstick after AIBOT_GRIND_FREEZE_DWELL:
// SelectGrindTarget already returns the LEAST-clustered candidate, HandleOverpullRetreat is
// the in-combat backstop if too many pile on, and a genuinely unkillable field still trips
// the C# 120s no-kill deadline → durable shelve. The no_target handback (truly empty field)
// is UNCHANGED. Only the overpull_dwell objective handback is removed.
//
// [TEAMPLAY] 2026-07-02 (B2/B3, the focus-fire divergence closure): the sticky-assist fix
// (v1.1) bridged only the narrowest of THREE divergence windows. This pass closes the two
// structural ones: (B3) pull discipline — a follower whose assist seam can't resolve yet
// HOLDS its grind pull for a bounded dwell so the anchor pulls first (the fresh-engagement
// seeding gap: nothing could create the FIRST assist), applied at the grind dispatch and
// the enriched-MOVE_TO approach scan; (B2) mid-combat convergence — an engaged follower
// re-checks the seam every tick and SWITCHES to the anchor's resolved victim (the old
// valid-victim branch held course for the victim's whole life and never consulted the
// resolver, so the arrival fan-out persisted until kill boundaries coincided by luck).
// ============================================================

// [DOCTRINE] Re-resolve the engagement doctrine for the current combat state and swap the
// instance only when the kind changes (so TeamAuto's sticky memo + pull-hold counter reset by
// construction on a mode switch). Cheap; called once per behaviour tick from UpdateAI.
void AiBotAI::RefreshDoctrine()
{
    DoctrineKind const kind = ResolveDoctrine(*this);
    if (!m_doctrine || kind != m_doctrineKind)
    {
        // [SUI] A Solo-era brain errand does not survive joining a human's party:
        // the planner stands down on pparty, but its last MOVE_TO kept walking the
        // bot to a dead objective — and a live TASK_MOVE_TO also gates
        // DoPartyFollow, so the new member marched away instead of forming up.
        if (kind == DoctrineKind::PlayerParty && m_doctrineKind == DoctrineKind::Solo)
            SuiAbandonJourney();
        char const* from = m_doctrine ? m_doctrine->Name() : "(none)";
        m_doctrine = MakeDoctrine(kind);
        m_doctrineKind = kind;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-DOCTRINE] %s: %s -> %s%s",
            me ? me->GetName() : "?", from, m_doctrine ? m_doctrine->Name() : "?",
            m_combatDirective.IsActive() ? " (directive active)" : "");
    }
}

// ── [PLAYERPARTY] The escort primitives (2026-07-07) ──────────────────────────────────────

// The human this bot escorts: the group LEADER when it is a real (non-bot) session, else the
// first real-player member; nullptr when no real player is in the group (or no group). This
// is the ONE detection primitive — ResolveDoctrine keys the PlayerParty doctrine on it, the
// STATE producer echoes it (pparty), the bridge Form/Disband guards refuse on it, and the
// escort hook follows it. Bot sessions are identified by WorldSession::GetBot() (the
// PlayerBotEntry every PlayerBotMgr-owned session carries; real clients have none).
Player* AiBotAI::FindPartyBoss() const
{
    if (!me || !me->IsInWorld())
        return nullptr;

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;

    // [SUI] A group member currently DRIVEN by a real player outranks every other
    // candidate — the pack follows the character the human is actually playing,
    // not the human's abandoned (AI-run) body. Full pre-pass so a real leader
    // earlier in iteration order can't shadow a possessed bot later in it.
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (pMember && pMember != me && SuiPossess::GetPossessor(pMember))
            return pMember;
    }

    Player* firstReal = nullptr;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me)
            continue;
        WorldSession* pSess = pMember->GetSession();
        if (!pSess || pSess->GetBot())
            continue;   // a bot session — not a boss
        if (pMember->GetObjectGuid() == pGroup->GetLeaderGuid())
            return pMember;   // the leader is real — unambiguous
        if (!firstReal)
            firstReal = pMember;
    }
    return firstReal;   // leader is a bot but a human is present — escort the human
}

// The human THIS bot keeps formation on (2026-07-08, multi-human split). Null iff the group
// holds no real player — the SAME truth value as FindPartyBoss, so pparty semantics are
// unchanged when the STATE producer keys on this instead. With one human it degenerates to
// him; with several, GUIDLow % count spreads the escort across them deterministically (the
// group's member list is one server-side object — every bot walks the identical order, so
// no sort and no coordination are needed, and the pick never flaps between ticks; it only
// reshuffles when membership actually changes, which is the right time to reshuffle).
Player* AiBotAI::FindEscortBoss() const
{
    if (!me || !me->IsInWorld())
        return nullptr;

    Group* pGroup = me->GetGroup();
    if (!pGroup)
        return nullptr;

    // [SUI] Mirror FindPartyBoss's pre-pass: the group member the human actually
    // DRIVES outranks every real-session candidate. This function feeds the STATE
    // pparty echo and the formation target; without the pre-pass the enrolled own
    // character reads pparty=0 (no OTHER real player in its group) so the brain
    // sends it questing, and every bot keeps formation on the abandoned (AI-run)
    // body instead of the character the human is playing.
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (pMember && pMember != me && SuiPossess::GetPossessor(pMember))
            return pMember;
    }

    std::vector<Player*> reals;
    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* pMember = itr->getSource();
        if (!pMember || pMember == me)
            continue;
        WorldSession* pSess = pMember->GetSession();
        if (!pSess || pSess->GetBot())
            continue;   // a bot session — not a human
        reals.push_back(pMember);
    }

    if (reals.empty())
        return nullptr;

    // [FOLLOW-CMD] Explicit assignment wins: "{bot} follow {player}" stored a lowercased
    // name; if that human is HERE, escort him. Not present (offline / left / typo) → fall
    // through to the auto split below, so a stale override can never strand the bot.
    if (!m_escortOverrideName.empty())
    {
        for (Player* pReal : reals)
        {
            char const* n = pReal->GetName();
            size_t i = 0;
            for (; n[i] && i < m_escortOverrideName.size(); ++i)
                if ((char)tolower((unsigned char)n[i]) != m_escortOverrideName[i])
                    break;
            if (!n[i] && i == m_escortOverrideName.size())
                return pReal;   // full-length case-insensitive match
        }
    }

    return reals[me->GetGUIDLow() % reals.size()];
}

// Keep formation on the boss (called from the escort hook, out of combat, after the engage
// attempt found nothing to fight). Movement is spine-owned, doctrine names targets only.
// [MULTI-HUMAN] "the boss" here = this bot's ASSIGNED human (FindEscortBoss) — with several
// real players in the party the escort splits across them deterministically.
//  - dead boss: stand vigil (his ghost is not a follow target; follow resumes on his rez);
//  - cross-map boss (2026-07-08, instance-follow): dwell AIBOT_PARTY_INSTANCE_DWELL_MS so a
//    portal in-out can't thrash, never while he's taxi-flying or riding a transport (boat/
//    zeppelin — he'll land somewhere; chasing mid-ride teleports bots into the ocean), then
//    TeleportTo his exact position — into his instance OR back out, symmetrically. The far
//    port defers behind a loading screen like any real relocation; the doctrine + pparty stay
//    live throughout. NO ReGroundZ on this dest: it queries the CURRENT map's terrain, which
//    is the wrong map here — the boss is standing on his coords, they're trustworthy;
//  - left far behind on the SAME map (boss took a port): NearTeleportTo the boss, grounded;
//  - otherwise: (re)issue MoveFollow only when the follow generator is not already driving,
//    with a per-guid angle so the escort fans out behind him instead of stacking.
void AiBotAI::DoPartyFollow()
{
    // [MULTI-HUMAN] Formation keys on the ASSIGNED human (FindEscortBoss), not the single
    // detection boss — with two real players the fleet splits ~evenly instead of stacking
    // on one. Catch-up teleport + instance-follow below inherit the same target, so each
    // half of the escort tracks ITS human even when the humans split up.
    // [SUI] Divinity-style chain: an UNLINKED member (broken off from the portrait
    // chain) never formation-follows — it stands where it was left. Combat assist
    // and the player-party stand-down stay live; only the follow leg is severed.
    if (m_suiUnlinked)
        return;

    // A toon the human is commanding from the sky stands where it was sent. Formation-following
    // would drag it back to the party the moment its ordered leg finished, which is the opposite
    // of driving it: you moved it there on purpose.
    if (SuiPossess::IsCommandedFromFreeView(me))
        return;

    Player* pBoss = FindEscortBoss();
    if (!pBoss || !pBoss->IsInWorld() || !pBoss->IsAlive())
        return;

    if (pBoss->GetMapId() != me->GetMapId())
    {
        // [PLAYERPARTY] Instance-follow (2026-07-08): the boss crossed a map boundary
        // (dungeon portal — or a boat/taxi, which we deliberately wait out).
        if (pBoss->HasUnitState(UNIT_STATE_TAXI_FLIGHT) || pBoss->GetTransport())
        {
            m_bossOffMapMs = 0;   // in transit — he'll land; don't chase a moving platform
            return;
        }

        m_bossOffMapMs += AIBOT_UPDATE_INTERVAL;
        if (m_bossOffMapMs < AIBOT_PARTY_INSTANCE_DWELL_MS)
            return;

        m_bossOffMapMs = 0;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-PARTY] %s: boss on map %u (we are on %u) — instance-follow TeleportTo (%.1f, %.1f, %.1f)",
            me->GetName(), pBoss->GetMapId(), me->GetMapId(),
            pBoss->GetPositionX(), pBoss->GetPositionY(), pBoss->GetPositionZ());
        me->TeleportTo(pBoss->GetMapId(),
            pBoss->GetPositionX(), pBoss->GetPositionY(), pBoss->GetPositionZ(),
            me->GetOrientation());
        return;
    }

    m_bossOffMapMs = 0;   // same map — a fresh crossing starts a fresh dwell

    float const dist = me->GetDistance(pBoss);

    if (dist > AIBOT_PARTY_CATCHUP_TELEPORT)
    {
        float bx = pBoss->GetPositionX();
        float by = pBoss->GetPositionY();
        float bz = pBoss->GetPositionZ();
        ReGroundZ(bx, by, bz, "party-catchup");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-PARTY] %s: left %.0fyd behind the boss — catch-up teleport to (%.1f, %.1f, %.1f)",
            me->GetName(), dist, bx, by, bz);
        me->NearTeleportTo(bx, by, bz, me->GetOrientation());
        return;
    }

    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE &&
        dist > AIBOT_PARTY_FOLLOW_DIST + 1.0f)
    {
        // Deterministic per-bot spread behind the boss: 8 slots around the rear arc.
        float const angle = M_PI_F + (float(me->GetGUIDLow() % 8) - 3.5f) * (M_PI_F / 8.0f);
        me->GetMotionMaster()->MoveFollow(pBoss, AIBOT_PARTY_FOLLOW_DIST, angle);
    }
}

// ============================================================
// MAIN UPDATE LOOP — COMPLETE-METHOD REPLACEMENT (2026-07-05, survival pass fixes 1+2)
//
// Identical to the deployed doctrine/self-unstick version EXCEPT:
//
//   [EAT-HYST]  The OOC eat gate is now a hysteresis LATCH. Old behaviour: with an active
//               task, DrinkAndEat was reachable only below 40% HP / 20% mana — the moment
//               HP crossed 41% the condition went false and the grind dispatch re-pulled,
//               so a tasked bot lived permanently in the 40-60% band (casters re-pulled at
//               21% mana). New: dipping below the floor latches recovery ON; the latch
//               releases only at 90% HP / 85% mana (the proven RezHealTarget shape).
//
//   [PULLGATE]  Solo fight-initiation floor at BOTH engage sites (grind dispatch +
//               approach scan): never START a fight below 70% HP / 50% mana — latch
//               recovery and stand down instead of patrolling INTO the field. Defense,
//               in-progress fights, and the TeamAuto/Directed doctrines are untouched
//               (the group has its own recovery protocols: GroupDefend / guard-heal /
//               the chain 40/40 gate). Recovery (~10-30s) is well inside the 120s
//               no-kill deadline, so no false shelves.
//
// The kobold-camp self-unstick, doctrine dispatch, [GRAVE-SELFREZ], and §4 blocks are
// carried verbatim from the deployed build.
// ============================================================
// Bridge connect/recv/state/flush on the 1 Hz cadence. Factored out of UpdateAI
// so the possessed short-circuit keeps the bridge (and the C# brain's view of
// this bot) alive while every behaviour tick is suspended.
void AiBotAI::UpdateBridgeTick()
{
    // [SUI] An enrolled REAL character never talks to the brain. The brain
    // auto-registers every HELLO into characters.playerbot for restart
    // persistence -- which adopted a real character into the fleet, respawned
    // it on a synthetic account after a restart, and stole it from its owner
    // (Tesfff, 2026-08-10). Doctrine + explicit CMSG_SUI_ORDER injections are
    // the whole control surface for an unattended real character.
    if (m_ownedDummyEntry)
        return;

    if (!m_bridgeConnected)
    {
        if (m_bridgeReconnectTimer <= AIBOT_UPDATE_INTERVAL)
        {
            m_bridgeReconnectTimer = 0;
            BridgeConnect();
        }
        else
        {
            m_bridgeReconnectTimer -= AIBOT_UPDATE_INTERVAL;
        }
    }

    if (m_bridgeConnected)
    {
        if (!m_bridgeHelloSent && m_initialized)
            BridgeSendHello();

        BridgeRecv();

        if (m_bridgeHelloSent)
        {
            if (m_bridgeStateTimer <= AIBOT_UPDATE_INTERVAL)
            {
                m_bridgeStateTimer = BRIDGE_STATE_INTERVAL;
                BridgeSendState();
            }
            else
            {
                m_bridgeStateTimer -= AIBOT_UPDATE_INTERVAL;
            }
        }

        BridgeFlush();   // Session 36: drain bytes deferred by a prior partial/blocked write
    }
}

// SUI possession toggle (SuiPossess.cpp). On: freeze in place — the human's
// client is about to take over the mover; any in-flight spline or task motion
// would fight it. Off: resume on a fresh 1s tick so the accumulated diff of a
// long possession doesn't fire one giant catch-up tick, and let RefreshDoctrine
// re-resolve naturally on that tick.
void AiBotAI::SetPossessed(bool on)
{
    if (m_possessed == on)
        return;
    m_possessed = on;
    if (on)
    {
        if (me)
        {
            StopMoving();
            me->GetMotionMaster()->MoveIdle();
        }
    }
    else
    {
        m_updateTimer.Reset(1000);
    }
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[AIBOT] %s possession %s",
        me ? me->GetName() : "<no player>", on ? "BEGIN" : "END");
}

void AiBotAI::UpdateAI(uint32 const diff)
{
    // Handle pending teleports from base class
    PlayerBotAI::UpdateAI(diff);

    // [ROTATION] Combat sub-tick (2026-07-16): with a slate loaded, evaluate it at 4 Hz
    // AHEAD of the 1s behaviour gate — the 1 Hz loop can't weave a GCD, and its wand
    // autorepeat early-return silences a wanding bot's spell evaluation entirely. This
    // runs ONLY the cast attempt; every behaviour decision (tasks, doctrine, bridge,
    // loot, kill detection) stays on the 1s tick below. Slate-less bots skip in one
    // branch — vanilla cadence bit-for-bit.
    m_rotationSubTick.Update(diff);
    if (m_rotationSubTick.Passed())
    {
        m_rotationSubTick.Reset(AIBOT_ROTATION_SUBTICK_MS);
        if (!m_possessed
            && !m_rotation.empty() && me && me->IsInWorld() && !me->IsBeingTeleported()
            && me->IsAlive() && me->IsInCombat()
            && !me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL)
            && !me->IsNonMeleeSpellCasted(false, false, true))
            UpdateRotationSlate();
    }

    // [TRACE] Movement trace sub-tick (2026-07-20) — the SYMPTOM half of the fly
    // instrumentation. Placed here for the same reason as the rotation sub-tick above: it must
    // run AHEAD of the 1s behaviour gate, because 1 Hz is ~7yd of travel per sample at run speed
    // — far too coarse to resolve a float that opens and closes inside one fillet arc. It
    // self-throttles to AIBOT_TRACE_SAMPLE_MS internally and returns on the first branch unless
    // this bot's name is in run/bin/aibot_trace.txt, so an unarmed fleet pays one string compare
    // per bot per 250ms and emits nothing. Pairs with the dispatch-time [AIBOT-TRACE] WP/PATH
    // lines from GroundPathPoints — cause and symptom land on one timeline in Server.log.
    if (!m_possessed)
        UpdateMovementTrace(diff);

    m_updateTimer.Update(diff);
    if (m_updateTimer.Passed())
        m_updateTimer.Reset(AIBOT_UPDATE_INTERVAL);
    else
        return;

    if (!me->IsInWorld() || me->IsBeingTeleported())
        return;

    // [SUI] A real player is driving this body (SuiPossess). Every autonomous
    // behaviour — tasks, doctrine, combat, loot, wander — is suspended; only the
    // bridge stays alive so the C# brain keeps seeing STATE (possessed:1) and
    // stands down. SetPossessed(false) resumes on a fresh 1s tick.
    // Commanded from the free view is the exception: autonomy still must not choose goals,
    // but the tick has to RUN or the ordered task has nothing to walk it. The doctrine that
    // results is PlayerParty (the possessor's own body resolves as boss), which is exactly the
    // wanted shape — no grind, no patrol, no wander, combat assist live, and the task machinery
    // reachable for the MOVE_TO the order just queued. DoPartyFollow is separately suppressed
    // for this bot so it holds the ground you sent it to instead of walking back to the party.
    if (m_possessed && !SuiPossess::IsCommandedFromFreeView(me))
    {
        UpdateBridgeTick();
        return;
    }

    // Decrement wander/patrol timer
    if (m_wanderTimer > AIBOT_UPDATE_INTERVAL)
        m_wanderTimer -= AIBOT_UPDATE_INTERVAL;
    else
        m_wanderTimer = 0;

    // §4 approach-scan throttle
    if (m_approachScanTimer > AIBOT_UPDATE_INTERVAL)
        m_approachScanTimer -= AIBOT_UPDATE_INTERVAL;
    else
        m_approachScanTimer = 0;

    // [ADDED] Combat-stalemate ignore set: tick down per-guid cooldowns
    for (auto it = m_combatIgnore.begin(); it != m_combatIgnore.end(); )
    {
        if (it->second <= AIBOT_UPDATE_INTERVAL)
            it = m_combatIgnore.erase(it);
        else { it->second -= AIBOT_UPDATE_INTERVAL; ++it; }
    }

    // [DOCTRINE] Resolve which engagement doctrine governs this tick (Solo / TeamAuto / Directed)
    // and swap on change, before any acquisition or combat decision below consults m_doctrine.
    RefreshDoctrine();

    // [BG-ACCEPT] Auto-accept a pending Battleground invitation. The packet-level
    // accept (SMSG_BATTLEFIELD_STATUS) is already detected in
    // CombatBotBaseAI::OnPacketReceived and sets m_receivedBgInvite = true. The
    // helper CombatBotBaseAI::SendBattlefieldPortPacket() walks every queue
    // type, finds the one this bot is invited to, and calls
    // HandleBattleFieldPortOpcode with action=1 (port in). BattleBotAI and
    // PartyBotAI both call this from their UpdateAI — AiBotAI was the lone
    // holdout. Without this, the bot stays on the base WSG/AB/AV map while
    // its group leader enters the BG instance. Config-gated so ops can turn
    // it off for solo content.
    if (sWorld.getConfig(CONFIG_BOOL_AI_BOT_AUTO_ACCEPT_BG) && m_receivedBgInvite)
    {
        if (!me->InBattleGround() && !me->IsBeingTeleported())
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-BG] %s: BG invite detected — accepting and porting in",
                me->GetName());
            SendBattlefieldPortPacket();
            m_receivedBgInvite = false;
        }
    }

    // --- Bridge: connect + recv + periodic state ---
    UpdateBridgeTick();

    // One-time log on first successful update tick
    if (!m_loggedFirstUpdate)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] %s (guid %u) UpdateAI active - class %u, level %u, zone %u, map %u",
            me->GetName(), me->GetGUIDLow(), me->GetClass(), me->GetLevel(),
            me->GetZoneId(), me->GetMapId());
        m_loggedFirstUpdate = true;
        me->SetSaveDisabled(false);
        me->SaveToDB();
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] %s (guid %u) saved to DB", me->GetName(), me->GetGUIDLow());
    }

    // --- Initialization (once, on first update after login) ---
    if (!m_initialized)
    {
        if (m_freshSpawn)
        {
            LearnPremadeSpecForClass();
            // Only give starting gear, not premade BG gear
            AutoEquipGear(PLAYER_BOT_AUTO_EQUIP_STARTING_GEAR);
        }

        if (m_role == ROLE_INVALID)
            AutoAssignRole();

        ResetSpellData();
        PopulateSpellData();
        AddAllSpellReagents();
        me->UpdateSkillsToMaxSkillsForLevel();
        me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SPAWNING);
        SummonPetIfNeeded();
        me->SetHealthPercent(100.0f);
        me->SetPowerPercent(me->GetPowerType(), 100.0f);

        if (urand(0, 1))
        {
            me->ToggleFlag(PLAYER_FLAGS, PLAYER_FLAGS_HIDE_HELM);
            me->ToggleFlag(PLAYER_FLAGS, PLAYER_FLAGS_HIDE_CLOAK);
        }

        uint32 newzone, newarea;
        me->GetZoneAndAreaId(newzone, newarea);
        me->UpdateZone(newzone, newarea);

        // Join zone-specific General and Trade channels so bots can hear channel chat
        // Channel names are zone-dependent: "General - Elwynn Forest", "Trade - Stormwind City", etc.
        if (me->GetSession())
        {
            const AreaEntry* zoneEntry = AreaEntry::GetById(newzone);
            if (zoneEntry && zoneEntry->Name)
            {
                std::string zoneName = zoneEntry->Name;

                WorldPackets::Channel::JoinChannel joinGeneral;
                joinGeneral.channelName = "General - " + zoneName;
                joinGeneral.channelPassword = "";
                me->GetSession()->HandleJoinChannelOpcode(joinGeneral);

                WorldPackets::Channel::JoinChannel joinTrade;
                joinTrade.channelName = "Trade - " + zoneName;
                joinTrade.channelPassword = "";
                me->GetSession()->HandleJoinChannelOpcode(joinTrade);

                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] %s joined General and Trade channels for zone %s",
                    me->GetName(), zoneName.c_str());
            }
            else
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] %s could not resolve zone name for zone %u, skipping channel join",
                    me->GetName(), newzone);
            }
        }

        m_initialized = true;
        m_lastKnownLevel = me->GetLevel();
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] %s initialized - class %u, level %u, role %u",
            me->GetName(), me->GetClass(), me->GetLevel(), m_role);
        return;
    }

    // --- Death handling: ghost at corpse, wait for C# RESURRECT (or self-rez at a graveyard) ---
    if (me->IsDead())
    {
        if (!m_wasDead)
        {
            m_wasDead = true;

            float deathX = me->GetPositionX();
            float deathY = me->GetPositionY();
            float deathZ = me->GetPositionZ();
            uint32 deathMap = me->GetMapId();

            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT] %s died at (%.1f, %.1f, %.1f) map=%u — ghosting at corpse, waiting for C# RESURRECT",
                me->GetName(), deathX, deathY, deathZ, deathMap);

            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
                StopMoving();

            m_currentTask.Clear();

            // Clean up any existing corpse from a previous death
            if (Corpse* oldCorpse = me->GetCorpse())
                me->SpawnCorpseBones();

            // Ghost form — bot becomes translucent spirit, corpse drops
            me->BuildPlayerRepop();
            // NO RepopAtGraveyard — ghost stays right here at the death position

            char deathData[160];
            snprintf(deathData, sizeof(deathData),
                "x=%.1f|y=%.1f|z=%.1f|map=%u|attackers=%u",
                deathX, deathY, deathZ, deathMap, m_lastAttackerCount);
            BridgeSendEvent("DEATH", deathData);

            return;
        }

        // [GRAVE-SELFREZ] Graveyard self-rez — the firing half of the zone-0 escape.
        if (m_pendingGraveyardRez)
        {
            if (m_graveRezWaitMs > AIBOT_UPDATE_INTERVAL)
                m_graveRezWaitMs -= AIBOT_UPDATE_INTERVAL;
            else
                m_graveRezWaitMs = 0;

            float distToGrave = me->GetDistance2d(m_graveRezX, m_graveRezY);
            bool landed   = (me->GetMapId() == m_graveRezMap) && (distToGrave < 25.0f);
            bool timedOut = (m_graveRezWaitMs == 0);

            if (landed || timedOut)
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT] %s graveyard self-rez %s at (%.1f, %.1f) (dist=%.1f)",
                    me->GetName(), landed ? "confirmed" : "TIMEOUT",
                    me->GetPositionX(), me->GetPositionY(), distToGrave);

                m_pendingGraveyardRez = false;
                m_graveRezWaitMs = 0;

                me->ResurrectPlayer(0.5f);
                me->CombatStop(true);   // never resume rez in-combat (a ghost pulled nothing anyway)
                me->SpawnCorpseBones();

                BridgeSendEvent("RESPAWN", "");
                BridgeSendState();
                return;
            }

            return;
        }

        // Subsequent ticks while dead: keep bridge alive, wait for RESURRECT.
        return;
    }
    else
    {
        if (m_wasDead)
        {
            m_wasDead = false;
            SummonPetIfNeeded();
            return;
        }
    }

    // [OVERPULL] Peak melee attackers this combat — stamped on the DEATH event above.
    if (me->IsInCombat())
    {
        uint32 atk = (uint32)me->GetAttackers().size();
        if (atk > m_lastAttackerCount)
            m_lastAttackerCount = atk;
    }
    else
        m_lastAttackerCount = 0;


    // --- TASK_TAXI: in-flight — skip ALL behavior until we land ---
    if (m_currentTask.type == TASK_TAXI)
    {
        if (me->GetTaxi().empty() && !me->HasUnitState(UNIT_STATE_TAXI_FLIGHT))
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT] %s: flight complete (arrived at node %u)",
                me->GetName(), m_currentTask.taxiDestNode);
            BridgeSendEvent("FLIGHT_COMPLETE", "");
            m_currentTask.Clear();
            // Don't return — fall through to normal idle behavior this tick
        }
        else
        {
            return; // still flying — don't wander, fight, eat, buff, etc.
        }
    }

    // --- Level-up detection ---
    if (m_lastKnownLevel > 0 && me->GetLevel() > m_lastKnownLevel)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] %s leveled up: %u -> %u",
            me->GetName(), m_lastKnownLevel, me->GetLevel());
        SendLevelUpEvent(me->GetLevel());
        m_lastKnownLevel = me->GetLevel();

        // Re-learn spells and update skills for new level
        PopulateSpellData();
        me->UpdateSkillsToMaxSkillsForLevel();
    }

    // --- Auto-loot timer ---
    if (m_lootTimer > 0)
    {
        m_lootTimer -= (int32)AIBOT_UPDATE_INTERVAL;
        if (m_lootTimer <= 0)
        {
            m_lootTimer = 0;
            if (!m_lootTargetGuid.IsEmpty())
            {
                DoAutoLoot(m_lootTargetGuid);
                m_lootTargetGuid.Clear();
            }
        }
    }

    // --- CC break ---
    if (me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
    {
        BreakCrowdControlEffects();
        return;
    }

    // --- Auto-repeat spell handling (Hunter auto shot) ---
    if (me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
    {
        if (!me->GetVictim())
            me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);
        else if (me->GetClass() == CLASS_HUNTER)
        {
            if (me->GetCombatDistance(me->GetVictim()) < 8.0f)
                me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);
            else
                UpdateInCombatAI_Hunter();
        }

        return;
    }

    if (me->IsNonMeleeSpellCasted(false, false, true))
        return;

    if (me->GetTargetGuid() == me->GetObjectGuid())
        me->ClearTarget();

    Unit* pVictim = me->GetVictim();

    // Prevent chasing stealthed target
    if (pVictim && !pVictim->IsVisibleForOrDetect(me, me, false))
    {
        me->AttackStop();
        me->ClearTarget();
        me->StopMoving();
        if (pVictim = SelectAttackTarget(pVictim))
            AttackStart(pVictim);
        return;
    }

    // --- Out of combat: eat/drink — [EAT-HYST] hysteresis latch (2026-07-05) ---
    // OLD: one-way gate — with an active task DrinkAndEat was reachable only below 40% HP /
    // 20% mana, so the loop RELEASED the bot back into the grind at 41%/21%: a tasked bot
    // lived permanently in the fight-losing band, every objective, every camp.
    // NEW: dipping below the floor latches recovery ON; the latch releases only at 90% HP /
    // 85% mana (the shape the C# rez heal proved at 0.95/0.85). The food aura breaks on
    // aggro but the LATCH survives combat, so a mid-eat defense fight resumes eating
    // afterward instead of chaining into the next pull half-dead.
    if (!me->IsInCombat())
    {
        bool hasActiveTask = (m_currentTask.type == TASK_MOVE_TO ||
                              m_currentTask.type == TASK_GRIND);
        bool const manaUser = (me->GetPowerType() == POWER_MANA);
        float const hp = me->GetHealthPercent();
        float const mp = manaUser ? me->GetPowerPercent(POWER_MANA) : 100.0f;

        if (hp < AIBOT_EAT_ENTER_HP || (manaUser && mp < AIBOT_EAT_ENTER_MANA))
            m_eatRecoveryLatch = true;

        if (m_eatRecoveryLatch && hp >= AIBOT_EAT_EXIT_HP && mp >= AIBOT_EAT_EXIT_MANA)
            m_eatRecoveryLatch = false;

        if (!hasActiveTask || m_eatRecoveryLatch)
        {
            if (DrinkAndEat())
                return;
        }
    }

    if (me->GetStandState() != UNIT_STAND_STATE_STAND)
        me->SetStandState(UNIT_STAND_STATE_STAND);

    if (me->GetSheath() == SHEATH_STATE_UNARMED && !me->IsMounted())
        me->SetSheath(SHEATH_STATE_MELEE);

    // --- Out of combat behavior ---
    if (!me->IsInCombat())
    {
        // [PULL] Out of combat = no pull in flight. Clear any stale pull state so it can never
        // linger across an OOC gap into an unrelated defense fight. BeginPull (below, in the
        // TASK_GRIND engage) re-sets it later THIS tick when we actually pull, then returns.
        m_pullActive = false;

        if (CheckForUnreachableTarget())
            return;

        UpdateOutOfCombatAI();

        if (m_isBuffing)
            return;

        // Can enter combat from UpdateOutOfCombatAI().
        if (me->IsInCombat())
            return;

        if (me->IsNonMeleeSpellCasted())
            return;


        // --- Kill detection (must run before combat/OOC branching) ---
        if (!pVictim && m_lastVictimEntry != 0)
        {
            // Victim pointer cleared = mob died or despawned. Fire kill event.
            // --- Tapped check: only process kills we actually tagged ---
            Creature* pKillCreature = me->GetMap()->GetCreature(ObjectGuid(HIGHGUID_UNIT, m_lastVictimEntry, m_lastVictimGuidLow));
            if (pKillCreature && pKillCreature->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_TAPPED) && !pKillCreature->IsTappedBy(me))
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT] %s: mob (entry=%u guid=%u) tapped by another — skipping kill credit",
                    me->GetName(), m_lastVictimEntry, m_lastVictimGuidLow);
                m_lastVictimEntry = 0;
                m_lastVictimGuidLow = 0;
            }
            else
            {
            SendKillEvent(m_lastVictimEntry, m_lastVictimGuidLow);
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT] %s: kill detected (entry=%u guid=%u)",
                me->GetName(), m_lastVictimEntry, m_lastVictimGuidLow);

            // Queue auto-loot with humanization delay
            Creature* victim = me->GetMap()->GetCreature(ObjectGuid(HIGHGUID_UNIT, m_lastVictimEntry, m_lastVictimGuidLow));
            if (victim && victim->IsDead())
            {
                m_lootTargetGuid = victim->GetObjectGuid();
                m_lootTimer = urand(1000, 2500);
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-LOOT] %s: loot timer started (%dms) for %s (entry=%u)",
                    me->GetName(), m_lootTimer, victim->GetName(), m_lastVictimEntry);
            }
            else
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-LOOT] %s: could not find dead creature for loot (entry=%u guid=%u)",
                    me->GetName(), m_lastVictimEntry, m_lastVictimGuidLow);
            }

            if (m_currentTask.type == TASK_GRIND)
            {
                // wolf-meat fix (2026-06-30): MatchesObjectiveEntry checks the primary
                // dispatched creatureEntry OR any tied item-drop alternate, not exact
                // equality alone — so a kill on a tied local sibling (e.g. Timber Wolf
                // when the dispatched entry was Young Wolf) still advances THIS leg's
                // killCount instead of silently not counting toward the objective.
                bool matches = (m_currentTask.creatureEntry == 0 ||
                                m_currentTask.MatchesObjectiveEntry(m_lastVictimEntry));
                if (matches)
                {
                    m_currentTask.killCount++;
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT] %s: GRIND kill %d/%d (entry=%u)",
                        me->GetName(), m_currentTask.killCount, m_currentTask.killGoal, m_lastVictimEntry);
                    if (m_currentTask.killGoal > 0 &&
                        m_currentTask.killCount >= m_currentTask.killGoal)
                    {
                        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                            "[AIBOT] %s: GRIND task complete (%d/%d kills)",
                            me->GetName(), m_currentTask.killCount, m_currentTask.killGoal);
                        BridgeSendEvent("TASK_COMPLETE", "GRIND finished");
                        m_currentTask.Clear();
                    }
                }
            }
            m_lastVictimEntry = 0;
            m_lastVictimGuidLow = 0;
            }
        }
        else if (pVictim && pVictim->IsCreature())
        {
            m_lastVictimEntry = static_cast<Creature*>(pVictim)->GetEntry();
            m_lastVictimGuidLow = pVictim->GetGUIDLow();
        }

        // ── [PLAYERPARTY] Escort mode (2026-07-07): a REAL player leads this group ──
        // The human is the coordinator; C++ owns the whole behaviour. Engage the doctrine's
        // party focus (boss's victim → boss's attacker → a party member's attacker → sticky;
        // NEVER a self-initiated pull), else keep formation on the boss. The return makes the
        // task machinery unreachable while escorting — no grind dispatch, no MOVE_TO resume,
        // no patrol, no wander — so a stale C# task from before the invite simply idles out
        // (C# stands down to Goal.Idle off the pparty STATE echo and SET_TASK IDLEs anyway).
        // Placed AFTER kill-detection so escort kills still fire KILL events + loot timers,
        // and AFTER the [EAT-HYST] block so a companion still sits to eat between fights.
        if (m_doctrineKind == DoctrineKind::PlayerParty)
        {
            if (Unit* pEscortTarget = m_doctrine->AcquireTarget(*this))
            {
                if (AttackStart(pEscortTarget))
                    return;
            }

            // [HUB-ERRAND] YIELD (2026-07-08 §3): an active MOVE_TO while in a human's party
            // is by construction a deliberate C#-issued errand leg (the GoalSelector's
            // player-party hold stands everything else down; C# never sends otherwise), so
            // fall THROUGH to the task machinery below and let the leg walk/resume/arrive
            // exactly like a solo MOVE_TO. Doctrine stays PlayerParty throughout — a mob
            // jumping the bot at the vendor still gets the full escort ladder above. Every
            // OTHER task type keeps the pre-yield stand-down (a stale pre-invite GRIND still
            // idles out as before). Known cosmetic: 1-2 ticks of turn-toward-boss between
            // errand legs (the task clears on TASK_COMPLETE, follow resumes until the next
            // command lands).
            // [SUI-DIAG] Why is this member not forming up? Every "nobody follows me" report
            // ends at one of three answers and none of them were observable: a stale
            // TASK_MOVE_TO diverts past the follow call entirely, an unlinked member stands its
            // ground by design, or the boss simply did not resolve. Throttled to 5s per bot,
            // and only reachable in PlayerParty doctrine, so it is bounded to the human party
            // rather than the whole fleet.
            if (m_suiFollowDiagTimer > AIBOT_UPDATE_INTERVAL)
                m_suiFollowDiagTimer -= AIBOT_UPDATE_INTERVAL;
            else
            {
                m_suiFollowDiagTimer = 5000;
                Player* pDiagBoss = FindEscortBoss();
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[SUI-FOLLOW] %s: task=%u unlinked=%u commanded=%u boss=%s dist=%.1f",
                    me->GetName(), uint32(m_currentTask.type), m_suiUnlinked ? 1u : 0u,
                    SuiPossess::IsCommandedFromFreeView(me) ? 1u : 0u,
                    pDiagBoss ? pDiagBoss->GetName() : "(none)",
                    pDiagBoss ? me->GetDistance(pDiagBoss) : -1.0f);
            }

            if (m_currentTask.type != TASK_MOVE_TO)
            {
                DoPartyFollow();
                return;
            }
            // fall through — the TASK_MOVE_TO resume/arrival machinery below drives the leg
        }

        // --- TASK_GRIND: proactive pull or patrol ---
        if (m_currentTask.type == TASK_GRIND)
        {
            // [PULLGATE] Fight-initiation floor (solo, 2026-07-05). Never START a fight
            // under-resourced: below the floor, latch recovery and stand down — and do NOT
            // patrol (DoGrindPatrol random-hops INTO the field, i.e. body-pulling at low
            // HP — the exact death class this closes). The eat block above owns every
            // subsequent tick until the latch releases at 90/85; recovery (~10-30s) is well
            // inside the C# 120s no-kill deadline, so no false shelves. Placed BEFORE
            // AcquireTarget so neither the pull nor the freeze/self-unstick escalation can
            // run while weak. TeamAuto/Directed pass through untouched — the group owns its
            // own recovery (GroupDefend / guard-heal / the chain 40/40 gate).
            if (m_doctrineKind == DoctrineKind::Solo && !PullReady())
            {
                if (!m_eatRecoveryLatch)
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT-PULLGATE] %s: refusing new pull at hp=%.0f%% mp=%.0f%% — recovering to %.0f/%.0f",
                        me->GetName(), me->GetHealthPercent(),
                        me->GetPowerType() == POWER_MANA ? me->GetPowerPercent(POWER_MANA) : 100.0f,
                        AIBOT_EAT_EXIT_HP, AIBOT_EAT_EXIT_MANA);
                m_eatRecoveryLatch = true;
                return;
            }

            // [SPREAD] Anti-convergence (FINDING_005): don't add to a dogpile. For a SOLO FILLER
            // grind (entry==0), if AIBOT_SPREAD_BOT_CAP+ other independent bots are already grinding
            // within AIBOT_SPREAD_RADIUS, patrol out instead of engaging — landing no kills here lets
            // the C# no-progress breaker relocate us to a less-crowded camp, so the fleet spreads
            // instead of 8 bots piling one camp into a 48-attacker overpull. Quests (shared mobs) and
            // groups (meant to stack + focus-fire) are exempt; the grid-search runs only for the solo
            // filler case. m_spreadDeferred logs the defer once per episode, not every tick.
            uint32 const nearbyBots =
                (m_doctrineKind == DoctrineKind::Solo && m_currentTask.creatureEntry == 0 && !me->GetGroup())
                ? CountNearbyBots(me, AIBOT_SPREAD_RADIUS) : 0;
            if (nearbyBots >= AIBOT_SPREAD_BOT_CAP)
            {
                if (!m_spreadDeferred)
                {
                    m_spreadDeferred = true;
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT-SPREAD] %s: %u other bots grinding within %.0fyd (cap %u) — patrolling out to spread",
                        me->GetName(), nearbyBots, AIBOT_SPREAD_RADIUS, AIBOT_SPREAD_BOT_CAP);
                }
                DoGrindPatrol();
                return;
            }
            m_spreadDeferred = false;

            // [DOCTRINE] Acquisition + pull discipline are the doctrine's; the freeze / GRIND_BLOCKED
            // / patrol bookkeeping stays here in the spine. AcquireTarget returns the mob to pull, or
            // nullptr to hold/patrol. A nullptr that is a DELIBERATE TeamAuto wait-for-anchor hold (B3
            // — a follower letting the anchor pull first) is flagged by HoldingForTeam(): patrol
            // WITHOUT a freeze tick, so a hold can never bump m_grindFreezeStreak or fire a false
            // GRIND_BLOCKED|no_target. Solo returns its priority scan; the anchor / filler-detour /
            // dwell-expired follower all fall through to a real pull. (The whole group-fight decision
            // — resolver-first, the B3 dwell + its counter, sticky — now lives in AiBotDoctrineTeam.)
            if (Unit* pGrindTarget = m_doctrine->AcquireTarget(*this))
            {
                if (m_doctrine->HoldPull(*this, pGrindTarget))
                {
                    ++m_grindFreezeStreak;
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT-OVERPULL] %s: holding (%u/%u) — target %s in %u-deep cluster (solo cap %u) [will self-unstick]",
                        me->GetName(), m_grindFreezeStreak, AIBOT_GRIND_FREEZE_DWELL,
                        pGrindTarget->GetName(),
                        CountNearbyHostiles(pGrindTarget, AIBOT_PULL_DENSITY_RADIUS), AIBOT_OVERPULL_SOLO);

                    if (m_grindFreezeStreak >= AIBOT_GRIND_FREEZE_DWELL)
                    {
                        m_grindFreezeStreak = 0;

                        // Self-unstick: the objective grind USED to hand back GRIND_BLOCKED|overpull_dwell
                        // so C# could "resync + re-derive" — but the brain ALWAYS answered with a forced
                        // single-kill detour + re-issue of the SAME objective, re-freezing on the SAME
                        // dense field (the kobold-camp livelock). A dense field of QUEST mobs is exactly
                        // where we want to grind, so take the pull right here: HoldPull handed us the
                        // least-clustered candidate, HandleOverpullRetreat is the in-combat backstop if too
                        // many pile on, and a field that genuinely can't be killed still trips the C# 120s
                        // no-kill deadline → durable shelve.
                        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                            "[AIBOT-OVERPULL] %s: freeze-escape — self-unstick pull of %s (bypassing cap)",
                            me->GetName(), pGrindTarget->GetName());
                        AttackStart(pGrindTarget);
                        return;
                    }

                    DoGrindPatrol();
                    return;
                }

                // Pull cleared the cap → engage with a real PULL (FINDING_005): tag it, then drag
                // it back to open ground so the camp's neighbours don't dogpile. BeginPull owns the
                // approach + retreat; HandlePullRetreat drives it on the in-combat ticks that follow.
                m_grindFreezeStreak = 0;
                BeginPull(pGrindTarget);
                return;
            }

            // AcquireTarget returned nullptr. If that was a deliberate wait-for-anchor hold (B3),
            // patrol and DO NOT count a freeze — the doctrine is choosing to wait, not stuck.
            if (m_doctrine->HoldingForTeam())
            {
                DoGrindPatrol();
                return;
            }

            // Genuinely no valid target this tick. Only an OBJECTIVE grind (entry!=0) hands back (quest
            // mobs all dead/tapped → C# resyncs + detours). An entry==0 grind just patrols: a filler with
            // no mobs is GrindPlanner's no-kills→reselect job, and a detour rides its own WAIT deadline.
            if (m_currentTask.creatureEntry != 0)
            {
                if (++m_grindFreezeStreak >= AIBOT_GRIND_FREEZE_DWELL)
                {
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT-OVERPULL] %s: freeze-escape — no valid target %u ticks, signaling GRIND_BLOCKED",
                        me->GetName(), m_grindFreezeStreak);
                    m_grindFreezeStreak = 0;
                    char ev[160];
                    snprintf(ev, sizeof(ev),
                        "x=%.1f|y=%.1f|z=%.1f|entry=%u|reason=no_target",
                        m_currentTask.x, m_currentTask.y, m_currentTask.z,
                        m_currentTask.creatureEntry);
                    BridgeSendEvent("GRIND_BLOCKED", ev);
                }
            }

            DoGrindPatrol();
            return;
        }

       // --- TASK_MOVE_TO: resume movement after interruption ---
        if (m_currentTask.type == TASK_MOVE_TO)
        {
            // ── §4 approach scan ──
            if (m_currentTask.creatureEntry != 0 && m_approachScanTimer == 0)
            {
                m_approachScanTimer = urand(2000, 3000);
                if (Unit* pMob = ScanApproachTarget())
                {
                    // [PULLGATE] (solo, 2026-07-05) Arriving at the field under-resourced:
                    // the scan just proved live mobs are AHEAD, so stop HERE — still outside
                    // the camp — latch recovery, and eat before the engage. The MOVE_TO
                    // resume logic below picks the journey back up once the latch releases,
                    // the scan re-fires at full HP, and the engage proceeds as normal.
                    if (m_doctrineKind == DoctrineKind::Solo && !PullReady())
                    {
                        if (!m_eatRecoveryLatch)
                            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                                "[AIBOT-PULLGATE] %s: field ahead (%s, %.0fyd) but hp=%.0f%% mp=%.0f%% — stopping to recover first",
                                me->GetName(), pMob->GetName(), me->GetDistance(pMob),
                                me->GetHealthPercent(),
                                me->GetPowerType() == POWER_MANA ? me->GetPowerPercent(POWER_MANA) : 100.0f);
                        StopMoving();
                        m_eatRecoveryLatch = true;
                        return;
                    }

                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT-PATH] %s: approach scan hit %s (entry=%u, %.0fyd) — handoff to GRIND",
                        me->GetName(), pMob->GetName(), m_currentTask.creatureEntry, me->GetDistance(pMob));
                    ConvertMoveToGrindInPlace();

                    // [DOCTRINE] The scan hit is the OTHER "group arrives at a camp" moment. A
                    // TeamAuto follower piles the ANCHOR'S resolved victim if there is one (via the
                    // doctrine's target authority), or HOLDS if the anchor isn't fighting yet — the
                    // grind dispatch's wait-for-anchor owns the wait from the very next tick. Solo /
                    // anchor: engage the scan hit under the pull veto, byte-for-byte the old engage.
                    Unit* pEngage = pMob;
                    if (m_combatDirective.IsActive() &&
                        m_combatDirective.anchorGuidLow != me->GetGUIDLow())
                    {
                        if (Unit* pAssist = m_doctrine->MaintainTarget(*this, nullptr))
                            pEngage = pAssist;
                        else
                            return;   // follower, anchor not fighting yet → hold (grind gate next tick)
                    }

                    // [OVERPULL] Same veto as the grind dispatch: convert to a local grind but
                    // don't dive a dense pack solo. The grind dispatch re-gates next tick.
                    if (!m_doctrine->HoldPull(*this, pEngage))
                        AttackStart(pEngage);
                    return;
                }
            }

            // Still actively walking — let the motion generator finish
            if (me->IsMoving())
                return;

            // Not moving. Either we arrived or we got interrupted.
            float dist = me->GetDistance2d(m_currentTask.x, m_currentTask.y);

            if (dist > 3.0f)
            {
                // Haven't arrived — resume movement.
                if (!m_pathWaypoints.empty() &&
                    m_pathIndex < (uint32)m_pathWaypoints.size() - 1)
                {
                    // Resume the current leg's remaining chunks
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT-PATH] %s: resuming chunked path from waypoint %u/%u",
                        me->GetName(), m_pathIndex, (uint32)m_pathWaypoints.size());
                    StartNextPathChunk();
                }
                else
                {
                    // Current leg exhausted (or interrupted with no stored path) —
                    // compute the next leg toward the true dest and walk it.
                    MoveToDestination(m_currentTask.x, m_currentTask.y, m_currentTask.z);
                }
            }
            else
            {
                // Close enough to the destination.
                // §4: an enriched objective MOVE_TO arrived at the deep coord with no
                // scan hit — grind here in place rather than emit a false "arrived"
                // TASK_COMPLETE (merged step: that would mean objective-done, zero kills).
                if (m_currentTask.creatureEntry != 0)
                {
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT] %s: objective MOVE_TO auto-arrived (dist=%.1f) — GRIND in place",
                        me->GetName(), dist);
                    ConvertMoveToGrindInPlace();
                }
                else
                {
                    // Stamp the exact arrival coord so C# refreshes ctx.Pos immediately
                    // (no 5s STATE-cycle stale read driving a needless MOVE_TO re-issue).
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT] %s: MOVE_TO auto-arrived (dist=%.1f, threshold=3.0)",
                        me->GetName(), dist);
                    char arrBuf[96];
                    snprintf(arrBuf, sizeof(arrBuf), "MOVE_TO arrived|x=%.1f|y=%.1f|z=%.1f",
                             me->GetPositionX(), me->GetPositionY(), me->GetPositionZ());
                    BridgeSendEvent("TASK_COMPLETE", arrBuf);
                    m_currentTask.Clear();
                    ClearStoredPath();
                }
            }
            return;
        }

        // --- Default idle wander ---
        DoRandomWander();
        return;
    }

    // [PULL] Proactive pull-and-retreat — drag a freshly-tagged mob to open ground before the
    // fight settles, so only it (+ tight neighbours) engage. Runs ahead of the stalemate/overpull
    // handlers and owns the tick while active. (FINDING_005)
    if (HandlePullRetreat())
        return;

    // [ADDED] Combat stalemate breaker — runs only while in combat.
    if (HandleCombatStalemate())
        return;

    // [OVERPULL] Overpull retreat — bail out of a fight where more than the cap are on us.
    if (HandleOverpullRetreat())
        return;

    // Cache victim info before combat system clears it
    if (pVictim && pVictim->IsCreature())
    {
        m_lastVictimEntry = static_cast<Creature*>(pVictim)->GetEntry();
        m_lastVictimGuidLow = pVictim->GetGUIDLow();
    }

    // --- Tap-respect: drop a mob the server says belongs to a non-group player ---
    if (pVictim && pVictim->IsCreature() && !m_combatDirective.IsActive())
    {
        Creature* pVicCre = static_cast<Creature*>(pVictim);
        if (pVicCre->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_TAPPED) &&
            !pVicCre->IsTappedBy(me))
        {
            // Is the tapper in my group? If so it's a focus-fire assist, not a steal.
            bool tapperInMyGroup = false;
            if (Group* pGroup = me->GetGroup())
            {
                if (Player* pTapper = pVicCre->GetLootRecipient())
                    tapperInMyGroup = pGroup->IsMember(pTapper->GetObjectGuid());
            }

            // [PARTY-TAP-EXEMPT] (2026-07-16) The human's fight is authoritative — even on a
            // stranger's tap. If any REAL player in my group is currently attacking this mob,
            // or the mob is beating on a group member (or a member's pet), disengaging is
            // abandoning MY party's fight, not declining a steal: the old gate AttackStopped
            // here while the doctrine ladder recommitted the same mob next tick — a 1 Hz
            // engage/stop thrash that reads at the keyboard as "barely pushing buttons".
            // The human owns the ninja-etiquette call; companions back his choice. Mobs no
            // group player is on remain vetoed (companions still don't initiate steals).
            bool partyOwnsThisFight = false;
            if (!tapperInMyGroup)
            {
                if (Group* pGroup = me->GetGroup())
                {
                    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
                    {
                        Player* pMember = itr->getSource();
                        if (!pMember || pMember == me || !pMember->IsInWorld())
                            continue;
                        WorldSession* pSess = pMember->GetSession();
                        if (!pSess || pSess->GetBot())
                            continue;   // only a HUMAN's choice grants the exemption
                        if (pMember->GetVictim() == pVicCre)
                        {
                            partyOwnsThisFight = true;
                            break;
                        }
                    }
                }
                if (!partyOwnsThisFight)
                    if (Unit* pMobVictim = pVicCre->GetVictim())
                        if (Player* pMobVictimOwner = pMobVictim->GetCharmerOrOwnerPlayerOrPlayerItself())
                            if (pMobVictimOwner == me ||
                                (me->GetGroup() && me->GetGroup()->IsMember(pMobVictimOwner->GetObjectGuid())))
                                partyOwnsThisFight = true;
                if (partyOwnsThisFight)
                    m_combatIgnore.erase(pVicCre->GetGUIDLow());   // heal any earlier wrong veto
            }

            if (!tapperInMyGroup && !partyOwnsThisFight)
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT] %s: victim %u (guid=%u) tapped by another — disengaging (not mine)",
                    me->GetName(), pVicCre->GetEntry(), pVicCre->GetGUIDLow());

                me->AttackStop();
                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
                    StopMoving();
                me->ClearTarget();

                // Short-ignore so Select*Target / SelectGrindTarget don't instantly re-acquire
                // this same tapped mob from the hostile-ref list next tick.
                m_combatIgnore[pVicCre->GetGUIDLow()] = AIBOT_TAPPED_IGNORE_MS;
                return;
            }
        }
    }

    // --- In combat: target validation + switch ---
    if (!pVictim || !IsValidHostileTarget(pVictim) ||
        !pVictim->IsWithinDist(me, VISIBILITY_DISTANCE_SMALL))
    {
        bool victimDied = (pVictim && pVictim->IsDead() && pVictim->IsCreature());
        if (!victimDied && !pVictim && m_lastVictimEntry != 0 && !me->IsInCombat())
            victimDied = true;

        if (victimDied)
        {
            uint32 killedEntry = pVictim ? static_cast<Creature*>(pVictim)->GetEntry() : m_lastVictimEntry;
            uint32 killedGuid = pVictim ? pVictim->GetGUIDLow() : m_lastVictimGuidLow;
            // --- Tapped check: only process kills we actually tagged ---
            Creature* pKillCreature2 = pVictim ? static_cast<Creature*>(pVictim) : me->GetMap()->GetCreature(ObjectGuid(HIGHGUID_UNIT, m_lastVictimEntry, m_lastVictimGuidLow));
            if (pKillCreature2 && pKillCreature2->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_TAPPED) && !pKillCreature2->IsTappedBy(me))
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT] %s: mob (entry=%u guid=%u) tapped by another — skipping kill credit (combat path)",
                    me->GetName(), killedEntry, killedGuid);
                m_lastVictimEntry = 0;
                m_lastVictimGuidLow = 0;
            }
            else
            {
            SendKillEvent(killedEntry, killedGuid);

            // Queue auto-loot with humanization delay
            Creature* deadCreature = pVictim ? static_cast<Creature*>(pVictim) : me->GetMap()->GetCreature(ObjectGuid(HIGHGUID_UNIT, m_lastVictimEntry, m_lastVictimGuidLow));
            if (deadCreature && deadCreature->IsDead())
            {
                m_lootTargetGuid = deadCreature->GetObjectGuid();
                m_lootTimer = urand(1000, 2500);
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-LOOT] %s: loot timer started (%dms) for entry=%u (in-combat path)",
                    me->GetName(), m_lootTimer, killedEntry);
            }

            if (m_currentTask.type == TASK_GRIND)
            {
                // wolf-meat fix (2026-06-30): same MatchesObjectiveEntry widening as the
                // OOC kill-detect path above — primary or any tied alternate counts.
                bool matches = (m_currentTask.creatureEntry == 0 ||
                                m_currentTask.MatchesObjectiveEntry(killedEntry));
                if (matches)
                {
                    m_currentTask.killCount++;
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT] %s: GRIND kill %d/%d (entry=%u)",
                        me->GetName(), m_currentTask.killCount, m_currentTask.killGoal, killedEntry);
                    if (m_currentTask.killGoal > 0 &&
                        m_currentTask.killCount >= m_currentTask.killGoal)
                    {
                        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                            "[AIBOT] %s: GRIND task complete (%d/%d kills)",
                            me->GetName(), m_currentTask.killCount, m_currentTask.killGoal);
                        BridgeSendEvent("TASK_COMPLETE", "GRIND finished");
                        m_currentTask.Clear();
                    }
                }
            }

            m_lastVictimEntry = 0;
            m_lastVictimGuidLow = 0;
            }
        }

        // [DOCTRINE] Target authority on reselect (the flap fix). TeamAuto returns the anchor's
        // mob and the follower COMMITS to it — even if it's the same mob we just "lost" for
        // drifting past VISIBILITY_DISTANCE_SMALL (we were lagging; AttackStart re-chases and the
        // ChaseMovementGenerator closes the gap). No pExcept exclusion of the anchor's mob, and no
        // "not hitting me → drop" bail for a mob the anchor is tanking — those two, plus the SMALL
        // drop, are what produced the A/B/A/B flap. Solo / defer → nullptr → the legacy solo pick
        // + drop below run byte-for-byte. (Kill-credit above already ran regardless.)
        if (Unit* pFocus = m_doctrine->MaintainTarget(*this, pVictim))
        {
            AttackStart(pFocus);
            return;
        }

        if (Unit* pNewVictim = SelectAttackTarget(pVictim))
        {
            if (pVictim != pNewVictim)
            {
                AttackStart(pNewVictim);
                return;
            }
        }

        if (me->GetVictim() &&
           (me != me->GetVictim()->GetVictim()))
        {
            me->AttackStop(false);
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
                StopMoving();
            return;
        }
    }
    else
    {
        // [DOCTRINE] Mid-combat convergence via the single target authority. TeamAuto returns the
        // anchor's mob; a follower SWITCHES to it and commits. There is NO VISIBILITY_DISTANCE_SMALL
        // gate here anymore — that gate (narrower than the doctrine's own NORMAL range check) is
        // exactly what made a lagging follower flap A/B/A/B, switching to a mob the outer validation
        // would reject the very next tick. Committing without it, the ChaseMovementGenerator closes
        // the gap. Solo / defer → nullptr → hold course exactly as before; the anchor gets nullptr
        // from the resolver (self), so it is untouched and the team converges on IT.
        if (Unit* pFocus = m_doctrine->MaintainTarget(*this, pVictim))
        {
            if (pFocus != pVictim && AttackStart(pFocus))
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-DOCTRINE] %s: converge %s -> %s (%s)",
                    me->GetName(), pVictim->GetName(), pFocus->GetName(), m_doctrine->Name());
                return;
            }
        }

        if (!me->HasInArc(pVictim, 2 * M_PI_F / 3) && !me->IsMoving())
        {
            me->SetInFront(pVictim);
            me->SendMovementPacket(MSG_MOVE_SET_FACING, false);
        }

        if (!me->HasUnitState(UNIT_STATE_MELEE_ATTACKING) &&
           (m_role != ROLE_HEALER) &&
            IsValidHostileTarget(pVictim) &&
            AttackStart(pVictim))
            return;
    }

    if (me->IsInCombat())
        UpdateInCombatAI();
}