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

#include <cstring>
#include <algorithm>

#include "NetplayProtocol.h"
#include "Platform.h"

namespace melonDS
{

using Platform::Log;
using Platform::LogLevel;

// ---- Compression stubs ----
// For now we send data uncompressed. Compression can be added later
// using zlib or another library without changing the protocol.

bool CompressData(const void* src, u32 srcLen, std::vector<u8>& dst)
{
    dst.resize(srcLen);
    memcpy(dst.data(), src, srcLen);
    return true;
}

bool DecompressData(const u8* src, u32 srcLen, void* dst, u32 dstLen)
{
    if (srcLen != dstLen) return false;
    memcpy(dst, src, srcLen);
    return true;
}

// ---- NetplayTransport ----

NetplayTransport::NetplayTransport()
{
}

NetplayTransport::~NetplayTransport()
{
    Stop();
}

bool NetplayTransport::StartHost(int port, int maxClients)
{
    std::lock_guard<std::mutex> lock(ENetMutex);

    if (enet_initialize() != 0)
    {
        Log(LogLevel::Error, "Netplay: failed to initialize ENet\n");
        return false;
    }

    ENetAddress addr;
    addr.host = ENET_HOST_ANY;
    addr.port = port;

    Host = enet_host_create(&addr, maxClients, Chan_MAX, 0, 0);
    if (!Host)
    {
        Log(LogLevel::Error, "Netplay: failed to create ENet host on port %d\n", port);
        enet_deinitialize();
        return false;
    }

    HostMode = true;
    NumPeers = 0;
    Connected.store(true);

    Log(LogLevel::Info, "Netplay: host started on port %d\n", port);
    return true;
}

bool NetplayTransport::StartClient(const char* host, int port, int timeoutMs)
{
    std::lock_guard<std::mutex> lock(ENetMutex);

    if (enet_initialize() != 0)
    {
        Log(LogLevel::Error, "Netplay: failed to initialize ENet\n");
        return false;
    }

    Host = enet_host_create(nullptr, 1, Chan_MAX, 0, 0);
    if (!Host)
    {
        Log(LogLevel::Error, "Netplay: failed to create ENet client\n");
        enet_deinitialize();
        return false;
    }

    ENetAddress addr;
    enet_address_set_host(&addr, host);
    addr.port = port;

    ENetPeer* peer = enet_host_connect(Host, &addr, Chan_MAX, 0);
    if (!peer)
    {
        Log(LogLevel::Error, "Netplay: failed to initiate connection to %s:%d\n", host, port);
        enet_host_destroy(Host);
        Host = nullptr;
        enet_deinitialize();
        return false;
    }

    // Wait for connection
    ENetEvent event;
    if (enet_host_service(Host, &event, timeoutMs) > 0 &&
        event.type == ENET_EVENT_TYPE_CONNECT)
    {
        Peers[0] = peer;
        NumPeers = 1;
        HostMode = false;
        Connected.store(true);

        Log(LogLevel::Info, "Netplay: connected to %s:%d\n", host, port);
        return true;
    }

    Log(LogLevel::Error, "Netplay: connection to %s:%d timed out\n", host, port);
    enet_peer_reset(peer);
    enet_host_destroy(Host);
    Host = nullptr;
    enet_deinitialize();
    return false;
}

void NetplayTransport::Stop()
{
    std::lock_guard<std::mutex> lock(ENetMutex);

    if (!Host) return;

    // Disconnect all peers gracefully
    for (int i = 0; i < NumPeers; i++)
    {
        if (Peers[i])
        {
            enet_peer_disconnect_now(Peers[i], 0);
            Peers[i] = nullptr;
        }
    }

    enet_host_flush(Host);
    enet_host_destroy(Host);
    Host = nullptr;
    NumPeers = 0;
    Connected.store(false);

    enet_deinitialize();

    Log(LogLevel::Info, "Netplay: transport stopped\n");
}

void NetplayTransport::SendTo(int peerIdx, const void* data, u32 len, int channel, bool reliable)
{
    std::lock_guard<std::mutex> lock(ENetMutex);

    if (!Host || peerIdx < 0 || peerIdx >= NumPeers || !Peers[peerIdx])
        return;

    u32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
    ENetPacket* pkt = enet_packet_create(data, len, flags);
    enet_peer_send(Peers[peerIdx], channel, pkt);
}

void NetplayTransport::Broadcast(const void* data, u32 len, int channel, bool reliable)
{
    std::lock_guard<std::mutex> lock(ENetMutex);

    if (!Host) return;

    u32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
    ENetPacket* pkt = enet_packet_create(data, len, flags);
    enet_host_broadcast(Host, channel, pkt);
}

int NetplayTransport::Poll(const PacketCallback& callback, int timeoutMs)
{
    std::lock_guard<std::mutex> lock(ENetMutex);

    if (!Host) return 0;

    int count = 0;
    ENetEvent event;

    while (enet_host_service(Host, &event, (count == 0) ? timeoutMs : 0) > 0)
    {
        switch (event.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
            if (HostMode)
            {
                // New client connected
                if (NumPeers < kNetplayMaxPlayers)
                {
                    Peers[NumPeers] = event.peer;
                    event.peer->data = (void*)(intptr_t)NumPeers;
                    NumPeers++;

                    Log(LogLevel::Info, "Netplay: peer connected (total: %d)\n", NumPeers);

                    if (OnEvent)
                        OnEvent(NumPeers - 1, true);
                }
                else
                {
                    enet_peer_disconnect_now(event.peer, 0);
                }
            }
            break;

        case ENET_EVENT_TYPE_DISCONNECT:
        {
            int peerIdx = (int)(intptr_t)event.peer->data;

            Log(LogLevel::Info, "Netplay: peer %d disconnected\n", peerIdx);

            // Find and remove peer
            for (int i = 0; i < NumPeers; i++)
            {
                if (Peers[i] == event.peer)
                {
                    Peers[i] = nullptr;
                    if (OnEvent)
                        OnEvent(i, false);
                    break;
                }
            }
            break;
        }

        case ENET_EVENT_TYPE_RECEIVE:
        {
            int peerIdx;
            if (HostMode)
                peerIdx = (int)(intptr_t)event.peer->data;
            else
                peerIdx = 0;

            if (callback)
                callback(peerIdx, event.packet->data, (u32)event.packet->dataLength, event.channelID);

            enet_packet_destroy(event.packet);
            count++;
            break;
        }

        default:
            break;
        }
    }

    return count;
}

u32 NetplayTransport::GetPeerRTT(int peerIdx) const
{
    if (peerIdx < 0 || peerIdx >= NumPeers || !Peers[peerIdx])
        return 0;

    return Peers[peerIdx]->roundTripTime;
}

// ---- BlobTransfer ----

void BlobTransfer::Send(NetplayTransport& transport, int peerIdx,
                        NetplayBlobType type, const void* data, u32 len)
{
    // Send BlobStart
    MsgBlobStart startMsg;
    startMsg.Type = Msg_BlobStart;
    startMsg.BlobType = type;
    startMsg.TotalLen = len;
    transport.SendTo(peerIdx, &startMsg, sizeof(startMsg), Chan_Control, true);

    // Send chunks
    const u8* ptr = (const u8*)data;
    u32 offset = 0;
    std::vector<u8> chunkBuf(sizeof(MsgBlobChunk) + kBlobChunkSize);

    while (offset < len)
    {
        u32 chunkLen = std::min(kBlobChunkSize, len - offset);

        MsgBlobChunk* chunkMsg = (MsgBlobChunk*)chunkBuf.data();
        chunkMsg->Type = Msg_BlobChunk;
        chunkMsg->Offset = offset;
        memcpy(chunkBuf.data() + sizeof(MsgBlobChunk), ptr + offset, chunkLen);

        transport.SendTo(peerIdx, chunkBuf.data(), sizeof(MsgBlobChunk) + chunkLen, Chan_Control, true);
        offset += chunkLen;
    }

    // Send BlobEnd
    MsgBlobEnd endMsg;
    endMsg.Type = Msg_BlobEnd;
    endMsg.BlobType = type;
    endMsg.Checksum = 0; // TODO: CRC32
    for (u32 i = 0; i < len; i++)
        endMsg.Checksum += ((const u8*)data)[i];
    transport.SendTo(peerIdx, &endMsg, sizeof(endMsg), Chan_Control, true);
}

void BlobTransfer::Broadcast(NetplayTransport& transport,
                             NetplayBlobType type, const void* data, u32 len)
{
    // Send BlobStart
    MsgBlobStart startMsg;
    startMsg.Type = Msg_BlobStart;
    startMsg.BlobType = type;
    startMsg.TotalLen = len;
    transport.Broadcast(&startMsg, sizeof(startMsg), Chan_Control, true);

    // Send chunks
    const u8* ptr = (const u8*)data;
    u32 offset = 0;
    std::vector<u8> chunkBuf(sizeof(MsgBlobChunk) + kBlobChunkSize);

    while (offset < len)
    {
        u32 chunkLen = std::min(kBlobChunkSize, len - offset);

        MsgBlobChunk* chunkMsg = (MsgBlobChunk*)chunkBuf.data();
        chunkMsg->Type = Msg_BlobChunk;
        chunkMsg->Offset = offset;
        memcpy(chunkBuf.data() + sizeof(MsgBlobChunk), ptr + offset, chunkLen);

        transport.Broadcast(chunkBuf.data(), sizeof(MsgBlobChunk) + chunkLen, Chan_Control, true);
        offset += chunkLen;
    }

    // Send BlobEnd
    MsgBlobEnd endMsg;
    endMsg.Type = Msg_BlobEnd;
    endMsg.BlobType = type;
    endMsg.Checksum = 0;
    for (u32 i = 0; i < len; i++)
        endMsg.Checksum += ((const u8*)data)[i];
    transport.Broadcast(&endMsg, sizeof(endMsg), Chan_Control, true);
}

bool BlobTransfer::OnMessage(const u8* data, u32 len)
{
    if (len < 1) return false;

    u8 type = data[0];

    switch (type)
    {
    case Msg_BlobStart:
    {
        if (len < sizeof(MsgBlobStart)) return false;
        const MsgBlobStart* msg = (const MsgBlobStart*)data;

        Type = (NetplayBlobType)msg->BlobType;
        TotalLen = msg->TotalLen;
        ReceivedLen = 0;
        Buffer.resize(TotalLen);
        memset(Buffer.data(), 0, TotalLen);
        Complete = false;
        Receiving = true;

        Log(LogLevel::Info, "Netplay: receiving blob type %d, size %u\n", Type, TotalLen);
        break;
    }

    case Msg_BlobChunk:
    {
        if (!Receiving || len < sizeof(MsgBlobChunk)) return false;
        const MsgBlobChunk* msg = (const MsgBlobChunk*)data;

        u32 chunkLen = len - sizeof(MsgBlobChunk);
        u32 offset = msg->Offset;

        if (offset + chunkLen > TotalLen)
        {
            Log(LogLevel::Error, "Netplay: blob chunk out of bounds\n");
            return false;
        }

        memcpy(Buffer.data() + offset, data + sizeof(MsgBlobChunk), chunkLen);
        ReceivedLen += chunkLen;
        break;
    }

    case Msg_BlobEnd:
    {
        if (!Receiving || len < sizeof(MsgBlobEnd)) return false;
        const MsgBlobEnd* msg = (const MsgBlobEnd*)data;

        // Verify checksum
        u32 checksum = 0;
        for (u32 i = 0; i < TotalLen; i++)
            checksum += Buffer[i];

        if (checksum != msg->Checksum)
        {
            Log(LogLevel::Error, "Netplay: blob checksum mismatch (expected %08X, got %08X)\n",
                msg->Checksum, checksum);
            Receiving = false;
            return false;
        }

        Complete = true;
        Receiving = false;
        Log(LogLevel::Info, "Netplay: blob type %d received successfully (%u bytes)\n", Type, TotalLen);
        return true;
    }

    default:
        return false;
    }

    return false;
}

void BlobTransfer::Reset()
{
    Type = Blob_SRAM;
    TotalLen = 0;
    ReceivedLen = 0;
    Buffer.clear();
    Complete = false;
    Receiving = false;
}

}
