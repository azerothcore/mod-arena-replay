#include "ArenaReplayCloneRewriter.h"
#include "MoveSplineFlag.h"
#include "SharedDefines.h"
#include "SpellAuraDefines.h"
#include "SpellInfo.h"
#include "UnitDefines.h"
#include "UpdateData.h"
#include "UpdateFields.h"
#include <span>
#include <vector>
#include <zlib.h>

namespace
{
    // keeps the inflated buffer below ByteBuffer's 10,000,000 byte ASSERT
    constexpr uint32 MaxUncompressedUpdateSize = 9000000;
    constexpr uint8 MonsterMoveFacingTarget = 3;
    constexpr std::size_t NoValue = std::size_t(-1);

    constexpr uint16 ObjectGuidFields[] = { OBJECT_FIELD_GUID };

    constexpr uint16 UnitGuidFields[] =
    {
        OBJECT_FIELD_GUID, UNIT_FIELD_CHARM, UNIT_FIELD_SUMMON,
        UNIT_FIELD_CRITTER, UNIT_FIELD_CHARMEDBY, UNIT_FIELD_SUMMONEDBY,
        UNIT_FIELD_CREATEDBY, UNIT_FIELD_TARGET, UNIT_FIELD_CHANNEL_OBJECT
    };

    constexpr uint16 PlayerGuidFields[] =
    {
        OBJECT_FIELD_GUID, UNIT_FIELD_CHARM, UNIT_FIELD_SUMMON,
        UNIT_FIELD_CRITTER, UNIT_FIELD_CHARMEDBY, UNIT_FIELD_SUMMONEDBY,
        UNIT_FIELD_CREATEDBY, UNIT_FIELD_TARGET, UNIT_FIELD_CHANNEL_OBJECT,
        PLAYER_DUEL_ARBITER, PLAYER_FARSIGHT
    };

    constexpr uint16 GameObjectGuidFields[] =
    {
        OBJECT_FIELD_GUID, OBJECT_FIELD_CREATED_BY
    };

    constexpr uint16 DynamicObjectGuidFields[] =
    {
        OBJECT_FIELD_GUID, DYNAMICOBJECT_CASTER
    };

    constexpr uint16 CorpseGuidFields[] =
    {
        OBJECT_FIELD_GUID, CORPSE_FIELD_OWNER, CORPSE_FIELD_PARTY
    };

    std::span<uint16 const> GetGuidFields(uint8 typeId)
    {
        switch (typeId)
        {
            case TYPEID_UNIT:
                return UnitGuidFields;
            case TYPEID_PLAYER:
                return PlayerGuidFields;
            case TYPEID_GAMEOBJECT:
                return GameObjectGuidFields;
            case TYPEID_DYNAMICOBJECT:
                return DynamicObjectGuidFields;
            case TYPEID_CORPSE:
                return CorpseGuidFields;
            default:
                return ObjectGuidFields;
        }
    }

    template <typename T>
    T Copy(ByteBuffer& in, ByteBuffer& out)
    {
        T value = in.read<T>();
        out.append<T>(value);
        return value;
    }

    void CopyBytes(ByteBuffer& in, ByteBuffer& out, std::size_t count)
    {
        if (!count)
            return;

        std::size_t const start = in.rpos();
        in.read_skip(count);
        out.append(in.contents() + start, count);
    }

    void CopyTail(ByteBuffer& in, ByteBuffer& out)
    {
        CopyBytes(in, out, in.size() - in.rpos());
    }

    ByteBuffer Decompress(ByteBuffer& in)
    {
        uint32 const size = in.read<uint32>();
        if (!size || size > MaxUncompressedUpdateSize)
            throw ByteBufferInvalidValueException("uncompressed size",
                "out of range");

        ByteBuffer body(size);
        body.resize(size);
        uLongf bodySize = size;
        int const result = uncompress(body.contents(), &bodySize,
            in.contents() + in.rpos(), in.size() - in.rpos());
        if (result != Z_OK || bodySize != size)
            throw ByteBufferInvalidValueException("zlib stream", "corrupt");

        in.rfinish();
        return body;
    }

    class CloneRewriter
    {
    public:
        CloneRewriter(ObjectGuid realGuid, ObjectGuid cloneGuid)
            : _real(realGuid.GetRawValue()), _clone(cloneGuid.GetRawValue())
        {
        }

        CloneRewriteResult Rewrite(WorldPacket& packet) const;

    private:
        uint64 Substitute(uint64 raw) const
        {
            return raw == _real ? _clone : raw;
        }

        uint64 CopyPackedGuid(ByteBuffer& in, ByteBuffer& out) const
        {
            uint64 raw;
            in.readPackGUID(raw);
            out.appendPackGUID(Substitute(raw));
            return raw;
        }

        uint64 CopyGuid(ByteBuffer& in, ByteBuffer& out) const
        {
            uint64 const raw = in.read<uint64>();
            out << Substitute(raw);
            return raw;
        }

        uint32 CopyMovementInfo(ByteBuffer& in, ByteBuffer& out) const;
        void CopySpline(ByteBuffer& in, ByteBuffer& out) const;
        void CopyMovementUpdate(ByteBuffer& in, ByteBuffer& out,
            bool isClone) const;
        void CopyValuesUpdate(ByteBuffer& in, ByteBuffer& out,
            uint8 typeId) const;
        bool CopyUpdateBlock(ByteBuffer& in, ByteBuffer& out) const;
        bool CopyUpdateObject(ByteBuffer& in, ByteBuffer& out) const;
        void CopyAuraUpdate(ByteBuffer& in, ByteBuffer& out) const;
        void CopySpellCastTargets(ByteBuffer& in, ByteBuffer& out) const;
        void CopySpellGo(ByteBuffer& in, ByteBuffer& out) const;
        void CopyChat(ByteBuffer& in, ByteBuffer& out) const;

        uint64 _real;
        uint64 _clone;
    };

    // MovementInfo without its leading guid; returns the movement flags
    uint32 CloneRewriter::CopyMovementInfo(ByteBuffer& in,
        ByteBuffer& out) const
    {
        uint32 const flags = Copy<uint32>(in, out);
        uint16 const flags2 = Copy<uint16>(in, out);
        CopyBytes(in, out, sizeof(uint32) + 4 * sizeof(float));

        if (flags & MOVEMENTFLAG_ONTRANSPORT)
        {
            CopyPackedGuid(in, out);
            CopyBytes(in, out,
                4 * sizeof(float) + sizeof(uint32) + sizeof(int8));

            if (flags2 & MOVEMENTFLAG2_INTERPOLATED_MOVEMENT)
                CopyBytes(in, out, sizeof(uint32));
        }

        if ((flags & (MOVEMENTFLAG_SWIMMING | MOVEMENTFLAG_FLYING)) ||
            (flags2 & MOVEMENTFLAG2_ALWAYS_ALLOW_PITCHING))
            CopyBytes(in, out, sizeof(float));

        CopyBytes(in, out, sizeof(uint32));

        if (flags & MOVEMENTFLAG_FALLING)
            CopyBytes(in, out, 4 * sizeof(float));

        if (flags & MOVEMENTFLAG_SPLINE_ELEVATION)
            CopyBytes(in, out, sizeof(float));

        return flags;
    }

    void CloneRewriter::CopySpline(ByteBuffer& in, ByteBuffer& out) const
    {
        using Movement::MoveSplineFlag;

        uint32 const splineFlags = Copy<uint32>(in, out);
        if (splineFlags & MoveSplineFlag::Final_Angle)
            CopyBytes(in, out, sizeof(float));
        else if (splineFlags & MoveSplineFlag::Final_Target)
            CopyGuid(in, out);
        else if (splineFlags & MoveSplineFlag::Final_Point)
            CopyBytes(in, out, 3 * sizeof(float));

        CopyBytes(in, out,
            3 * sizeof(int32) + sizeof(uint32) + 3 * sizeof(float));

        uint32 const nodes = Copy<uint32>(in, out);
        CopyBytes(in, out, std::size_t(nodes) * 3 * sizeof(float));
        CopyBytes(in, out, sizeof(uint8) + 3 * sizeof(float));
    }

    void CloneRewriter::CopyMovementUpdate(ByteBuffer& in, ByteBuffer& out,
        bool isClone) const
    {
        uint16 flags = in.read<uint16>();
        // a recorded self-create would make the client control the clone
        if (isClone)
            flags &= ~UPDATEFLAG_SELF;
        out << flags;

        if (flags & UPDATEFLAG_LIVING)
        {
            uint32 const moveFlags = CopyMovementInfo(in, out);
            CopyBytes(in, out, 9 * sizeof(float));

            if (moveFlags & MOVEMENTFLAG_SPLINE_ENABLED)
                CopySpline(in, out);
        }
        else if (flags & UPDATEFLAG_POSITION)
        {
            CopyPackedGuid(in, out);
            CopyBytes(in, out, 8 * sizeof(float));
        }
        else if (flags & UPDATEFLAG_STATIONARY_POSITION)
            CopyBytes(in, out, 4 * sizeof(float));

        if (flags & UPDATEFLAG_UNKNOWN)
            CopyBytes(in, out, sizeof(uint32));

        if (flags & UPDATEFLAG_LOWGUID)
            CopyBytes(in, out, sizeof(uint32));

        if (flags & UPDATEFLAG_HAS_TARGET)
            CopyPackedGuid(in, out);

        if (flags & UPDATEFLAG_TRANSPORT)
            CopyBytes(in, out, sizeof(uint32));

        if (flags & UPDATEFLAG_VEHICLE)
            CopyBytes(in, out, sizeof(uint32) + sizeof(float));

        if (flags & UPDATEFLAG_ROTATION)
            CopyBytes(in, out, sizeof(int64));
    }

    void CloneRewriter::CopyValuesUpdate(ByteBuffer& in, ByteBuffer& out,
        uint8 typeId) const
    {
        uint8 const maskBlocks = Copy<uint8>(in, out);
        std::vector<uint32> mask(maskBlocks);
        for (uint32& block : mask)
            block = Copy<uint32>(in, out);

        std::vector<std::size_t> valuePos(mask.size() * 32, NoValue);
        for (std::size_t index = 0; index < valuePos.size(); ++index)
        {
            if (!(mask[index / 32] & (1u << (index % 32))))
                continue;

            valuePos[index] = out.wpos();
            CopyBytes(in, out, sizeof(uint32));
        }

        auto valueAt = [&](std::size_t index) -> uint64
        {
            if (index >= valuePos.size() || valuePos[index] == NoValue)
                return 0;

            return out.read<uint32>(valuePos[index]);
        };

        // create blocks leave out zero fields, so a missing high half is 0;
        // the mask stays valid because both GUIDs share their high half
        for (uint16 field : GetGuidFields(typeId))
        {
            if (field >= valuePos.size() || valuePos[field] == NoValue)
                continue;

            uint64 const value = valueAt(field) | (valueAt(field + 1) << 32);
            if (value == _real)
                out.put<uint32>(valuePos[field], uint32(_clone));
        }
    }

    // Returns false when the block has to be left out.
    bool CloneRewriter::CopyUpdateBlock(ByteBuffer& in, ByteBuffer& out) const
    {
        uint8 const updateType = Copy<uint8>(in, out);
        switch (updateType)
        {
            case UPDATETYPE_OUT_OF_RANGE_OBJECTS:
            case UPDATETYPE_NEAR_OBJECTS:
            {
                uint32 const count = in.read<uint32>();
                std::vector<uint64> guids;
                for (uint32 i = 0; i < count; ++i)
                {
                    uint64 raw;
                    in.readPackGUID(raw);
                    if (!ObjectGuid(raw).IsItem())
                        guids.push_back(Substitute(raw));
                }

                out << uint32(guids.size());
                for (uint64 raw : guids)
                    out.appendPackGUID(raw);

                return !guids.empty();
            }
            case UPDATETYPE_VALUES:
            {
                ObjectGuid const guid(CopyPackedGuid(in, out));
                CopyValuesUpdate(in, out, guid.GetTypeId());
                return !guid.IsItem();
            }
            case UPDATETYPE_MOVEMENT:
            {
                ObjectGuid const guid(CopyPackedGuid(in, out));
                CopyMovementUpdate(in, out, guid.GetRawValue() == _real);
                return !guid.IsItem();
            }
            case UPDATETYPE_CREATE_OBJECT:
            case UPDATETYPE_CREATE_OBJECT2:
            {
                ObjectGuid const guid(CopyPackedGuid(in, out));
                uint8 const typeId = Copy<uint8>(in, out);
                CopyMovementUpdate(in, out, guid.GetRawValue() == _real);
                CopyValuesUpdate(in, out, typeId);
                return !guid.IsItem();
            }
            default:
                throw ByteBufferInvalidValueException("update type",
                    "unknown");
        }
    }

    // Returns false when no block is left.
    bool CloneRewriter::CopyUpdateObject(ByteBuffer& in, ByteBuffer& out) const
    {
        uint32 const blockCount = in.read<uint32>();
        std::size_t const countPos = out.wpos();
        out << uint32(0);

        uint32 kept = 0;
        for (uint32 i = 0; i < blockCount; ++i)
        {
            ByteBuffer block(64);
            if (!CopyUpdateBlock(in, block))
                continue;

            out.append(block);
            ++kept;
        }

        out.put<uint32>(countPos, kept);
        return kept > 0;
    }

    void CloneRewriter::CopyAuraUpdate(ByteBuffer& in, ByteBuffer& out) const
    {
        CopyPackedGuid(in, out);
        while (in.rpos() < in.size())
        {
            CopyBytes(in, out, sizeof(uint8));
            if (!Copy<uint32>(in, out))
                continue;

            uint8 const flags = Copy<uint8>(in, out);
            CopyBytes(in, out, 2 * sizeof(uint8));

            if (!(flags & AFLAG_CASTER))
                CopyPackedGuid(in, out);

            if (flags & AFLAG_DURATION)
                CopyBytes(in, out, 2 * sizeof(uint32));
        }
    }

    void CloneRewriter::CopySpellCastTargets(ByteBuffer& in,
        ByteBuffer& out) const
    {
        uint32 const targetMask = Copy<uint32>(in, out);

        if (targetMask & (TARGET_FLAG_UNIT | TARGET_FLAG_CORPSE_ALLY |
            TARGET_FLAG_GAMEOBJECT | TARGET_FLAG_CORPSE_ENEMY |
            TARGET_FLAG_UNIT_MINIPET))
            CopyPackedGuid(in, out);

        if (targetMask & (TARGET_FLAG_ITEM | TARGET_FLAG_TRADE_ITEM))
            CopyPackedGuid(in, out);

        if (targetMask & TARGET_FLAG_SOURCE_LOCATION)
        {
            CopyPackedGuid(in, out);
            CopyBytes(in, out, 3 * sizeof(float));
        }

        if (targetMask & TARGET_FLAG_DEST_LOCATION)
        {
            CopyPackedGuid(in, out);
            CopyBytes(in, out, 3 * sizeof(float));
        }
    }

    void CloneRewriter::CopySpellGo(ByteBuffer& in, ByteBuffer& out) const
    {
        CopyPackedGuid(in, out);
        CopyPackedGuid(in, out);
        CopyBytes(in, out, sizeof(uint8) + 3 * sizeof(uint32));

        uint8 const hitCount = Copy<uint8>(in, out);
        for (uint8 i = 0; i < hitCount; ++i)
            CopyGuid(in, out);

        uint8 const missCount = Copy<uint8>(in, out);
        for (uint8 i = 0; i < missCount; ++i)
        {
            CopyGuid(in, out);
            if (Copy<uint8>(in, out) == SPELL_MISS_REFLECT)
                CopyBytes(in, out, sizeof(uint8));
        }

        CopySpellCastTargets(in, out);
    }

    void CloneRewriter::CopyChat(ByteBuffer& in, ByteBuffer& out) const
    {
        uint8 const chatType = Copy<uint8>(in, out);
        CopyBytes(in, out, sizeof(int32));
        CopyGuid(in, out);
        CopyBytes(in, out, sizeof(uint32));

        switch (chatType)
        {
            case CHAT_MSG_MONSTER_SAY:
            case CHAT_MSG_MONSTER_YELL:
            case CHAT_MSG_MONSTER_EMOTE:
                CopyBytes(in, out, Copy<uint32>(in, out));
                break;
            default:
                break;
        }

        CopyGuid(in, out);
    }

    CloneRewriteResult CloneRewriter::Rewrite(WorldPacket& packet) const
    {
        packet.rpos(0);
        WorldPacket out(packet.GetOpcode(), packet.size() + 16);

        switch (packet.GetOpcode())
        {
            case SMSG_NOTIFICATION:
            case SMSG_WORLD_STATE_UI_TIMER_UPDATE:
            case SMSG_CAST_FAILED:
            case SMSG_PET_NAME_QUERY_RESPONSE:
            case SMSG_GAMEOBJECT_QUERY_RESPONSE:
            case SMSG_CANCEL_COMBAT:
            case SMSG_MOUNTRESULT:
            case SMSG_DISMOUNTRESULT:
            case SMSG_MIRRORIMAGE_DATA:
                return CloneRewriteResult::Send;
            case SMSG_NAME_QUERY_RESPONSE:
            {
                uint64 raw;
                packet.readPackGUID(raw);
                return raw == _real ? CloneRewriteResult::CloneNameQuery
                    : CloneRewriteResult::Send;
            }
            case SMSG_UPDATE_OBJECT:
                if (!CopyUpdateObject(packet, out))
                    return CloneRewriteResult::Drop;
                break;
            case SMSG_COMPRESSED_UPDATE_OBJECT:
            {
                // recorded before the network thread took over compression;
                // the socket compresses the rewritten packet again
                ByteBuffer body = Decompress(packet);
                out.SetOpcode(SMSG_UPDATE_OBJECT);
                if (!CopyUpdateObject(body, out))
                    return CloneRewriteResult::Drop;
                break;
            }
            case SMSG_DESTROY_OBJECT:
                if (ObjectGuid(packet.read<uint64>(0)).IsItem())
                    return CloneRewriteResult::Drop;
                CopyGuid(packet, out);
                break;
            case MSG_MOVE_START_FORWARD:
            case MSG_MOVE_START_BACKWARD:
            case MSG_MOVE_STOP:
            case MSG_MOVE_START_STRAFE_LEFT:
            case MSG_MOVE_START_STRAFE_RIGHT:
            case MSG_MOVE_STOP_STRAFE:
            case MSG_MOVE_JUMP:
            case MSG_MOVE_START_TURN_LEFT:
            case MSG_MOVE_START_TURN_RIGHT:
            case MSG_MOVE_STOP_TURN:
            case MSG_MOVE_FALL_LAND:
            case MSG_MOVE_SET_FACING:
            case MSG_MOVE_HEARTBEAT:
                CopyPackedGuid(packet, out);
                CopyMovementInfo(packet, out);
                break;
            case SMSG_MONSTER_MOVE:
                CopyPackedGuid(packet, out);
                CopyBytes(packet, out,
                    sizeof(uint8) + 3 * sizeof(float) + sizeof(uint32));
                if (Copy<uint8>(packet, out) == MonsterMoveFacingTarget)
                    CopyGuid(packet, out);
                break;
            case SMSG_FORCE_RUN_SPEED_CHANGE:
            case SMSG_FORCE_SWIM_SPEED_CHANGE:
            case SMSG_FORCE_FLIGHT_SPEED_CHANGE:
            case SMSG_SPELL_FAILURE:
            case SMSG_SPELL_DELAYED:
            case SMSG_POWER_UPDATE:
            case SMSG_CANCEL_AUTO_REPEAT:
            case SMSG_DISMOUNT:
                CopyPackedGuid(packet, out);
                break;
            case SMSG_PERIODICAURALOG:
            case SMSG_SPELLHEALLOG:
            case SMSG_SPELLENERGIZELOG:
            case SMSG_SPELLNONMELEEDAMAGELOG:
            case SMSG_ATTACKSTOP:
                CopyPackedGuid(packet, out);
                CopyPackedGuid(packet, out);
                break;
            case SMSG_ATTACKERSTATEUPDATE:
                CopyBytes(packet, out, sizeof(uint32));
                CopyPackedGuid(packet, out);
                CopyPackedGuid(packet, out);
                break;
            case SMSG_SPELLDAMAGESHIELD:
            case SMSG_ATTACKSTART:
                CopyGuid(packet, out);
                CopyGuid(packet, out);
                break;
            case SMSG_EMOTE:
                CopyBytes(packet, out, sizeof(uint32));
                CopyGuid(packet, out);
                break;
            case SMSG_PLAY_SPELL_IMPACT:
            case SMSG_MOUNTSPECIAL_ANIM:
            case SMSG_ARENA_UNIT_DESTROYED:
            case SMSG_AI_REACTION:
            case SMSG_GAMEOBJECT_DESPAWN_ANIM:
                CopyGuid(packet, out);
                break;
            case SMSG_AURA_UPDATE:
            case SMSG_AURA_UPDATE_ALL:
                CopyAuraUpdate(packet, out);
                break;
            case SMSG_SPELL_START:
                CopyPackedGuid(packet, out);
                CopyPackedGuid(packet, out);
                CopyBytes(packet, out,
                    sizeof(uint8) + 2 * sizeof(uint32) + sizeof(int32));
                CopySpellCastTargets(packet, out);
                break;
            case SMSG_SPELL_GO:
                CopySpellGo(packet, out);
                break;
            case SMSG_MESSAGECHAT:
                CopyChat(packet, out);
                break;
            default:
                return CloneRewriteResult::DropUnreadable;
        }

        CopyTail(packet, out);
        packet = std::move(out);
        return CloneRewriteResult::Send;
    }
}

CloneRewriteResult RewriteReplayPacketForClone(WorldPacket& packet,
    ObjectGuid realGuid, ObjectGuid cloneGuid)
{
    try
    {
        return CloneRewriter(realGuid, cloneGuid).Rewrite(packet);
    }
    catch (ByteBufferException const&)
    {
        return CloneRewriteResult::DropUnreadable;
    }
}
