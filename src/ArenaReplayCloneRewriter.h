#ifndef _MOD_ARENA_REPLAY_CLONE_REWRITER_H_
#define _MOD_ARENA_REPLAY_CLONE_REWRITER_H_

#include "ObjectGuid.h"
#include "WorldPacket.h"

enum class CloneRewriteResult
{
    Send,
    Drop,
    // unparseable or unknown opcode, it may still carry the real GUID
    DropUnreadable,
    // SMSG_NAME_QUERY_RESPONSE for the real GUID, the caller replaces it
    CloneNameQuery
};

// Rewrites a recorded replay packet in place so every reference to realGuid
// points to cloneGuid. Drops every item object: the replay holds the
// recording players' inventories, and one of them can be the watcher's.
// realGuid and cloneGuid must differ only in their low 32 bits.
CloneRewriteResult RewriteReplayPacketForClone(WorldPacket& packet,
    ObjectGuid realGuid, ObjectGuid cloneGuid);

#endif
