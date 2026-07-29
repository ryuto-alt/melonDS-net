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

#ifndef LOCALMP_H
#define LOCALMP_H

#include <atomic>

#include "types.h"
#include "Platform.h"
#include "MPInterface.h"
#include "Wifi.h"

namespace melonDS
{
struct MPStatusData
{
    u16 ConnectedBitmask; // bitmask of which instances are ready to send/receive packets
    u16 MPHostinst; // instance ID from which the last CMD frame was sent
    u16 MPReplyBitmask;   // bitmask of which clients replied in time
};

constexpr u32 kPacketQueueSize = 0x10000;
constexpr u32 kReplyQueueSize = 0x10000;
constexpr u32 kMaxFrameSize = 0x948;

class LocalMP : public MPInterface
{
public:
    LocalMP() noexcept;
    LocalMP(const LocalMP&) = delete;
    LocalMP& operator=(const LocalMP&) = delete;
    LocalMP(LocalMP&& other) = delete;
    LocalMP& operator=(LocalMP&& other) = delete;
    ~LocalMP() noexcept;

    void Process() {}

    void Begin(int inst);
    void End(int inst);

    // Throw away every queued packet and every read position. Netplay calls
    // this at each sync barrier: a machine that has been running since the last
    // session has traffic queued that a machine joining fresh does not, and a
    // packet one console can still read while another cannot is a desync.
    void ResetQueues();

    int SendPacket(int inst, u8* data, int len, u64 timestamp);
    int RecvPacket(int inst, u8* data, u64* timestamp);
    int SendCmd(int inst, u8* data, int len, u64 timestamp);
    int SendReply(int inst, u8* data, int len, u64 timestamp, u16 aid);
    int SendAck(int inst, u8* data, int len, u64 timestamp);
    int RecvHostPacket(int inst, u8* data, u64* timestamp);
    u16 RecvReplies(int inst, u8* data, u64 timestamp, u16 aidmask);

    // Counts packets actually pushed between the local consoles. If this stops
    // moving while frames keep advancing, the game stalled, not the emulator.
    melonDS::u32 GetTrafficCount() const { return TrafficCount; }

    // Rendezvous cost, for the periodic frame trace. Waits that resolved off
    // the cached horizon cost nothing; the spin count is what burns a core.
    struct WaitStats { melonDS::u64 Calls, Cached, Spins, SpinMS; };
    WaitStats TakeWaitStats()
    {
        WaitStats s {};
        for (auto& c : Counters)
        {
            s.Calls  += c.Calls;  c.Calls  = 0;
            s.Cached += c.Cached; c.Cached = 0;
            s.Spins  += c.Spins;  c.Spins  = 0;
            s.SpinMS += c.SpinMS; c.SpinMS = 0;
        }
        return s;
    }

    // ---- Netplay lockstep ----
    // See WifiLockstep in Wifi.h. On: every receive is decided by the other
    // consoles' emulated clocks, never by a wall-clock timeout, so all machines
    // running the same mirror consoles see the same wireless traffic.
    void SetLockstep(bool enable) { Lockstep.store(enable); }
    WifiLockstep& LockstepSlot(int inst) { return Slots[inst]; }

    // Session frame of a mirror console, published by NetplaySession. This is
    // the arbitration clock for traffic sent before the consoles have synced
    // their wifi clocks (beacons, auth/assoc).
    void SetInstanceFrame(int inst, u32 frame)
    {
        Slots[inst].Frame.store(frame, std::memory_order_release);
    }

private:
    int SendPacketGeneric(int inst, u32 type, u8* packet, int len, u64 timestamp) noexcept;
    int RecvPacketGeneric(int inst, u8* packet, bool block, u64* timestamp,
                          u64 maxsenderclock) noexcept;

    // Block until every other console that could still send us something has
    // emulated strictly past `clock`. Returns false if the wait was abandoned --
    // session torn down, or the safety cap tripped, which means we desynced and
    // say so loudly rather than hanging the emulator.
    bool WaitForPeers(int inst, u64 clock) noexcept;

    std::atomic<bool> Lockstep {false};
    WifiLockstep Slots[16] {};

    // Grace period on the non-MP queue: a console only accepts traffic that is
    // already an emulated millisecond old, which turns "is there a packet?"
    // into a question about the past that every machine answers identically.
    // Beacons and the auth/assoc handshake are all that travels here and none
    // of it is timing-critical -- the MP queues get the exact treatment.
    static constexpr u64 kLockstepLag = 33514;

    // A console that stops running entirely must not hang the emulator. Giving
    // up here IS a desync, so it is logged as one.
    static constexpr u64 kLockstepWaitCapMS = 2000;

    melonDS::u32 TrafficCount = 0;

    Platform::Mutex* MPQueueLock;
    MPStatusData MPStatus {};
    // One queue per sender, not one shared ring. Two consoles transmitting at
    // the same emulated moment land in a shared ring in whatever order the two
    // threads reached the mutex -- and with three or more consoles that order
    // decides which packet is at the head, which decides whether a reader that
    // is only allowed to see packets up to time T finds anything at all. That
    // is a wall-clock input to an emulated decision: with two consoles it never
    // showed (the only other sender was the peer itself), with three it desyncs
    // the session outright. Separate queues plus a deterministic pick order
    // (see PickNext) take real time out of it entirely.
    u8 MPPacketQueue[16][kPacketQueueSize] {};
    u8 MPReplyQueue[16][kReplyQueueSize] {};
    u32 PacketWriteOffset[16] {};
    u32 ReplyWriteOffset[16] {};
    u32 PacketReadOffset[16][16] {};   // [reader][sender]
    u32 ReplyReadOffset[16][16] {};

    // Arrival order, for the non-netplay path where the emulated clocks are not
    // published and plain FIFO is the right answer.
    u64 SendSeq = 0;

    // How far every peer had provably got, as last observed by this console.
    // Clocks only move forward, so once we have seen a minimum H, every later
    // question about a moment before H is already answered -- no shared reads,
    // no spinning. Consoles routinely run a frame or more apart, so this is the
    // common case: it turns thousands of rendezvous a frame into a compare.
    // Only valid while the connected set is unchanged (a console that just
    // powered its wifi on starts behind everyone).
    // Written and read only by the owning console's own thread -- one cache
    // line each, or four consoles updating them thousands of times a frame
    // spend their time invalidating each other's lines.
    struct alignas(64) HorizonSlot
    {
        u64 Clock = 0;
        u16 Mask = 0;
        bool Valid = false;
    };
    HorizonSlot Horizon[16] {};

    // Rendezvous counters, same reasoning: per console, never shared, summed
    // when the frame trace asks for them.
    struct alignas(64) WaitCounters
    {
        u64 Calls = 0, Cached = 0, Spins = 0, SpinMS = 0;
    };
    WaitCounters Counters[16] {};

    void FIFOWrite(int sender, int fifo, const void* buf, int len) noexcept;
    void FIFORead(int reader, int sender, int fifo, void* buf, int len) noexcept;
    bool PeekHeader(int reader, int sender, int fifo, MPPacketHeader& hdr) noexcept;
    void DropQueue(int reader, int sender, int fifo) noexcept;
    int PickNext(int reader, int fifo, u64 maxsenderclock, MPPacketHeader& out) noexcept;

    int LastHostID = -1;
    Platform::Semaphore* SemPool[32] {};
};
}

#endif // LOCALMP_H
