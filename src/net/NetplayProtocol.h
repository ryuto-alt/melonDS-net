/*
    Copyright 2016-2025 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef NETPLAYPROTOCOL_H
#define NETPLAYPROTOCOL_H

#include <vector>
#include <string>
#include <queue>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>

#include <enet/enet.h>

#include "types.h"

namespace melonDS
{

// ---- Protocol constants ----

constexpr u32 kNetplayMagic = 0x504E4C4D; // "MLNP"
constexpr u32 kNetplayVersion = 1;
constexpr int kNetplayDefaultPort = 7065;
constexpr int kNetplayMaxPlayers = 4;

// ENet channels
enum NetplayChannel
{
    Chan_Control = 0,   // reliable control messages
    Chan_Input = 1,     // unreliable input frames
    Chan_MAX = 2,
};

// Control message types (channel 0, reliable)
enum NetplayMsgType : u8
{
    Msg_SessionOffer    = 0x10,
    Msg_SessionAccept   = 0x11,
    Msg_BlobStart       = 0x12,
    Msg_BlobChunk       = 0x13,
    Msg_BlobEnd         = 0x14,
    Msg_SyncReady       = 0x15,
    Msg_StartGame       = 0x16,
    Msg_DesyncAlert     = 0x20,
    Msg_Disconnect      = 0xFF,
};

// Input message types (channel 1)
enum NetplayInputMsgType : u8
{
    Msg_InputFrame      = 0x30,
    Msg_InputBatch      = 0x31,
};

// Blob types for state transfer
enum NetplayBlobType : u8
{
    Blob_SRAM = 0,
    Blob_Savestate0 = 1,
    Blob_Savestate1 = 2,
    Blob_Savestate2 = 3,
    Blob_Savestate3 = 4,
    Blob_MAX,
};

// Disconnect reasons
enum NetplayDisconnectReason : u8
{
    Disconnect_Normal = 0,
    Disconnect_Desync = 1,
    Disconnect_Error = 2,
};

// ---- Data structures ----

#pragma pack(push, 1)

struct InputFrame
{
    u32 FrameNum;
    u32 KeyMask;      // 12-bit button mask
    u8  Touching;
    u16 TouchX;
    u16 TouchY;
    u8  LidClosed;
    u32 Checksum;     // simple validation
};
static_assert(sizeof(InputFrame) == 18, "InputFrame must be 18 bytes packed");

struct NetplayHeader
{
    u8 Type;
};

struct MsgSessionOffer
{
    u8 Type;        // Msg_SessionOffer
    u64 ROMHash;
    u8 NumPlayers;
    u8 InputDelay;
};

struct MsgSessionAccept
{
    u8 Type;        // Msg_SessionAccept
    u8 PlayerID;
};

struct MsgBlobStart
{
    u8 Type;        // Msg_BlobStart
    u8 BlobType;
    u32 TotalLen;
};

struct MsgBlobChunk
{
    u8 Type;        // Msg_BlobChunk
    u32 Offset;
    // followed by data bytes (up to 64KB)
};

struct MsgBlobEnd
{
    u8 Type;        // Msg_BlobEnd
    u8 BlobType;
    u32 Checksum;
};

struct MsgSyncReady
{
    u8 Type;        // Msg_SyncReady
};

struct MsgStartGame
{
    u8 Type;        // Msg_StartGame
    u32 Frame;
    u8 InputDelay;
};

struct MsgDesyncAlert
{
    u8 Type;        // Msg_DesyncAlert
    u32 Frame;
    u64 Hash;
};

struct MsgDisconnect
{
    u8 Type;        // Msg_Disconnect
    u8 Reason;
};

struct MsgInputFrame
{
    u8 Type;        // Msg_InputFrame
    InputFrame Input;
};

struct MsgInputBatch
{
    u8 Type;        // Msg_InputBatch
    u8 Count;
    // followed by InputFrame[Count]
};

#pragma pack(pop)

// ---- Blob transfer helper ----

constexpr u32 kBlobChunkSize = 0x10000; // 64KB chunks

// ---- Compression helpers ----

bool CompressData(const void* src, u32 srcLen, std::vector<u8>& dst);
bool DecompressData(const u8* src, u32 srcLen, void* dst, u32 dstLen);

// ---- Network transport ----

class NetplayTransport
{
public:
    NetplayTransport();
    ~NetplayTransport();

    // Host: create server listening on port
    bool StartHost(int port, int maxClients);

    // Client: connect to host
    bool StartClient(const char* host, int port, int timeoutMs = 5000);

    void Stop();

    bool IsConnected() const { return Connected.load(); }
    bool IsHost() const { return HostMode; }
    int GetNumPeers() const { return NumPeers; }

    // Send to a specific peer (0-indexed among connected peers)
    void SendTo(int peerIdx, const void* data, u32 len, int channel, bool reliable);

    // Send to all peers
    void Broadcast(const void* data, u32 len, int channel, bool reliable);

    // Process incoming packets, calls callback for each
    // Returns number of packets processed
    using PacketCallback = std::function<void(int peerIdx, const u8* data, u32 len, int channel)>;
    int Poll(const PacketCallback& callback, int timeoutMs = 0);

    // Get round-trip time to a peer in ms
    u32 GetPeerRTT(int peerIdx) const;

    // Callback for connection/disconnection events
    using EventCallback = std::function<void(int peerIdx, bool connected)>;
    void SetEventCallback(const EventCallback& cb) { OnEvent = cb; }

private:
    ENetHost* Host = nullptr;
    ENetPeer* Peers[kNetplayMaxPlayers] {};
    int NumPeers = 0;
    bool HostMode = false;
    std::atomic<bool> Connected{false};

    EventCallback OnEvent;

    std::mutex ENetMutex;
};

// ---- Blob sender/receiver ----

class BlobTransfer
{
public:
    // Sending side: split data into chunks and send
    static void Send(NetplayTransport& transport, int peerIdx,
                     NetplayBlobType type, const void* data, u32 len);

    // Broadcast blob to all peers
    static void Broadcast(NetplayTransport& transport,
                          NetplayBlobType type, const void* data, u32 len);

    // Receiving side: feed incoming blob messages
    // Returns true when a complete blob is received
    bool OnMessage(const u8* data, u32 len);

    // Get completed blob
    bool IsComplete() const { return Complete; }
    NetplayBlobType GetType() const { return Type; }
    const std::vector<u8>& GetData() const { return Buffer; }
    void Reset();

private:
    NetplayBlobType Type = Blob_SRAM;
    u32 TotalLen = 0;
    u32 ReceivedLen = 0;
    std::vector<u8> Buffer;
    bool Complete = false;
    bool Receiving = false;
};

}

#endif // NETPLAYPROTOCOL_H
