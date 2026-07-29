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
#include <thread>

#include "LocalMP.h"

// Spin hint for the lockstep rendezvous: a few nanoseconds, no syscall, no
// core handover. Everything else there costs at least a scheduler round trip.
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    #include <emmintrin.h>
    #define MP_CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(__arm__)
    #define MP_CPU_PAUSE() __asm__ __volatile__("yield")
#else
    #define MP_CPU_PAUSE() ((void)0)
#endif

using namespace melonDS;
using namespace melonDS::Platform;

using Platform::Log;
using Platform::LogLevel;

namespace melonDS
{

LocalMP::LocalMP() noexcept :
    MPQueueLock(Mutex_Create())
{
    memset(MPPacketQueue, 0, sizeof(MPPacketQueue));
    memset(MPReplyQueue, 0, sizeof(MPReplyQueue));
    memset(&MPStatus, 0, sizeof(MPStatus));
    memset(PacketWriteOffset, 0, sizeof(PacketWriteOffset));
    memset(ReplyWriteOffset, 0, sizeof(ReplyWriteOffset));
    memset(PacketReadOffset, 0, sizeof(PacketReadOffset));
    memset(ReplyReadOffset, 0, sizeof(ReplyReadOffset));

    // prepare semaphores
    // semaphores 0-15: regular frames; semaphore I is posted when instance I needs to process a new frame
    // semaphores 16-31: MP replies; semaphore I is posted when instance I needs to process a new MP reply

    for (int i = 0; i < 32; i++)
    {
        SemPool[i] = Semaphore_Create();
    }

    Log(LogLevel::Info, "MP comm init OK\n");
}

LocalMP::~LocalMP() noexcept
{
    for (int i = 0; i < 32; i++)
    {
        Semaphore_Free(SemPool[i]);
        SemPool[i] = nullptr;
    }

    Mutex_Free(MPQueueLock);
}

void LocalMP::Begin(int inst)
{
    Mutex_Lock(MPQueueLock);
    // Start listening from where everyone is now -- whatever was queued while
    // this console had its wifi off is not addressed to the session it is
    // joining, and reading it would only hand it garbage.
    for (int s = 0; s < 16; s++)
    {
        PacketReadOffset[inst][s] = PacketWriteOffset[s];
        ReplyReadOffset[inst][s] = ReplyWriteOffset[s];
    }
    Semaphore_Reset(SemPool[inst]);
    Semaphore_Reset(SemPool[16 + inst]);
    MPStatus.ConnectedBitmask |= (1 << inst);
    Mutex_Unlock(MPQueueLock);
}

void LocalMP::End(int inst)
{
    Mutex_Lock(MPQueueLock);
    MPStatus.ConnectedBitmask &= ~(1 << inst);
    Mutex_Unlock(MPQueueLock);
}

void LocalMP::ResetQueues()
{
    Mutex_Lock(MPQueueLock);

    memset(MPPacketQueue, 0, sizeof(MPPacketQueue));
    memset(MPReplyQueue, 0, sizeof(MPReplyQueue));
    memset(PacketWriteOffset, 0, sizeof(PacketWriteOffset));
    memset(ReplyWriteOffset, 0, sizeof(ReplyWriteOffset));
    memset(PacketReadOffset, 0, sizeof(PacketReadOffset));
    memset(ReplyReadOffset, 0, sizeof(ReplyReadOffset));
    memset(HorizonValid, 0, sizeof(HorizonValid));

    MPStatus.MPReplyBitmask = 0;
    SendSeq = 0;
    LastHostID = -1;

    for (int i = 0; i < 32; i++)
        Semaphore_Reset(SemPool[i]);

    Mutex_Unlock(MPQueueLock);
}

void LocalMP::FIFORead(int reader, int sender, int fifo, void* buf, int len) noexcept
{
    u8* data = (fifo == 0) ? MPPacketQueue[sender] : MPReplyQueue[sender];
    u32 datalen = (fifo == 0) ? kPacketQueueSize : kReplyQueueSize;
    u32 offset = (fifo == 0) ? PacketReadOffset[reader][sender]
                             : ReplyReadOffset[reader][sender];

    if ((offset + len) >= datalen)
    {
        u32 part1 = datalen - offset;
        memcpy(buf, &data[offset], part1);
        memcpy(&((u8*)buf)[part1], data, len - part1);
        offset = len - part1;
    }
    else
    {
        memcpy(buf, &data[offset], len);
        offset += len;
    }

    if (fifo == 0) PacketReadOffset[reader][sender] = offset;
    else           ReplyReadOffset[reader][sender] = offset;
}

void LocalMP::FIFOWrite(int sender, int fifo, const void* buf, int len) noexcept
{
    u8* data = (fifo == 0) ? MPPacketQueue[sender] : MPReplyQueue[sender];
    u32 datalen = (fifo == 0) ? kPacketQueueSize : kReplyQueueSize;
    u32 offset = (fifo == 0) ? PacketWriteOffset[sender] : ReplyWriteOffset[sender];

    if ((offset + len) >= datalen)
    {
        u32 part1 = datalen - offset;
        memcpy(&data[offset], buf, part1);
        memcpy(data, &((const u8*)buf)[part1], len - part1);
        offset = len - part1;
    }
    else
    {
        memcpy(&data[offset], buf, len);
        offset += len;
    }

    if (fifo == 0) PacketWriteOffset[sender] = offset;
    else           ReplyWriteOffset[sender] = offset;
}

// Header of the next packet `reader` would take from `sender`, without
// consuming it. False when that queue is empty for this reader.
bool LocalMP::PeekHeader(int reader, int sender, int fifo, MPPacketHeader& hdr) noexcept
{
    u32 rd = (fifo == 0) ? PacketReadOffset[reader][sender] : ReplyReadOffset[reader][sender];
    u32 wr = (fifo == 0) ? PacketWriteOffset[sender] : ReplyWriteOffset[sender];

    if (rd == wr) return false;

    u32 saved = rd;
    FIFORead(reader, sender, fifo, &hdr, sizeof(hdr));

    // Put the offset back: peeking must not consume.
    if (fifo == 0) PacketReadOffset[reader][sender] = saved;
    else           ReplyReadOffset[reader][sender] = saved;

    if (hdr.Magic != 0x4946494E)
    {
        // The writer lapped us. Nothing in there can be trusted any more.
        Log(LogLevel::Warn, "MP: queue overflow (reader %d, sender %d, fifo %d)\n",
            reader, sender, fifo);
        DropQueue(reader, sender, fifo);
        return false;
    }

    return true;
}

void LocalMP::DropQueue(int reader, int sender, int fifo) noexcept
{
    if (fifo == 0) PacketReadOffset[reader][sender] = PacketWriteOffset[sender];
    else           ReplyReadOffset[reader][sender] = ReplyWriteOffset[sender];
}

// Which sender's packet `reader` takes next, or -1 for "nothing due".
//
// Under lockstep the order is (SenderClock, sender index): a total order over
// emulated state, so every machine delivers the same packets in the same order
// no matter how the threads were scheduled. Outside it, arrival order -- what
// the single shared ring used to give.
int LocalMP::PickNext(int reader, int fifo, u64 maxsenderclock, MPPacketHeader& out) noexcept
{
    const bool lockstep = Lockstep.load(std::memory_order_relaxed);

    int best = -1;
    MPPacketHeader besthdr {};

    for (int s = 0; s < 16; s++)
    {
        if (s == reader) continue;   // a console never reads its own queue

        MPPacketHeader hdr;
        if (!PeekHeader(reader, s, fifo, hdr)) continue;

        if (lockstep)
        {
            // Half-open window: anything stamped at or after the moment being
            // asked about belongs to the next question.
            if (hdr.SenderClock >= maxsenderclock) continue;

            if (best < 0 || hdr.SenderClock < besthdr.SenderClock ||
                (hdr.SenderClock == besthdr.SenderClock && s < best))
            {
                best = s; besthdr = hdr;
            }
        }
        else if (best < 0 || hdr.Seq < besthdr.Seq)
        {
            best = s; besthdr = hdr;
        }
    }

    if (best >= 0) out = besthdr;
    return best;
}

// ---- Netplay lockstep ----
//
// Every machine in a mirror-netplay session runs the same set of consoles and
// exchanges nothing but inputs, so the wireless traffic between those consoles
// has to come out identical on all of them. Wall-clock timeouts cannot deliver
// that: whether a reply "made it in time" would depend on the OS scheduler, and
// two machines start answering differently within seconds -- which is exactly
// how both players end up in a different game.
//
// So each console publishes an arbitration clock (session frame plus cycles
// into that frame -- comparable between consoles even when one was just reset
// and the other resumed from a savestate), and:
//
//   * every packet is stamped with its sender's clock,
//   * a console only accepts packets stamped strictly BEFORE the emulated
//     moment it is asking about,
//   * before it may answer "nothing arrived", it waits for every other console
//     to reach that moment.
//
// Both halves are pure functions of emulated time, so every machine reaches the
// same answer no matter how the threads were scheduled.
//
// The window is half-open -- [.., T), reached rather than passed -- and it has
// to be. The MP reply window closes at the exact moment each client derives as
// its own NextSync from the host's CMD frame, so host and clients ask about the
// SAME T. Demanding that a peer be strictly past T made every one of those a
// mutual deadlock: both consoles parked at T, each waiting for the other to
// leave it, both giving up 2 seconds later. (Seen in the wild: a Download Play
// transfer spent minutes at a standstill, both instances timing out on the
// identical clock value.) Reaching T is enough, because a console at T has
// provably sent everything it will ever stamp before T.
bool LocalMP::WaitForPeers(int inst, u64 clock) noexcept
{
    // Never park. The consoles hand off to each other a few dozen times per
    // frame (the beacon path alone re-syncs them every 512 emulated us) and
    // each handoff is settled within emulated microseconds -- tens of wall
    // microseconds of real work. Sleeping costs an OS timer tick instead:
    // QThread::usleep rounds up to 1 ms on Windows, and 30-40 of those a frame
    // is exactly the 33 ms/frame that made a Download Play transfer crawl at
    // half speed. So spin: a hot core here is cheaper than a scheduler round
    // trip, and both consoles are running flat out anyway.
    //
    // ponytail: pause-spin, then yield. Upgrade path if this ever shows up as
    // burn on a small machine: a condition variable per slot, signalled from
    // Advance() -- which costs a broadcast on every wifi tick, so measure first.
    StatWaitCalls.fetch_add(1, std::memory_order_relaxed);

    // Already known to be settled: every peer had reached at least this far the
    // last time we looked, and clocks never go backwards.
    if (HorizonValid[inst] && HorizonMask[inst] == MPStatus.ConnectedBitmask &&
        clock <= PeerHorizon[inst])
    {
        StatWaitCached.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    int spins = 0;
    u64 t0 = 0;

    for (;;)
    {
        if (!Lockstep.load(std::memory_order_relaxed))
            return false;

        u16 mask = MPStatus.ConnectedBitmask;
        bool behind = false;
        u64 horizon = ~(u64)0;

        for (int i = 0; i < 16; i++)
        {
            // Only consoles with their wifi powered can still send us anything;
            // MP_Begin/MP_End keep that bitmask honest.
            if (i == inst || !(mask & (1<<i)))
                continue;

            u64 peer = Slots[i].Clock.load(std::memory_order_acquire);
            if (peer < clock)
            {
                behind = true;
                break;
            }
            if (peer < horizon) horizon = peer;
        }

        if (!behind)
        {
            // Remember how far ahead everyone actually was, not just that they
            // cleared this one moment.
            PeerHorizon[inst] = horizon;
            HorizonMask[inst] = mask;
            HorizonValid[inst] = true;

            if (spins)
            {
                StatWaitSpins.fetch_add((u64)spins, std::memory_order_relaxed);
                if (t0) StatWaitSpinMS.fetch_add(Platform::GetMSCount() - t0, std::memory_order_relaxed);
            }
            return true;
        }

        spins++;

        // Sub-microsecond waits: the peer is mid-instruction, a hint to the
        // core beats any trip through the scheduler.
        if (spins < 4096)
        {
            MP_CPU_PAUSE();
            continue;
        }

        // Longer than that and the peer may be waiting for a core of its own
        // (small machines, or more consoles than cores).
        std::this_thread::yield();

        // Bounded even when the wait is hopeless -- but check the clock rarely,
        // it is a timer syscall.
        if ((spins & 0x3FF) == 0)
        {
            if (t0 == 0)
                t0 = Platform::GetMSCount();
            else if ((Platform::GetMSCount() - t0) > kLockstepWaitCapMS)
            {
                Log(LogLevel::Error,
                    "MP lockstep: instance %d waited out %llu ms at clock %llu -- session has desynced\n",
                    inst, (unsigned long long)kLockstepWaitCapMS, (unsigned long long)clock);
                return false;
            }
        }
    }
}

int LocalMP::SendPacketGeneric(int inst, u32 type, u8* packet, int len, u64 timestamp) noexcept
{
    if (len > kMaxFrameSize)
    {
        Log(LogLevel::Warn, "wifi: attempting to send frame too big (len=%d max=%d)\n", len, kMaxFrameSize);
        return 0;
    }

    Mutex_Lock(MPQueueLock);

    TrafficCount++;

    u16 mask = MPStatus.ConnectedBitmask;

    // TODO: check if the FIFO is full!

    MPPacketHeader pktheader;
    pktheader.Magic = 0x4946494E;
    pktheader.SenderID = inst;
    pktheader.Type = type;
    pktheader.Length = len;
    pktheader.Timestamp = timestamp;
    // Stamped with the clock of the tick we are inside, which is published
    // before the tick runs -- so a peer that has seen a strictly larger value
    // from us is guaranteed to find this packet already queued.
    pktheader.SenderClock = Slots[inst].Clock.load(std::memory_order_relaxed);
    pktheader.Seq = SendSeq++;

    type &= 0xFFFF;
    int nfifo = (type == 2) ? 1 : 0;
    FIFOWrite(inst, nfifo, &pktheader, sizeof(pktheader));
    if (len)
        FIFOWrite(inst, nfifo, packet, len);

    if (type == 1)
    {
        // NOTE: this is not guarded against, say, multiple multiplay games happening on the same machine
        // we would need to pass the packet's SenderID through the wifi module for that
        MPStatus.MPHostinst = inst;
        MPStatus.MPReplyBitmask = 0;
        for (int s = 0; s < 16; s++)
            ReplyReadOffset[inst][s] = ReplyWriteOffset[s];
        Semaphore_Reset(SemPool[16 + inst]);
    }
    else if (type == 2)
    {
        MPStatus.MPReplyBitmask |= (1 << inst);
    }

    Mutex_Unlock(MPQueueLock);

    if (type == 2)
    {
        Semaphore_Post(SemPool[16 +  MPStatus.MPHostinst]);
    }
    else
    {
        // Not to ourselves: a console never reads its own queue, so a count
        // posted here would be one the reader can never match with a packet.
        for (int i = 0; i < 16; i++)
        {
            if (i != inst && (mask & (1<<i)))
                Semaphore_Post(SemPool[i]);
        }
    }

    return len;
}

int LocalMP::RecvPacketGeneric(int inst, u8* packet, bool block, u64* timestamp,
                               u64 maxsenderclock) noexcept
{
    const bool lockstep = Lockstep.load(std::memory_order_relaxed);

    // Under lockstep the queues themselves are the answer: WaitForPeers has
    // already established that every console has emulated past the moment being
    // asked about, so whatever is queued for it is all there will ever be. The
    // semaphore is a wake-up hint for the blocking (non-netplay) path only --
    // consuming its count here would make "is there a packet?" depend on how
    // many times we happened to ask.
    if (!lockstep && !Semaphore_TryWait(SemPool[inst], block ? RecvTimeout : 0))
        return 0;

    Mutex_Lock(MPQueueLock);

    MPPacketHeader pktheader {};
    int sender = PickNext(inst, 0, maxsenderclock, pktheader);
    if (sender < 0)
    {
        Mutex_Unlock(MPQueueLock);
        return 0;
    }

    // Consume: header first, then the body.
    MPPacketHeader dummy;
    FIFORead(inst, sender, 0, &dummy, sizeof(dummy));

    if (pktheader.Length)
    {
        FIFORead(inst, sender, 0, packet, pktheader.Length);

        if (pktheader.Type == 1)
            LastHostID = pktheader.SenderID;
    }

    if (timestamp) *timestamp = pktheader.Timestamp;
    Mutex_Unlock(MPQueueLock);
    return pktheader.Length;
}

int LocalMP::SendPacket(int inst, u8* packet, int len, u64 timestamp)
{
    return SendPacketGeneric(inst, 0, packet, len, timestamp);
}

int LocalMP::RecvPacket(int inst, u8* packet, u64* timestamp)
{
    if (Lockstep.load(std::memory_order_relaxed))
    {
        // Beacons and the auth/assoc handshake: ask about a moment safely in
        // the past, so the answer is settled everywhere. Nothing here is
        // timing-critical -- the firmware even stretches its post-beacon window
        // to absorb exactly this kind of lag.
        u64 now = Slots[inst].Clock.load(std::memory_order_acquire);
        u64 upto = (now > kLockstepLag) ? (now - kLockstepLag) : 0;

        if (!WaitForPeers(inst, upto))
            return 0;

        return RecvPacketGeneric(inst, packet, false, timestamp, upto);
    }

    return RecvPacketGeneric(inst, packet, false, timestamp, ~(u64)0);
}

int LocalMP::SendCmd(int inst, u8* packet, int len, u64 timestamp)
{
    return SendPacketGeneric(inst, 1, packet, len, timestamp);
}

int LocalMP::SendReply(int inst, u8* packet, int len, u64 timestamp, u16 aid)
{
    return SendPacketGeneric(inst, 2 | (aid<<16), packet, len, timestamp);
}

int LocalMP::SendAck(int inst, u8* packet, int len, u64 timestamp)
{
    return SendPacketGeneric(inst, 3, packet, len, timestamp);
}

int LocalMP::RecvHostPacket(int inst, u8* packet, u64* timestamp)
{
    if (LastHostID != -1)
    {
        // check if the host is still connected

        u16 curinstmask = MPStatus.ConnectedBitmask;

        if (!(curinstmask & (1 << LastHostID)))
            return -1;
    }

    if (Lockstep.load(std::memory_order_relaxed))
    {
        // The MP client sits here at every NextSync. Wait for the other
        // consoles to emulate past this exact moment and then take whatever
        // they had queued for it: "nothing" then means nothing, rather than
        // "nothing yet, ask again in 25 milliseconds".
        u64 now = Slots[inst].Clock.load(std::memory_order_acquire);

        if (!WaitForPeers(inst, now))
            return 0;

        return RecvPacketGeneric(inst, packet, false, timestamp, now);
    }

    return RecvPacketGeneric(inst, packet, true, timestamp, ~(u64)0);
}

u16 LocalMP::RecvReplies(int inst, u8* packets, u64 timestamp, u16 aidmask)
{
    u16 ret = 0;
    u16 myinstmask = (1 << inst);
    u16 curinstmask;

    curinstmask = MPStatus.ConnectedBitmask;

    // if all clients have left: return early
    if ((myinstmask & curinstmask) == curinstmask)
        return 0;

    // Lockstep: park until every console has emulated past the close of the MP
    // reply window. Whoever has not replied by then genuinely missed it -- and
    // missed it on every machine, which is the whole point. The deadline is the
    // same moment each client derives as its own NextSync from our CMD frame,
    // so they are all allowed to reach it and nobody deadlocks.
    const bool lockstep = Lockstep.load(std::memory_order_relaxed);
    u64 deadline = ~(u64)0;

    if (lockstep)
    {
        deadline = Slots[inst].ReplyDeadline.load(std::memory_order_acquire);

        // Announce the end of the window before parking on it. We transmit
        // nothing until then (the ack goes out as the window closes), so this
        // is honest -- and without it a client that had fallen behind would sit
        // waiting for this console's clock to move while this console sat
        // waiting for that same client to reach the deadline.
        Slots[inst].Advance(deadline);

        if (!WaitForPeers(inst, deadline))
            return 0;
    }

    for (;;)
    {
        if (!lockstep && !Semaphore_TryWait(SemPool[16+inst], RecvTimeout))
        {
            // no more replies available
            return ret;
        }

        Mutex_Lock(MPQueueLock);

        MPPacketHeader pktheader {};
        int sender = PickNext(inst, 1, deadline, pktheader);
        if (sender < 0)
        {
            // Nothing left inside the window: whoever has not replied by now
            // genuinely missed it -- and missed it on every machine.
            Mutex_Unlock(MPQueueLock);
            return ret;
        }

        MPPacketHeader dummy;
        FIFORead(inst, sender, 1, &dummy, sizeof(dummy));

        if (pktheader.Timestamp < (timestamp - 32)) // stale packet
        {
            // Length is bounded at send time, so this always consumes exactly
            // the body and leaves the queue aligned.
            if (pktheader.Length && pktheader.Length <= kMaxFrameSize)
            {
                u8 discard[kMaxFrameSize];
                FIFORead(inst, sender, 1, discard, (int)pktheader.Length);
            }
            Mutex_Unlock(MPQueueLock);
            continue;
        }

        if (pktheader.Length)
        {
            u32 aid = (pktheader.Type >> 16);
            FIFORead(inst, sender, 1, &packets[(aid-1)*1024], pktheader.Length);
            ret |= (1 << aid);
        }

        myinstmask |= (1 << pktheader.SenderID);
        if (((myinstmask & curinstmask) == curinstmask) ||
            ((ret & aidmask) == aidmask))
        {
            // all the clients have sent their reply

            Mutex_Unlock(MPQueueLock);
            return ret;
        }

        Mutex_Unlock(MPQueueLock);
    }
}

}

