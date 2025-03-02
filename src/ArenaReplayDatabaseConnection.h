#ifndef _MOD_ARENA_REPLAY_DATABASE_CONNECTION_H_
#define _MOD_ARENA_REPLAY_DATABASE_CONNECTION_H_

#include "ByteBuffer.h"

class ArenaReplayByteBuffer : public ByteBuffer
{
    public:
        std::vector<uint8> contentsAsVector()
        {
            return _storage;
        }
};

#endif
