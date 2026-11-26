//
// Created by romain-p on 17/10/2021.
//
#include "ArenaReplayCloneRewriter.h"
#include "ArenaReplayDatabaseConnection.h"
#include "ArenaReplay_loader.h"
#include "ArenaTeamMgr.h"
#include "Base32.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "CharacterDatabase.h"
#include "Chat.h"
#include "Config.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "QueryPackets.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include <algorithm>
#include <iomanip>
#include <optional>
#include <unordered_map>

constexpr char const* MODULE_STRING = "mod-arena-replay";

enum ReplayStrings
{
    STRING_REPLAY_SAVED = 1,
    STRING_REPLAY_DISABLED = 2,
    STRING_INVALID_INPUT = 3,
    STRING_INVALID_MATCH_ID = 4,
    STRING_PLAYER_NOT_FOUND = 5,
    STRING_PLAYER_REPLAYS_FOUND = 6,
    STRING_PLAYER_REPLAY_ID = 7,
    STRING_PLAYER_NO_REPLAYS = 8,
    STRING_FAVORITE_SAVED = 9,
    STRING_FAVORITE_NOT_FOUND = 10,
    STRING_IN_QUEUE = 11,
    STRING_ARENA_MAP_FAILED = 12,
    STRING_REPLAY_BEGINS = 13,
    STRING_REPLAY_DATA_NOT_FOUND = 14,
    STRING_GOSSIP_LATEST_1V1 = 15,
    STRING_GOSSIP_LATEST_2V2 = 16,
    STRING_GOSSIP_LATEST_3V3 = 17,
    STRING_GOSSIP_LATEST_3V3SOLO = 18,
    STRING_GOSSIP_LATEST_5V5 = 19,
    STRING_GOSSIP_MATCH_ID = 20,
    STRING_GOSSIP_LIST_BY_PLAYERNAME = 21,
    STRING_GOSSIP_MY_FAVORITES = 22,
    STRING_GOSSIP_TOP_1V1_ALLTIME = 23,
    STRING_GOSSIP_TOP_2V2_ALLTIME = 24,
    STRING_GOSSIP_TOP_3V3_ALLTIME = 25,
    STRING_GOSSIP_TOP_3V3SOLO_ALLTIME = 26,
    STRING_GOSSIP_TOP_5V5_ALLTIME = 27,
    STRING_GOSSIP_MOST_WATCHED_ALLTIME = 28,
    STRING_GOSSIP_NO_REPLAYS = 29,
    STRING_GOSSIP_LIST_HEADER = 30,
    STRING_GOSSIP_BACK = 31,
    STRING_GOSSIP_FAVORITE_MATCH_ID = 32,
    STRING_GOSSIP_NO_SAVED_REPLAYS = 33,
    STRING_GOSSIP_REPLAY_MATCH = 34,
    STRING_GOSSIP_TIMES_WATCHED = 35,
    STRING_REPLAY_OWN_MATCH_BLOCKED = 36
};

static std::string GetReplayString(Player* player, uint32 id)
{
    return *player->GetSession()->GetModuleString(MODULE_STRING, id);
}

std::vector<Opcodes> watchList =
{
        SMSG_NOTIFICATION,
        SMSG_AURA_UPDATE,
        SMSG_WORLD_STATE_UI_TIMER_UPDATE,
        SMSG_COMPRESSED_UPDATE_OBJECT,
        SMSG_AURA_UPDATE_ALL,
        SMSG_NAME_QUERY_RESPONSE,
        SMSG_DESTROY_OBJECT,
        MSG_MOVE_START_FORWARD,
        MSG_MOVE_SET_FACING,
        MSG_MOVE_HEARTBEAT,
        MSG_MOVE_JUMP,
        SMSG_MONSTER_MOVE,
        MSG_MOVE_FALL_LAND,
        SMSG_PERIODICAURALOG,
        SMSG_ARENA_UNIT_DESTROYED,
        MSG_MOVE_START_STRAFE_RIGHT,
        MSG_MOVE_STOP_STRAFE,
        MSG_MOVE_START_STRAFE_LEFT,
        MSG_MOVE_STOP,
        MSG_MOVE_START_BACKWARD,
        MSG_MOVE_START_TURN_LEFT,
        MSG_MOVE_STOP_TURN,
        MSG_MOVE_START_TURN_RIGHT,
        SMSG_SPELL_START,
        SMSG_SPELL_GO,
        CMSG_CAST_SPELL,
        CMSG_CANCEL_CAST,
        SMSG_CAST_FAILED,
        SMSG_SPELL_START,
        SMSG_SPELL_FAILURE,
        SMSG_SPELL_DELAYED,
        SMSG_PLAY_SPELL_IMPACT,
        SMSG_FORCE_RUN_SPEED_CHANGE,
        SMSG_ATTACKSTART,
        SMSG_POWER_UPDATE,
        SMSG_ATTACKERSTATEUPDATE,
        SMSG_SPELLDAMAGESHIELD,
        SMSG_SPELLHEALLOG,
        SMSG_SPELLENERGIZELOG,
        SMSG_SPELLNONMELEEDAMAGELOG,
        SMSG_ATTACKSTOP,
        SMSG_EMOTE,
        SMSG_AI_REACTION,
        SMSG_PET_NAME_QUERY_RESPONSE,
        SMSG_CANCEL_AUTO_REPEAT,
        SMSG_UPDATE_OBJECT,
        SMSG_FORCE_FLIGHT_SPEED_CHANGE,
        SMSG_GAMEOBJECT_QUERY_RESPONSE,
        SMSG_FORCE_SWIM_SPEED_CHANGE,
        SMSG_GAMEOBJECT_DESPAWN_ANIM,
        SMSG_CANCEL_COMBAT,
        SMSG_DISMOUNTRESULT,
        SMSG_MOUNTRESULT,
        SMSG_DISMOUNT,
        CMSG_MOUNTSPECIAL_ANIM,
        SMSG_MOUNTSPECIAL_ANIM,
        SMSG_MIRRORIMAGE_DATA,
        SMSG_MESSAGECHAT
};

/*
CMSG_CANCEL_MOUNT_AURA,
CMSG_ALTER_APPEARANCE
SMSG_SUMMON_CANCEL
SMSG_PLAY_SOUND
SMSG_PLAY_SPELL_VISUAL
CMSG_ATTACKSWING
CMSG_ATTACKSTOP*/

// SMSG_MESSAGECHAT starts with the uint8 chat type, see ChatHandler::BuildChatPacket.
// Only local chat everyone around could hear belongs in a replay: whisper, party, guild
// and channel are private to the recorded player, and the arena spectator addon sends its
// data as CHAT_MSG_WHISPER + LANG_ADDON too.
static bool IsReplayableChatPacket(WorldPacket const& packet)
{
    if (packet.GetOpcode() != SMSG_MESSAGECHAT)
        return true;

    // contents() throws on an empty buffer
    if (packet.empty())
        return false;

    switch (packet.contents()[0])
    {
        case CHAT_MSG_SAY:
        case CHAT_MSG_YELL:
        case CHAT_MSG_EMOTE:
        case CHAT_MSG_TEXT_EMOTE:
        case CHAT_MSG_MONSTER_SAY:
        case CHAT_MSG_MONSTER_YELL:
        case CHAT_MSG_MONSTER_EMOTE:
            return true;
        default:
            return false;
    }
}

// Derived from the watcher alone, so a name query for it can be answered
// without any replay state.
static ObjectGuid GetReplayCloneGuid(ObjectGuid watcher)
{
    uint64 high = watcher.GetRawValue() & ~uint64(0xFFFFFFFF);
    uint32 low = 0xFFFFFFFF - uint32(watcher.GetRawValue());
    return ObjectGuid(high | low);
}

// A counter never handed out to a character can't collide with any player.
static bool IsReplayCloneGuidSafe(ObjectGuid clone)
{
    return uint32(clone.GetRawValue()) >=
        sObjectMgr->GetGenerator<HighGuid::Player>().GetNextAfterMaxUsed();
}

static bool IsReplayParticipant(std::string const& guids, ObjectGuid watcher)
{
    std::stringstream stream(guids);
    std::string entry;
    while (std::getline(stream, entry, ','))
    {
        uint64 raw = 0;
        try
        {
            raw = std::stoull(entry);
        }
        catch (std::exception const&)
        {
            continue;
        }

        if (raw == watcher.GetRawValue())
            return true;
    }

    return false;
}

static WorldPacket BuildCloneNameQueryResponse(Player const* watcher,
    ObjectGuid clone)
{
    std::string const name = watcher->GetName() + " - replay";

    WorldPackets::Query::NameQueryResponse response;
    response.Guid = clone.WriteAsPacked();
    response.NameUnknown = false;
    response.Name = name;
    response.Race = watcher->getRace();
    response.Sex = watcher->getGender();
    response.Class = watcher->getClass();
    response.Declined = false;
    return *response.Write();
}

struct PacketRecord { uint32 timestamp; WorldPacket packet; };
struct MatchRecord { BattlegroundTypeId typeId; uint8 arenaTypeId; uint32 mapId; std::deque<PacketRecord> packets; };
struct BgPlayersGuids { std::string alliancePlayerGuids; std::string hordePlayerGuids; };
std::unordered_map<uint32, MatchRecord> records;
std::unordered_map<uint64, MatchRecord> loadedReplays;
std::unordered_map<uint32, uint32> bgReplayIds;
std::unordered_map<uint32, BgPlayersGuids> bgPlayersGuids;

class ArenaReplayServerScript : public ServerScript
{
public:
    ArenaReplayServerScript() : ServerScript("ArenaReplayServerScript", {
        SERVERHOOK_CAN_PACKET_SEND,
        SERVERHOOK_CAN_PACKET_RECEIVE
    }) {}

    // The core answers "unknown" for the clone GUID; this also covers
    // queries sent after the replay ended.
    bool CanPacketReceive(WorldSession* session,
        WorldPacket const& packet) override
    {
        if (packet.GetOpcode() != CMSG_NAME_QUERY || packet.size() < 8)
            return true;

        Player* player = session ? session->GetPlayer() : nullptr;
        if (!player)
            return true;

        ObjectGuid const guid(packet.read<uint64>(0));
        if (guid != GetReplayCloneGuid(player->GetGUID()) ||
            sCharacterCache->GetCharacterCacheByGuid(guid))
            return true;

        WorldPacket response = BuildCloneNameQueryResponse(player, guid);
        session->SendPacket(&response);
        return false;
    }

    bool CanPacketSend(WorldSession* session, WorldPacket const& packet) override
    {
        if (session == nullptr || session->GetPlayer() == nullptr)
            return true;

        Battleground* bg = session->GetPlayer()->GetBattleground();

        if (!bg)
            return true;

        const bool isReplay = bgReplayIds.find(bg->GetInstanceID()) != bgReplayIds.end();

        // ignore packet when no bg or casual games
        if (isReplay)
          return true;

        // ignore packets until arena started
        if (bg->GetStatus() != BattlegroundStatus::STATUS_IN_PROGRESS)
            return true;

        // record packets from 1 player of each team
        // iterate just in case a player leaves and used as reference
        for (auto it : bg->GetPlayers())
        {
            if (it.second->GetBgTeamId() == session->GetPlayer()->GetBgTeamId())
            {
                if (it.second->GetGUID() != session->GetPlayer()->GetGUID())
                    return true;
                else
                    break;
            }
        }

        // ignore packets not in watch list
        if (std::find(watchList.begin(), watchList.end(), packet.GetOpcode()) == watchList.end())
            return true;

        if (!IsReplayableChatPacket(packet))
            return true;

        if (records.find(bg->GetInstanceID()) == records.end())
            records[bg->GetInstanceID()].packets.clear();
        MatchRecord& record = records[bg->GetInstanceID()];

        uint32 timestamp = bg->GetStartTime();
        record.typeId = bg->GetBgTypeID();
        record.arenaTypeId = bg->GetArenaType();
        record.mapId = bg->GetMapId();
        // push back packet inside queue of matchId 0
        record.packets.push_back({ timestamp, /* copy */ WorldPacket(packet) });
        return true;
    }
};

class ArenaReplayArenaScript : public ArenaScript {
public:
  ArenaReplayArenaScript() : ArenaScript("ArenaReplayArenaScript", {
      ARENAHOOK_ON_BEFORE_CHECK_WIN_CONDITION
  }) {}

  bool OnBeforeArenaCheckWinConditions(Battleground *const bg) override {
    const bool isReplay = bgReplayIds.find(bg->GetInstanceID()) != bgReplayIds.end();

    // if isReplay then return false to exit from check condition
    return !isReplay;
  }
};

class ArenaReplayBGScript : public BGScript
{
public:
    ArenaReplayBGScript() : BGScript("ArenaReplayBGScript", {
        ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_UPDATE,
        ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_ADD_PLAYER,
        ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_END,
        ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_DESTROY
    }) {}

    void OnBattlegroundUpdate(Battleground* bg, uint32 /* diff */) override
    {
        const bool isReplay = bgReplayIds.find(bg->GetInstanceID()) != bgReplayIds.end();
        if (!isReplay)
            return;

        if (!bg->isArena() && !sConfigMgr->GetOption<bool>("ArenaReplay.SaveBattlegrounds", true))
            return;

        if (!bg->isRated() && !sConfigMgr->GetOption<bool>("ArenaReplay.SaveUnratedArenas", true))
            return;

        uint32 replayId = bgReplayIds.at(bg->GetInstanceID());

        int32 startDelayTime = bg->GetStartDelayTime();
        if (startDelayTime > 1000) // reduces StartTime only when watching Replay
        {
            bg->SetStartDelayTime(1000);
            bg->SetStartTime(bg->GetStartTime() + (startDelayTime - 1000));
        }

        if (bg->GetStatus() != BattlegroundStatus::STATUS_IN_PROGRESS)
            return;

        // retrieve arena replay data
        auto it = loadedReplays.find(replayId);
        if (it == loadedReplays.end())
            return;

        MatchRecord& match = it->second;

        // if replay ends or spectator left > free arena replay data and/or kick player
        if (match.packets.empty() || bg->GetPlayers().empty())
        {
            loadedReplays.erase(it);

            if (!bg->GetPlayers().empty())
                bg->GetPlayers().begin()->second->LeaveBattleground(bg);

            return;
        }

        //send replay data to spectator
        while (!match.packets.empty() && match.packets.front().timestamp <= bg->GetStartTime())
        {
            if (bg->GetPlayers().empty())
                break;

            WorldPacket* myPacket = &match.packets.front().packet;
            Player* replayer = bg->GetPlayers().begin()->second;
            replayer->GetSession()->SendPacket(myPacket);
            match.packets.pop_front();
        }
    }

    void OnBattlegroundAddPlayer(Battleground* bg, Player* player) override
    {
        if (!player)
            return;

        if (player->IsSpectator())
            return;

        if (!bg->isArena() && !sConfigMgr->GetOption<bool>("ArenaReplay.SaveBattlegrounds", true))
            return;

        if (!bg->isRated() && !sConfigMgr->GetOption<bool>("ArenaReplay.SaveUnratedArenas", true))
            return;

        if (bgPlayersGuids.find(bg->GetInstanceID()) == bgPlayersGuids.end())
        {
            BgPlayersGuids playerguids;
            bgPlayersGuids[bg->GetInstanceID()] = playerguids;
        }

        std::string playerGuid = std::to_string(player->GetGUID().GetRawValue());
        TeamId bgTeamId = player->GetBgTeamId();

        if (bgTeamId == TEAM_ALLIANCE)
        {
            if (!bgPlayersGuids[bg->GetInstanceID()].alliancePlayerGuids.empty())
                bgPlayersGuids[bg->GetInstanceID()].alliancePlayerGuids += ", ";

            bgPlayersGuids[bg->GetInstanceID()].alliancePlayerGuids += playerGuid;
        }
        else
        {
            if (!bgPlayersGuids[bg->GetInstanceID()].hordePlayerGuids.empty())
                bgPlayersGuids[bg->GetInstanceID()].hordePlayerGuids += ", ";

            bgPlayersGuids[bg->GetInstanceID()].hordePlayerGuids += playerGuid;
        }
    }

    void OnBattlegroundEnd(Battleground *bg, TeamId winnerTeamId ) override {

        if (!bg->isArena() && !sConfigMgr->GetOption<bool>("ArenaReplay.SaveBattlegrounds", true))
            return;

        if (!bg->isRated() && !sConfigMgr->GetOption<bool>("ArenaReplay.SaveUnratedArenas", true))
            return;

        const bool isReplay = bgReplayIds.find(bg->GetInstanceID()) != bgReplayIds.end();

        // only saves if arena lasted at least X secs (StartDelayTime is included - 60s StartDelayTime + X StartTime)
        uint32 ValidArenaDuration = sConfigMgr->GetOption<uint32>("ArenaReplay.ValidArenaDuration", 75) * IN_MILLISECONDS;
        bool ValidArena = (bg->GetStartTime()) >= ValidArenaDuration || sConfigMgr->GetOption<uint32>("ArenaReplay.ValidArenaDuration", 75) == 0;

        // save replay when a bg ends
        if (!isReplay && ValidArena)
            saveReplay(bg, winnerTeamId);
    }

    // ~Battleground calls this for every BG; a replay BG that empties
    // before a time limit ends it is deleted without OnBattlegroundEnd
    void OnBattlegroundDestroy(Battleground* bg) override
    {
        bgReplayIds.erase(bg->GetInstanceID());
        bgPlayersGuids.erase(bg->GetInstanceID());
    }

    void saveReplay(Battleground* bg, TeamId winnerTeamId)
    {
        // retrieve replay data
        auto it = records.find(bg->GetInstanceID());
        if (it == records.end())
            return;

        MatchRecord& match = it->second;

        /** serialize arena replay data **/
        // Stay safely below ByteBuffer's hard 10,000,000-byte ASSERT in append(); crossing it crashes the worldserver.
        // https://github.com/azerothcore/azerothcore-wotlk/blob/23c6909eb611173c0decc68a32e42cc41f84126b/src/server/shared/Packets/ByteBuffer.cpp#L114
        constexpr uint32 REPLAY_MAX_BUFFER_SIZE = 9000000;

        ArenaReplayByteBuffer buffer;
        uint32 headerSize;
        uint32 timestamp;
        for (auto it : match.packets)
        {
            headerSize = it.packet.size(); //header 4Bytes packet size
            timestamp = it.timestamp;

            // break on a packet boundary so the written stream stays well-framed for playback
            if (buffer.size() + sizeof(headerSize) + sizeof(timestamp) + sizeof(uint16) + headerSize >= REPLAY_MAX_BUFFER_SIZE)
            {
                LOG_WARN("module", "ArenaReplay: replay for instance {} reached the {}-byte limit, truncating.",
                    bg->GetInstanceID(), REPLAY_MAX_BUFFER_SIZE);
                break;
            }

            buffer << headerSize; // 4 bytes
            buffer << timestamp; // 4 bytes
            buffer << it.packet.GetOpcode(); // 2 bytes
            if (headerSize > 0)
                buffer.append(it.packet.contents(), it.packet.size()); // headerSize bytes
        }

        uint32 teamWinnerRating = 0;
        uint32 teamLoserRating = 0;
        uint32 teamWinnerMMR = 0;
        uint32 teamLoserMMR = 0;
        std::string teamWinnerName;
        std::string teamLoserName;
        std::string winnerGuids;
        std::string loserGuids;

        if (winnerTeamId == TEAM_ALLIANCE)
        {
            winnerGuids = bgPlayersGuids[bg->GetInstanceID()].alliancePlayerGuids;
            loserGuids = bgPlayersGuids[bg->GetInstanceID()].hordePlayerGuids;
        }
        else
        {
            loserGuids = bgPlayersGuids[bg->GetInstanceID()].alliancePlayerGuids;
            winnerGuids = bgPlayersGuids[bg->GetInstanceID()].hordePlayerGuids;
        }

        for (const auto& playerPair : bg->GetPlayers())
        {
            Player* player = playerPair.second;
            if (!player || player->IsSpectator())
                continue;

            std::string playerGuid = std::to_string(player->GetGUID().GetRawValue());
            TeamId bgTeamId = player->GetBgTeamId();
            ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(bg->GetArenaTeamIdForTeam(bgTeamId));
            uint32 arenaTeamId = bg->GetArenaTeamIdForTeam(bgTeamId);
            TeamId teamId = static_cast<TeamId>(arenaTeamId);
            uint32 teamMMR = bg->GetArenaMatchmakerRating(teamId);

            if (bgTeamId == winnerTeamId)
            {
                getTeamInformation(bg, team, teamWinnerName, teamWinnerRating);
                teamWinnerMMR = teamMMR;
            }
            else // Loss
            {
                getTeamInformation(bg, team, teamLoserName, teamLoserRating);
                teamLoserMMR = teamMMR;
            }

            // Send replay ID to player after a game end
            uint32 replayfightid = 0;
            QueryResult qResult = CharacterDatabase.Query("SELECT MAX(`id`) AS max_id FROM `character_arena_replays`");
            if (qResult)
            {
                do
                {
                    replayfightid = qResult->Fetch()[0].Get<uint32>();
                } while (qResult->NextRow());
            }
            ChatHandler(player->GetSession()).PSendModuleSysMessage(MODULE_STRING, STRING_REPLAY_SAVED, replayfightid + 1);
        }

        const uint8 ARENA_TYPE_3V3_SOLO_QUEUE = sConfigMgr->GetOption<uint8>("ArenaReplay.3v3soloQ.ArenaType", 4);
        if (bg->isArena() && (!bg->isRated() || bg->GetArenaType() == ARENA_TYPE_3V3_SOLO_QUEUE))
        {
            teamWinnerName = GetTeamName(winnerGuids);
            teamLoserName = GetTeamName(loserGuids);
        }
        else if (!bg->isArena())
        {
            teamWinnerName = "Battleground";
            teamLoserName = "Battleground";
        }

        // // if loser has a negative value. the uint variable could return this (wrong) value
        // if (teamLoserMMR >= 4294967286)
        //     teamLoserMMR=0;

        // if (teamWinnerMMR >= 4294967286)
        //     teamWinnerMMR=0;

        // temporary code until the issue is not properly fixed
        teamLoserMMR=0;
        teamWinnerMMR=0;

        CharacterDatabase.Execute("INSERT INTO `character_arena_replays` "
            //   1             2            3            4          5          6                  7                    8
            "(`arenaTypeId`, `typeId`, `contentSize`, `contents`, `mapId`, `winnerTeamName`, `winnerTeamRating`, `winnerTeamMMR`, "
            //    9                10                 11                 12                 13
            "`loserTeamName`, `loserTeamRating`, `loserTeamMMR`, `winnerPlayerGuids`, `loserPlayerGuids`) "

            "VALUES ({}, {}, {}, \"{}\", {}, '{}', {}, {}, '{}', {}, {}, \"{}\", \"{}\")",
            //       1   2    3     4    5    6    7   8    9    10  11    12      13

            uint32(match.arenaTypeId), // 1
            uint32(match.typeId),      // 2
            buffer.size(),             // 3
            Acore::Encoding::Base32::Encode(buffer.contentsAsVector()), // 4
            bg->GetMapId(),    // 5
            teamWinnerName,    // 6
            teamWinnerRating,  // 7
            teamWinnerMMR,     // 8
            teamLoserName,     // 9
            teamLoserRating,   // 10
            teamLoserMMR,      // 11
            winnerGuids,       // 12
            loserGuids         // 13
        );

        records.erase(it);
    }

private:
    void getTeamInformation(Battleground *bg, ArenaTeam* team, std::string &teamName, uint32 &teamRating) {
        if (bg->isRated() && team)
        {
            if (team->GetId() < 0xFFF00000)
            {
                teamName = team->GetName();
                teamRating = team->GetRating();
            }
        }
    }

    std::string GetTeamName(std::string listPlayerGuids) {
        std::string teamName;
        std::stringstream ssPlayerGuids(listPlayerGuids);

        std::vector<std::string> playerGuids;
        std::string playerGuid;
        while (std::getline(ssPlayerGuids, playerGuid, ','))
            playerGuids.push_back(playerGuid);

        for (const std::string& guid : playerGuids)
        {
            uint64 _guid = std::stoi(guid);
            CharacterCacheEntry const* playerData = sCharacterCache->GetCharacterCacheByGuid(ObjectGuid(_guid));
            if (playerData)
                teamName += playerData->Name + " ";
        }

        // truncate last character if space
        if (!teamName.empty() && teamName.substr(teamName.size()-1, teamName.size()) == " ") {
            teamName.pop_back();
        }

        return teamName;
    }
};

enum ReplayGossips
{
    REPLAY_LATEST_2V2 = 1,
    REPLAY_LATEST_3V3 = 2,
    REPLAY_LATEST_5V5 = 3,
    REPLAY_LATEST_3V3SOLO = 4,
    REPLAY_LATEST_1V1 = 5,
    REPLAY_MATCH_ID = 6,
    REPLAY_LIST_BY_PLAYERNAME = 7,
    MY_FAVORITE_MATCHES = 8,
    REPLAY_TOP_2V2_ALLTIME = 9,
    REPLAY_TOP_3V3_ALLTIME = 10,
    REPLAY_TOP_5V5_ALLTIME = 11,
    REPLAY_TOP_3V3SOLO_ALLTIME = 12,
    REPLAY_TOP_1V1_ALLTIME = 13,
    REPLAY_MOST_WATCHED_ALLTIME = 14
};

class ReplayGossip : public CreatureScript
{
public:

    ReplayGossip() : CreatureScript("ReplayGossip") { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        if (!sConfigMgr->GetOption<bool>("ArenaReplay.Enable", true))
        {
            ChatHandler(player->GetSession()).PSendModuleSysMessage(MODULE_STRING, STRING_REPLAY_DISABLED);
            return true;
        }

        const bool isArena1v1Enabled = sConfigMgr->GetOption<bool>("ArenaReplay.1v1.Enable", false);
        const bool isArena3v3soloQEnabled = sConfigMgr->GetOption<bool>("ArenaReplay.3v3soloQ.Enable", false);

        if (isArena1v1Enabled)
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, GetReplayString(player, STRING_GOSSIP_LATEST_1V1), GOSSIP_SENDER_MAIN, REPLAY_LATEST_1V1);

        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, GetReplayString(player, STRING_GOSSIP_LATEST_2V2), GOSSIP_SENDER_MAIN, REPLAY_LATEST_2V2);
        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, GetReplayString(player, STRING_GOSSIP_LATEST_3V3), GOSSIP_SENDER_MAIN, REPLAY_LATEST_3V3);

        if (isArena3v3soloQEnabled)
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, GetReplayString(player, STRING_GOSSIP_LATEST_3V3SOLO), GOSSIP_SENDER_MAIN, REPLAY_LATEST_3V3SOLO);

        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, GetReplayString(player, STRING_GOSSIP_LATEST_5V5), GOSSIP_SENDER_MAIN, REPLAY_LATEST_5V5);

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, GetReplayString(player, STRING_GOSSIP_MATCH_ID), GOSSIP_SENDER_MAIN, REPLAY_MATCH_ID, "", 0, true);             // maybe add command .replay 'replayID' aswell
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, GetReplayString(player, STRING_GOSSIP_LIST_BY_PLAYERNAME), GOSSIP_SENDER_MAIN, REPLAY_LIST_BY_PLAYERNAME, "", 0, true); // to do: show a list, showing games with type, teamname and teamrating
        AddGossipItemFor(player, GOSSIP_ICON_TRAINER, GetReplayString(player, STRING_GOSSIP_MY_FAVORITES), GOSSIP_SENDER_MAIN, MY_FAVORITE_MATCHES);                   // To do: somehow show teamName/TeamRating/Classes (it's a different db table)

        if (isArena1v1Enabled)
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, GetReplayString(player, STRING_GOSSIP_TOP_1V1_ALLTIME), GOSSIP_SENDER_MAIN, REPLAY_TOP_1V1_ALLTIME);

        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, GetReplayString(player, STRING_GOSSIP_TOP_2V2_ALLTIME), GOSSIP_SENDER_MAIN, REPLAY_TOP_2V2_ALLTIME);
        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, GetReplayString(player, STRING_GOSSIP_TOP_3V3_ALLTIME), GOSSIP_SENDER_MAIN, REPLAY_TOP_3V3_ALLTIME);

        if (isArena3v3soloQEnabled)
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, GetReplayString(player, STRING_GOSSIP_TOP_3V3SOLO_ALLTIME), GOSSIP_SENDER_MAIN, REPLAY_TOP_3V3SOLO_ALLTIME);

        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, GetReplayString(player, STRING_GOSSIP_TOP_5V5_ALLTIME), GOSSIP_SENDER_MAIN, REPLAY_TOP_5V5_ALLTIME);


        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, GetReplayString(player, STRING_GOSSIP_MOST_WATCHED_ALLTIME), GOSSIP_SENDER_MAIN, REPLAY_MOST_WATCHED_ALLTIME);  // To Do: show arena type, maybe hide team name
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());

        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 /* sender */, uint32 action) override
    {
        const uint8 ARENA_TYPE_1v1 = sConfigMgr->GetOption<uint8>("ArenaReplay.1v1.ArenaType", 1);
        const uint8 ARENA_TYPE_3V3_SOLO_QUEUE = sConfigMgr->GetOption<uint8>("ArenaReplay.3v3soloQ.ArenaType", 4);

        player->PlayerTalkClass->ClearMenus();
        switch (action)
        {
            case REPLAY_LATEST_2V2:
                player->PlayerTalkClass->SendCloseGossip();
                ShowReplaysLast30Days(player, creature, ARENA_TYPE_2v2);
                break;
            case REPLAY_LATEST_3V3:
                player->PlayerTalkClass->SendCloseGossip();
                ShowReplaysLast30Days(player, creature, ARENA_TYPE_3v3);
                break;
            case REPLAY_LATEST_5V5:
                player->PlayerTalkClass->SendCloseGossip();
                ShowReplaysLast30Days(player, creature, ARENA_TYPE_5v5);
                break;
            case REPLAY_LATEST_3V3SOLO:
                player->PlayerTalkClass->SendCloseGossip();
                ShowReplaysLast30Days(player, creature, ARENA_TYPE_3V3_SOLO_QUEUE);
                break;
            case REPLAY_LATEST_1V1:
                player->PlayerTalkClass->SendCloseGossip();
                ShowReplaysLast30Days(player, creature, ARENA_TYPE_1v1);
                break;
            case REPLAY_TOP_2V2_ALLTIME:
                player->PlayerTalkClass->SendCloseGossip();
                ShowReplaysAllTime(player, creature, ARENA_TYPE_2v2);
                break;
            case REPLAY_TOP_3V3_ALLTIME:
                player->PlayerTalkClass->SendCloseGossip();
                ShowReplaysAllTime(player, creature, ARENA_TYPE_3v3);
                break;
            case REPLAY_TOP_5V5_ALLTIME:
                player->PlayerTalkClass->SendCloseGossip();
                ShowReplaysAllTime(player, creature, ARENA_TYPE_5v5);
                break;
            case REPLAY_TOP_3V3SOLO_ALLTIME:
                player->PlayerTalkClass->SendCloseGossip();
                ShowReplaysAllTime(player, creature, ARENA_TYPE_3V3_SOLO_QUEUE);
                break;
            case REPLAY_TOP_1V1_ALLTIME:
                player->PlayerTalkClass->SendCloseGossip();
                ShowReplaysAllTime(player, creature, ARENA_TYPE_1v1);
                break;
            case REPLAY_MOST_WATCHED_ALLTIME:
                player->PlayerTalkClass->SendCloseGossip();
                ShowMostWatchedReplays(player, creature);
                break;
            case MY_FAVORITE_MATCHES:
                player->PlayerTalkClass->SendCloseGossip();
                ShowSavedReplays(player, creature);
                break;
            case GOSSIP_ACTION_INFO_DEF: // "Back"
                OnGossipHello(player, creature);
                break;

            default:
                if (action >= GOSSIP_ACTION_INFO_DEF + 30) // Replay selected arenas (intid >= 30)
                    return replayArenaMatch(player, action - (GOSSIP_ACTION_INFO_DEF + 30));
        }

        return true;
    }

    bool OnGossipSelectCode(Player* player, Creature* /* creature */, uint32 /* sender */, uint32 action, const char* code) override
    {
        if (!code)
        {
            CloseGossipMenuFor(player);
            return false;
        }

        // Forbidden: ', %, and , (' causes crash when using 'Replay list by player name')
        std::string inputCode = std::string(code);
        if (inputCode.find('\'') != std::string::npos ||
            inputCode.find('%') != std::string::npos ||
            inputCode.find(',') != std::string::npos ||
            inputCode.length() > 50 ||
            inputCode.empty())
        {
            ChatHandler(player->GetSession()).PSendModuleSysMessage(MODULE_STRING, STRING_INVALID_INPUT);
            CloseGossipMenuFor(player);
            return false;
        }

        switch (action)
        {
            case REPLAY_MATCH_ID:
            {
                uint32 replayId;
                try
                {
                    replayId = std::stoi(code);
                }
                catch (...)
                {
                    ChatHandler(player->GetSession()).PSendModuleSysMessage(MODULE_STRING, STRING_INVALID_MATCH_ID);
                    CloseGossipMenuFor(player);
                    return false;
                }

                return replayArenaMatch(player, replayId);
            }
            case REPLAY_LIST_BY_PLAYERNAME:
            {
                CharacterCacheEntry const* playerData = sCharacterCache->GetCharacterCacheByName(std::string(code));
                if (!playerData)
                {
                    ChatHandler(player->GetSession()).PSendModuleSysMessage(MODULE_STRING, STRING_PLAYER_NOT_FOUND, std::string(code));
                    CloseGossipMenuFor(player);
                    return false;
                }

                std::string playerGuidStr = std::to_string(playerData->Guid.GetRawValue());

                // \? keeps the ?? fallbacks from being read as trigraphs (GCC -Wtrigraphs)
                QueryResult result = CharacterDatabase.Query(
                    "SELECT id, IFNULL(DATE_FORMAT(`timestamp`, '%d-%m-%Y'), '\?\?-\?\?-\?\?\?\?'), "
                    "IFNULL(DATE_FORMAT(`timestamp`, '%H:%i:%s'), '\?\?:\?\?:\?\?') "
                    "FROM character_arena_replays "
                    "WHERE winnerPlayerGuids LIKE '%{}%' OR loserPlayerGuids LIKE '%{}%'",
                    playerGuidStr, playerGuidStr);
                if (result)
                {
                    ChatHandler(player->GetSession()).PSendModuleSysMessage(MODULE_STRING, STRING_PLAYER_REPLAYS_FOUND, std::string(code));
                    do
                    {
                        Field* fields = result->Fetch();
                        uint32 replayId = fields[0].Get<uint32>();
                        std::string replayDate = fields[1].Get<std::string>();
                        std::string replayTime = fields[2].Get<std::string>();
                        ChatHandler(player->GetSession()).PSendModuleSysMessage(MODULE_STRING, STRING_PLAYER_REPLAY_ID,
                            replayId, replayDate, replayTime);
                        //AddGossipItemFor(player,) // to do: add gossips with the replays
                    } while (result->NextRow());

                    CloseGossipMenuFor(player);
                    return true;
                }
                else
                {
                    ChatHandler(player->GetSession()).PSendModuleSysMessage(MODULE_STRING, STRING_PLAYER_NO_REPLAYS, std::string(code));
                    CloseGossipMenuFor(player);
                    return false;
                }
            }
            case MY_FAVORITE_MATCHES:
            {
                try
                {
                    uint32 NumberTyped = std::stoi(code);
                    FavoriteMatchId(player->GetGUID().GetCounter(), NumberTyped);
                    return true;
                }
                catch (...)
                {
                    ChatHandler(player->GetSession()).PSendModuleSysMessage(MODULE_STRING, STRING_INVALID_MATCH_ID);
                    CloseGossipMenuFor(player);
                    return false;
                }
            }
        }

        return false;
    }

private:

    std::string GetClassIconById(uint8 id)
    {
        switch (id)
        {
            case CLASS_WARRIOR:
                return "|TInterface\\icons\\inv_sword_27";
            case CLASS_PALADIN:
                return "|TInterface\\icons\\inv_hammer_01";
            case CLASS_HUNTER:
                return "|TInterface\\icons\\inv_weapon_bow_07";
            case CLASS_ROGUE:
                return "|TInterface\\icons\\inv_throwingknife_04";
            case CLASS_PRIEST:
                return "|TInterface\\icons\\inv_staff_30";
            case CLASS_DEATH_KNIGHT:
                return "|TInterface\\icons\\spell_deathknight_classicon";
            case CLASS_SHAMAN:
                return "|TInterface\\icons\\inv_jewelry_talisman_04";
            case CLASS_MAGE:
                return "|TInterface\\icons\\inv_staff_13";
            case CLASS_WARLOCK:
                return "|TInterface\\icons\\spell_nature_drowsy";
            case CLASS_DRUID:
                return "|TInterface\\icons\\inv_misc_monsterclaw_04";
            default:
                return "";
        }
    }

    std::string GetRaceIconById(uint8 id, uint8 gender) {
        const std::string gender_icon = gender == GENDER_MALE ? "male" : "female";
        switch (id) {
            case RACE_HUMAN:
                return "|TInterface/ICONS/achievement_character_human_" + gender_icon;
            case RACE_ORC:
                return "|TInterface/ICONS/achievement_character_orc_" + gender_icon;
            case RACE_DWARF:
                return "|TInterface/ICONS/achievement_character_dwarf_" + gender_icon;
            case RACE_NIGHTELF:
                return "|TInterface/ICONS/achievement_character_nightelf_" + gender_icon;
            case RACE_UNDEAD_PLAYER:
                return "|TInterface/ICONS/achievement_character_undead_" + gender_icon;
            case RACE_TAUREN:
                return "|TInterface/ICONS/achievement_character_tauren_" + gender_icon;
            case RACE_GNOME:
                return "|TInterface/ICONS/achievement_character_gnome_" + gender_icon;
            case RACE_TROLL:
                return "|TInterface/ICONS/achievement_character_troll_" + gender_icon;
            case RACE_BLOODELF:
                return "|TInterface/ICONS/achievement_character_bloodelf_" + gender_icon;
            case RACE_DRAENEI:
                return "|TInterface/ICONS/achievement_character_draenei_" + gender_icon;
            default:
                return "";
        }
    }

    std::string GetPlayersIconTexts(std::string playerGuids) {
        std::string iconsTextTeam;
        std::vector<std::string> playerGuidsTeam1;

        std::stringstream ssPlayerGuids(playerGuids);
        std::string item;

        while (std::getline(ssPlayerGuids, item, ','))
            playerGuidsTeam1.push_back(item);

        for (const std::string& guid : playerGuidsTeam1)
        {
            uint64 _guid = std::stoi(guid);
            CharacterCacheEntry const* playerData = sCharacterCache->GetCharacterCacheByGuid(ObjectGuid(_guid));
            if (playerData)
            {
                iconsTextTeam += GetClassIconById(playerData->Class) + ":14:14:05:00|t|r";
                iconsTextTeam += GetRaceIconById(playerData->Race, playerData->Sex) + ":14:14:05:00|t|r ";
            }
        }

        if (!iconsTextTeam.empty() && iconsTextTeam.back() == '\n')
            iconsTextTeam.pop_back();

        return iconsTextTeam;
    }

    struct ReplayInfo
    {
        uint32 matchId;

        std::string winnerPlayerGuids;
        std::string winnerTeamName;
        uint32 winnerTeamRating;

        std::string loserPlayerGuids;
        std::string loserTeamName;
        uint32 loserTeamRating;

        std::optional<uint32> TimesWatched;

        //uint32 winnerMMR;
        //uint32 loserMMR;
    };

    std::string GetGossipText(ReplayInfo info) {
        std::string iconsTextTeam1 = GetPlayersIconTexts(info.winnerPlayerGuids);
        std::string iconsTextTeam2 = GetPlayersIconTexts(info.loserPlayerGuids);

        std::string coloredWinnerTeamName = "|cff33691E" + info.winnerTeamName + "|r";
        std::string LoserTeamName = info.loserTeamName;

        // The most watched list puts both teams on one line, the count below
        std::string const vs = info.TimesWatched ? " vs (" : "\n vs (";

        std::string gossipText = ("[" + std::to_string(info.matchId) + "] (" +
            std::to_string(info.winnerTeamRating) + ")" +
            iconsTextTeam1 + "" +
            " '" + coloredWinnerTeamName + "'" +
            vs + std::to_string(info.loserTeamRating) + ")" +
            iconsTextTeam2 + "" +
            " '" + LoserTeamName + "'");

        return gossipText;
    }

    void ShowReplaysAllTime(Player* player, Creature* creature, uint8 arenaTypeId)
    {
        auto matchInfos = loadReplaysAllTimeByArenaType(arenaTypeId);
        ShowReplays(player, creature, matchInfos);
    }

    std::vector<ReplayInfo> loadReplaysAllTimeByArenaType(uint8 arenaTypeId)
    {
        std::vector<ReplayInfo> records;
        QueryResult result = CharacterDatabase.Query("SELECT id, winnerTeamName, winnerTeamRating, winnerPlayerGuids, loserTeamName, loserTeamRating, loserPlayerGuids FROM character_arena_replays WHERE arenaTypeId = {} ORDER BY winnerTeamRating DESC LIMIT 20", arenaTypeId);

        if (!result)
            return records;

        do
        {
            Field* fields = result->Fetch();
            if (!fields)
                return records;

            ReplayInfo info;
            info.matchId = fields[0].Get<uint32>();
            info.winnerTeamName = fields[1].Get<std::string>();
            info.winnerTeamRating = fields[2].Get<uint32>();
            info.winnerPlayerGuids = fields[3].Get<std::string>();
            info.loserTeamName = fields[4].Get<std::string>();
            info.loserTeamRating = fields[5].Get<uint32>();
            info.loserPlayerGuids = fields[6].Get<std::string>();

            records.push_back(info);
        } while (result->NextRow());

        return records;
    }

    void ShowReplaysLast30Days(Player* player, Creature* creature, uint8 arenaTypeId)
    {
        auto matchInfos = loadReplaysLast30Days(arenaTypeId);
        ShowReplays(player, creature, matchInfos);
    }

    std::vector<ReplayInfo> loadReplaysLast30Days(uint8 arenaTypeId)
    {
        std::vector<ReplayInfo> records;

        std::time_t now = std::time(nullptr);
        std::tm* tmNow = std::localtime(&now);
        tmNow->tm_mday -= 30;
        std::mktime(tmNow);

        std::stringstream ss;
        ss << std::put_time(tmNow, "%Y-%m-%d %H:%M:%S");
        std::string thirtyDaysAgo = ss.str();

		// Only show games that are 30 days old
        QueryResult result = CharacterDatabase.Query(
            "SELECT id, winnerTeamName, winnerTeamRating, winnerPlayerGuids, loserTeamName, loserTeamRating, loserPlayerGuids, timestamp "
            "FROM character_arena_replays "
            "WHERE arenaTypeId = {} AND timestamp >= '{}' "
            "ORDER BY id DESC LIMIT 20", arenaTypeId, thirtyDaysAgo.c_str());

        if (!result)
            return records;

        do
        {
            Field* fields = result->Fetch();
            if (!fields)
                return records;

            ReplayInfo info;
            info.matchId = fields[0].Get<uint32>();
            info.winnerTeamName = fields[1].Get<std::string>();
            info.winnerTeamRating = fields[2].Get<uint32>();
            info.winnerPlayerGuids = fields[3].Get<std::string>();
            info.loserTeamName = fields[4].Get<std::string>();
            info.loserTeamRating = fields[5].Get<uint32>();
            info.loserPlayerGuids = fields[6].Get<std::string>();

            records.push_back(info);
        } while (result->NextRow());

        return records;
    }

    void ShowMostWatchedReplays(Player* player, Creature* creature)
    {
        auto matchInfos = loadMostWatchedReplays();
        ShowReplays(player, creature, matchInfos);
    }

    void ShowReplays(Player* player, Creature* creature, std::vector<ReplayInfo> matchInfos) {
        if (matchInfos.empty())
            AddGossipItemFor(player, GOSSIP_ICON_TAXI, GetReplayString(player, STRING_GOSSIP_NO_REPLAYS), GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF);
        else
        {
            AddGossipItemFor(player, GOSSIP_ICON_TRAINER, GetReplayString(player, STRING_GOSSIP_LIST_HEADER), GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF); // Back to Main Menu
            for (const auto& info : matchInfos)
            {
                std::string gossipText = GetGossipText(info);
                if (info.TimesWatched)
                {
                    ChatHandler handler(player->GetSession());
                    gossipText += "\n " + handler.PGetParseModuleString(
                        MODULE_STRING, STRING_GOSSIP_TIMES_WATCHED,
                        *info.TimesWatched);
                }

                const uint32 actionOffset = GOSSIP_ACTION_INFO_DEF + 30;
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, gossipText, GOSSIP_SENDER_MAIN, actionOffset + info.matchId);
            }
        }

        AddGossipItemFor(player, GOSSIP_ICON_TAXI, GetReplayString(player, STRING_GOSSIP_BACK), GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF);
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
    }

    std::vector<ReplayInfo> loadMostWatchedReplays()
    {
        std::vector<ReplayInfo> records;
        QueryResult result = CharacterDatabase.Query(
            "SELECT id, winnerTeamName, winnerTeamRating, winnerPlayerGuids, "
            "loserTeamName, loserTeamRating, loserPlayerGuids, timesWatched "
            "FROM character_arena_replays "
            "ORDER BY timesWatched DESC, winnerTeamRating DESC "
            "LIMIT 28");

        if (!result)
            return records;

        do
        {
            Field* fields = result->Fetch();
            if (!fields)
                return records;

            ReplayInfo info;
            info.matchId = fields[0].Get<uint32>();
            info.winnerTeamName = fields[1].Get<std::string>();
            info.winnerTeamRating = fields[2].Get<uint32>();
            info.winnerPlayerGuids = fields[3].Get<std::string>();
            info.loserTeamName = fields[4].Get<std::string>();
            info.loserTeamRating = fields[5].Get<uint32>();
            info.loserPlayerGuids = fields[6].Get<std::string>();
            info.TimesWatched = fields[7].Get<uint32>();

            records.push_back(info);
        } while (result->NextRow());

        return records;
    }

    void ShowSavedReplays(Player* player, Creature* creature, bool firstPage = true)
    {
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, GetReplayString(player, STRING_GOSSIP_FAVORITE_MATCH_ID), GOSSIP_SENDER_MAIN, MY_FAVORITE_MATCHES, "", 0, true);

        std::string sortOrder = (firstPage) ? "ASC" : "DESC";
        QueryResult result = CharacterDatabase.Query("SELECT replay_id FROM character_saved_replays WHERE character_id = " + std::to_string(player->GetGUID().GetCounter()) + " ORDER BY id " + sortOrder + " LIMIT 29");
        if (!result)
            AddGossipItemFor(player, GOSSIP_ICON_TAXI, GetReplayString(player, STRING_GOSSIP_NO_SAVED_REPLAYS), GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF);
        else
        {
            do
            {
                Field* fields = result->Fetch();
                if (!fields)
                    break;

                uint32 matchId = fields[0].Get<uint32>();
                const uint32 actionOffset = GOSSIP_ACTION_INFO_DEF + 30;
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, ChatHandler(player->GetSession()).PGetParseModuleString(MODULE_STRING, STRING_GOSSIP_REPLAY_MATCH, matchId), GOSSIP_SENDER_MAIN, actionOffset + matchId);

            } while (result->NextRow());

            if (firstPage) // to do
            {
                //AddGossipItemFor(player, GOSSIP_ICON_TAXI, "Next Page", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF + 1);
            }
        }
        AddGossipItemFor(player, GOSSIP_ICON_TAXI, GetReplayString(player, STRING_GOSSIP_BACK), GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF);
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
    }

    void FavoriteMatchId(uint64 playerGuid, uint32 code)
    {
        // Need to check if the match exists in character_arena_replays, then insert in character_saved_replays
        QueryResult result = CharacterDatabase.Query("SELECT id FROM character_arena_replays WHERE id = " + std::to_string(code));
        if (result)
        {
            std::string query = "INSERT INTO character_saved_replays (character_id, replay_id) VALUES (" + std::to_string(playerGuid) + ", " + std::to_string(code) + ")";
            CharacterDatabase.Execute(query.c_str());

            if (Player* player = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(playerGuid)))
            {
                ChatHandler(player->GetSession()).PSendModuleSysMessage(MODULE_STRING, STRING_FAVORITE_SAVED, code);
                CloseGossipMenuFor(player);
            }
        }
        else
        {
            if (Player* player = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(playerGuid)))
            {
                ChatHandler(player->GetSession()).PSendModuleSysMessage(MODULE_STRING, STRING_FAVORITE_NOT_FOUND, code);
                CloseGossipMenuFor(player);
            }
        }
    }

public:
    static bool replayArenaMatch(Player* player, uint32 replayId)
    {
        auto handler = ChatHandler(player->GetSession());

        if (player->InBattlegroundQueue())
        {
            handler.PSendModuleSysMessage(MODULE_STRING, STRING_IN_QUEUE);
            return false;
        }

        if (!loadReplayDataForPlayer(player, replayId))
        {
            CloseGossipMenuFor(player);
            return false;
        }

        MatchRecord record = loadedReplays[player->GetGUID().GetCounter()];

        Battleground* bg = sBattlegroundMgr->CreateNewBattleground(record.typeId, GetBattlegroundBracketByLevel(record.mapId, sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL)), record.arenaTypeId, false);
        if (!bg)
        {
            handler.PSendModuleSysMessage(MODULE_STRING, STRING_ARENA_MAP_FAILED);
            handler.SetSentErrorMessage(true);
            return false;
        }

        bgReplayIds[bg->GetInstanceID()] = player->GetGUID().GetCounter();
        player->SetPendingSpectatorForBG(bg->GetInstanceID());
        bg->StartBattleground();

        BattlegroundTypeId bgTypeId = bg->GetBgTypeID();

        TeamId teamId = Player::TeamIdForRace(player->getRace());

        uint32 queueSlot = 0;
        WorldPacket data;

        player->SetBattlegroundId(bg->GetInstanceID(), bgTypeId, queueSlot, true, false, TEAM_NEUTRAL);
        player->SetEntryPoint();
        sBattlegroundMgr->SendToBattleground(player, bg->GetInstanceID(), bgTypeId);
        sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bg, queueSlot, STATUS_IN_PROGRESS, 0, bg->GetStartTime(), bg->GetArenaType(), teamId);
        player->GetSession()->SendPacket(&data);
        handler.PSendModuleSysMessage(MODULE_STRING, STRING_REPLAY_BEGINS, replayId);

        return true;
    }

private:
    static bool loadReplayDataForPlayer(Player* p, uint32 matchId)
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT id, arenaTypeId, typeId, contentSize, contents, mapId, "
            "timesWatched, winnerPlayerGuids, loserPlayerGuids "
            "FROM character_arena_replays WHERE id = {}", matchId);
        if (!result)
        {
            ChatHandler(p->GetSession()).PSendModuleSysMessage(MODULE_STRING, STRING_REPLAY_DATA_NOT_FOUND);
            CloseGossipMenuFor(p);
            return false;
        }

        Field* fields = result->Fetch();
        if (!fields)
        {
            ChatHandler(p->GetSession()).PSendModuleSysMessage(MODULE_STRING, STRING_REPLAY_DATA_NOT_FOUND);
            CloseGossipMenuFor(p);
            return false;
        }

        ObjectGuid cloneGuid;
        if (IsReplayParticipant(fields[7].Get<std::string>(), p->GetGUID()) ||
            IsReplayParticipant(fields[8].Get<std::string>(), p->GetGUID()))
        {
            cloneGuid = GetReplayCloneGuid(p->GetGUID());
            if (!IsReplayCloneGuidSafe(cloneGuid))
            {
                ChatHandler(p->GetSession()).PSendModuleSysMessage(
                    MODULE_STRING, STRING_REPLAY_OWN_MATCH_BLOCKED);
                CloseGossipMenuFor(p);
                return false;
            }
        }

        MatchRecord record;
        deserializeMatchData(record, fields);

        auto const isCompressed = [](PacketRecord const& packetRecord)
        {
            return packetRecord.packet.GetOpcode() ==
                SMSG_COMPRESSED_UPDATE_OBJECT;
        };

        // Recorded before the core compressed in the socket: the rewriter
        // can't read these, and dropping them leaves players out.
        if (!cloneGuid.IsEmpty() && std::any_of(record.packets.begin(),
            record.packets.end(), isCompressed))
        {
            ChatHandler(p->GetSession()).PSendModuleSysMessage(
                MODULE_STRING, STRING_REPLAY_OWN_MATCH_BLOCKED);
            CloseGossipMenuFor(p);
            return false;
        }

        // Update 'timesWatched' of a Replay +1 everytime someone watches it
        uint32 timesWatched = fields[6].Get<uint32>();
        timesWatched++;
        CharacterDatabase.Execute("UPDATE character_arena_replays SET timesWatched = {} WHERE id = {}", timesWatched, matchId);

        if (!cloneGuid.IsEmpty())
            rewriteReplayForClone(p, record, cloneGuid, matchId);

        loadedReplays[p->GetGUID().GetCounter()] = std::move(record);
        return true;
    }

    // The recorded packets carry the watcher's own GUID, which its client
    // would apply to the watcher itself.
    static void rewriteReplayForClone(Player* watcher, MatchRecord& record,
        ObjectGuid cloneGuid, uint32 matchId)
    {
        WorldPacket const cloneName =
            BuildCloneNameQueryResponse(watcher, cloneGuid);
        std::deque<PacketRecord> packets;
        uint32 unreadable = 0;

        for (PacketRecord& packetRecord : record.packets)
        {
            switch (RewriteReplayPacketForClone(packetRecord.packet,
                watcher->GetGUID(), cloneGuid))
            {
                case CloneRewriteResult::Send:
                    packets.push_back(std::move(packetRecord));
                    break;
                case CloneRewriteResult::CloneNameQuery:
                    packets.push_back({ packetRecord.timestamp, cloneName });
                    break;
                case CloneRewriteResult::DropUnreadable:
                    ++unreadable;
                    break;
                case CloneRewriteResult::Drop:
                    break;
            }
        }

        if (unreadable)
            LOG_WARN("module", "ArenaReplay: dropped {} unreadable packets "
                "from replay {} for {}", unreadable, matchId,
                watcher->GetGUID().ToString());

        record.packets = std::move(packets);
    }

    static void deserializeMatchData(MatchRecord& record, Field* fields)
    {
        record.arenaTypeId = uint8(fields[1].Get<uint32>());
        record.typeId = BattlegroundTypeId(fields[2].Get<uint32>());
        std::vector<uint8> data = *Acore::Encoding::Base32::Decode(fields[4].Get<std::string>());
        record.mapId = uint32(fields[5].Get<uint32>());
        ByteBuffer buffer;
        buffer.append(&data[0], data.size());

        /** deserialize replay binary data **/
        uint32 packetSize;
        uint32 packetTimestamp;
        uint16 opcode;
        while (buffer.rpos() <= buffer.size() - 1)
        {
            buffer >> packetSize;
            buffer >> packetTimestamp;
            buffer >> opcode;

            WorldPacket packet(opcode, packetSize);

            if (packetSize > 0)
            {
                std::vector<uint8> tmp(packetSize, 0);
                buffer.read(&tmp[0], packetSize);
                packet.append(&tmp[0], packetSize);
            }

            // replays stored before the chat filter existed still carry every chat type
            if (!IsReplayableChatPacket(packet))
                continue;

            record.packets.push_back({ packetTimestamp, packet });
        }
    }
};

static bool ArenaReplayEnable = true;
static bool ReplayCommandEnable = true;

using namespace Acore::ChatCommands;

class ArenaReplayCommandScript : public CommandScript
{
public:
    ArenaReplayCommandScript() : CommandScript("ArenaReplayCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "replay", HandleReplayCommand, SEC_PLAYER, Console::No }
        };

        return commandTable;
    }

    static char const* GetBlockReason(Player const* player)
    {
        if (player->IsBeingTeleported() || !player->IsInWorld())
            return "Can't use while being teleported.";

        if (!player->FindMap() || player->FindMap()->Instanceable())
            return "Can't use while in instance, bg or arena.";

        if (player->GetVehicle())
            return "Can't be on a vehicle.";

        if (player->IsInCombat())
            return "Can't be in combat.";

        if (player->InBattlegroundQueue())
            return "Can't be queued for arena or bg.";

        if (player->IsInFlight())
            return "Can't be in flight.";

        if (!player->IsAlive())
            return "Must be alive.";

        return nullptr;
    }

    static bool HandleReplayCommand(ChatHandler* handler, uint32 replayId)
    {
        Player* player = handler->GetPlayer();
        char const* reason = (!ArenaReplayEnable || !ReplayCommandEnable) ?
            "Arena Replay disabled!" : GetBlockReason(player);
        if (reason)
        {
            handler->SendSysMessage(reason);
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!ReplayGossip::replayArenaMatch(player, replayId))
        {
            handler->SetSentErrorMessage(true);
            return false;
        }

        return true;
    }
};

class ConfigLoaderArenaReplay : public WorldScript
{
public:
    ConfigLoaderArenaReplay() : WorldScript("config_loader_arena_replay", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD
    }) {}
    virtual void OnAfterConfigLoad(bool /*Reload*/) override
    {
        ArenaReplayEnable =
            sConfigMgr->GetOption<bool>("ArenaReplay.Enable", true);
        ReplayCommandEnable =
            sConfigMgr->GetOption<bool>("ArenaReplay.Command.Enable", true);
        DeleteOldReplays();
    }

private:
    void DeleteOldReplays()
    {
        // delete all the replays older than X days
        const auto days = sConfigMgr->GetOption<uint32>("ArenaReplay.DeleteReplaysAfterDays", 30);
        if (days > 0)
        {
            std::string addition = "";

            const bool deleteSavedReplays = sConfigMgr->GetOption<bool>("ArenaReplay.DeleteSavedReplays", false);

            if (!deleteSavedReplays)
                addition = "AND `id` NOT IN (SELECT `replay_id` FROM `character_saved_replays`)";

            const auto query = "DELETE FROM `character_arena_replays` WHERE `timestamp` < (NOW() - INTERVAL " + std::to_string(days) + " DAY) " + addition;
            CharacterDatabase.Execute(query);

            if (deleteSavedReplays)
                CharacterDatabase.Execute("DELETE FROM `character_saved_replays` WHERE `replay_id` NOT IN (SELECT `id` FROM `character_arena_replays`)");
        }
    }
};

void AddArenaReplayScripts()
{
    new ConfigLoaderArenaReplay();
    new ArenaReplayServerScript();
    new ArenaReplayBGScript();
    new ArenaReplayArenaScript();
    new ReplayGossip();
    new ArenaReplayCommandScript();
}
