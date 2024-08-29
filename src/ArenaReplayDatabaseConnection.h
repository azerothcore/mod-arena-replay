#ifndef _MOD_ARENA_REPLAY_DATABASE_CONNECTION_H_
#define _MOD_ARENA_REPLAY_DATABASE_CONNECTION_H_

#include "Database/MySQLConnection.h"
#include "PreparedStatement.h"
#include "ByteBuffer.h"
#include "DatabaseEnv.h"

// enum ArenaReplayDatabaseStatements : uint32 {
//     CHAR_INS_ARENA_REPLAYS
// };

class ArenaReplayByteBuffer : public ByteBuffer {
    public:
        std::vector<uint8> contentsAsVector()
        {
            return _storage;
        }
};

// class AC_DATABASE_API ArenaReplayDatabaseConnection : public MySQLConnection
// {
//     public:
//         typedef ArenaReplayDatabaseStatements Statements;

//         ArenaReplayDatabaseConnection(MySQLConnectionInfo& connInfo) : MySQLConnection(connInfo) {}
//         ArenaReplayDatabaseConnection(ProducerConsumerQueue<SQLOperation*>* q, MySQLConnectionInfo& connInfo) : MySQLConnection(q, connInfo) {}

//         void DoPrepareStatements() override {
//             // Arena Replay
//             PrepareStatement(CHAR_INS_ARENA_REPLAYS, "INSERT INTO character_arena_replays (arenaTypeId, typeId, contentSize, contents, mapId) VALUES (?, ?, ?, ?, ?)", CONNECTION_ASYNC);
//         };
// };

// AC_DATABASE_API extern DatabaseWorkerPool<ArenaReplayDatabaseConnection> ArenaReplayDatabase;

#endif
