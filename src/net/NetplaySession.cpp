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
#include <chrono>

#include "NetplaySession.h"
#include "Platform.h"
#include "NDSCart.h"
#include "xxhash/xxhash.h"

namespace melonDS
{

using Platform::Log;
using Platform::LogLevel;

NetplaySession* NetplaySession::Current = nullptr;

NetplaySession::NetplaySession()
{
    memset(Instances, 0, sizeof(Instances));
    memset(InstData, 0, sizeof(InstData));
    memset(InputBuf, 0, sizeof(InputBuf));
    memset(InputReady, 0, sizeof(InputReady));
}

NetplaySession::~NetplaySession()
{
    DeInit();
}

bool NetplaySession::Init(int localPlayerID, int numPlayers, int inputDelay)
{
    if (numPlayers < 2 || numPlayers > kNetplayMaxPlayers)
    {
        Log(LogLevel::Error, "Netplay: invalid player count %d\n", numPlayers);
        return false;
    }

    if (localPlayerID < 0 || localPlayerID >= numPlayers)
    {
        Log(LogLevel::Error, "Netplay: invalid local player ID %d\n", localPlayerID);
        return false;
    }

    LocalPlayerID = localPlayerID;
    NumInstances = numPlayers;
    InputDelay = inputDelay;
    CurrentFrame = 0;
    StartFrame = 0;
    HostMode = (localPlayerID == 0);

    // Clear input buffers
    memset(InputBuf, 0, sizeof(InputBuf));
    memset(InputReady, 0, sizeof(InputReady));

    // Pre-fill input buffer with neutral inputs for the initial delay frames
    for (int p = 0; p < numPlayers; p++)
    {
        for (int f = 0; f < inputDelay; f++)
        {
            InputFrame neutral = {};
            neutral.FrameNum = f;
            neutral.KeyMask = 0xFFF; // all buttons released
            neutral.Touching = 0;
            neutral.TouchX = 0;
            neutral.TouchY = 0;
            neutral.LidClosed = 0;
            neutral.Checksum = 0;

            InputBuf[p][f % INPUT_BUF_SIZE] = neutral;
            InputReady[p][f % INPUT_BUF_SIZE] = true;
        }
    }

    Current = this;
    Active.store(true);

    Log(LogLevel::Info, "Netplay: session initialized (player %d/%d, delay %d)\n",
        localPlayerID, numPlayers, inputDelay);
    return true;
}

void NetplaySession::DeInit()
{
    // Drop these first: the emu thread's wait loops key off Active/Stage, and
    // they must be able to bail out before we start joining threads and
    // deleting the instances they might still be looking at.
    Active.store(false);
    Stage.store(Stage_Idle);

    StopThreads();

    Transport.Stop();

    for (int i = 0; i < kNetplayMaxPlayers; i++)
    {
        if (Instances[i])
        {
            delete Instances[i];
            Instances[i] = nullptr;
        }
    }

    NumInstances = 0;

    if (Current == this)
        Current = nullptr;

    Log(LogLevel::Info, "Netplay: session deinitialized\n");
}

bool NetplaySession::CreateInstances(const NDSArgsBuilder& argsBuilder, void* origUserdata)
{
    ArgsBuilder = argsBuilder;
    OrigUserdata = origUserdata;

    for (int i = 0; i < NumInstances; i++)
    {
        NDSArgs args = argsBuilder(i, FirmwareData, BIOS9Data, BIOS7Data);

        // Each instance gets a NetplayInstanceData as userdata
        // so that Platform::MP_* callbacks can route to the correct LocalMP instance
        // OrigUserdata points to EmuInstance* so non-MP Platform callbacks still work
        // The ctor memsets InstData, so the in-class default for Magic is gone.
        // Platform::GetEmuInstance keys off it -- without it our struct gets
        // cast straight to EmuInstance* and the first callback crashes.
        InstData[i].Magic = NetplayInstanceData::kMagic;
        InstData[i].InstID = i;
        InstData[i].Session = this;
        InstData[i].OrigUserdata = origUserdata;

        Instances[i] = new NDS(std::move(args), &InstData[i]);

        if (!Instances[i])
        {
            Log(LogLevel::Error, "Netplay: failed to create NDS instance %d\n", i);
            return false;
        }

        // Hand this console its lockstep slot. From here on its wireless traffic
        // is arbitrated on emulated time instead of on 25ms wall-clock recv
        // timeouts -- without this the mirror consoles exchange a different set
        // of packets on every machine and the games drift apart within seconds.
        Instances[i]->Wifi.Lockstep = &LMP.LockstepSlot(i);

        Instances[i]->Reset();

        // The frontend clears the RTC's power-lost flag (StatusReg1 bit7) on
        // its own console via setDateTime(), but the mirror consoles never get
        // that: their clock sits at 2000-01-01 with the "battery died" flag
        // raised. A real-firmware boot (download play guest consoles) sees the
        // flag and drops into the first-boot "adjust system settings" wizard
        // instead of the DS menu. Clear it with a fixed date -- it must NOT be
        // the wall clock, because every peer runs this line at a different
        // moment and the RTC state has to stay byte-identical across machines.
        // (Late joiners overwrite this with the host's RTC via savestate.)
        Instances[i]->RTC.SetDateTime(2000, 1, 1, 0, 0, 0);

        // Every mirror console rasterizes a frame like any other, and a console
        // built this way gets the software renderer with threading OFF -- the
        // frontend only ever configured the one console it displays. So three
        // of the four consoles rasterized 192 scanlines of 3D and 2D inline, on
        // the thread that publishes their lockstep clock. That clock is frozen
        // for the whole of it, and every other console sits spinning until it
        // moves again: one console's rendering stalls the entire session.
        //
        // Rendering never touches emulated state (the renderers only write
        // framebuffers; VRAM display capture is handled inside them either
        // way), so this needs no agreement between machines.
        {
            RendererSettings rs = {};
            rs.ScaleFactor = 1;
            rs.Threaded = true;
            Instances[i]->GetRenderer().SetRenderSettings(rs);
        }
    }

    // Registering the instances here would be wrong: LocalMP's connected
    // bitmask is what tells the lockstep waits which consoles can still send
    // something, and a console that never powers its wifi on would stall every
    // other one forever. Wifi::UpdatePowerOn calls MP_Begin/MP_End itself.

    Log(LogLevel::Info, "Netplay: created %d NDS instances\n", NumInstances);
    return true;
}

bool NetplaySession::LoadROM(std::unique_ptr<NDSCart::CartCommon> cart)
{
    if (!cart || NumInstances == 0)
        return false;

    // Get ROM data for cloning to other instances
    const u8* romData = cart->GetROM();
    u32 romLen = cart->GetROMLength();

    if (!romData || romLen == 0)
        return false;

    // Get save data if any
    const u8* saveData = nullptr;
    u32 saveLen = 0;

    // Set the cart on instance 0
    Instances[0]->SetNDSCart(std::move(cart));
    Instances[0]->Reset();

    if (Instances[0]->NeedsDirectBoot())
        Instances[0]->SetupDirectBoot("");

    // For remaining instances, parse identical ROM copies
    for (int i = 1; i < NumInstances; i++)
    {
        // Create a cart copy from the same ROM data
        auto cartCopy = NDSCart::ParseROM(romData, romLen);
        if (!cartCopy)
        {
            Log(LogLevel::Error, "Netplay: failed to parse ROM copy for instance %d\n", i);
            return false;
        }

        Instances[i]->SetNDSCart(std::move(cartCopy));
        Instances[i]->Reset();

        if (Instances[i]->NeedsDirectBoot())
            Instances[i]->SetupDirectBoot("");
    }

    Log(LogLevel::Info, "Netplay: ROM loaded on all %d instances\n", NumInstances);
    return true;
}

void NetplaySession::SetSharedData(const u8* firmware, u32 fwlen)
{
    if (firmware && fwlen)
        FirmwareData.assign(firmware, firmware + fwlen);
    else
        FirmwareData.clear();
}

void NetplaySession::SetSharedBIOS(const u8* bios9, u32 len9, const u8* bios7, u32 len7)
{
    if (bios9 && len9)
        BIOS9Data.assign(bios9, bios9 + len9);
    else
        BIOS9Data.clear();

    if (bios7 && len7)
        BIOS7Data.assign(bios7, bios7 + len7);
    else
        BIOS7Data.clear();
}

bool NetplaySession::LoadROMData(const u8* romdata, u32 romlen)
{
    if (!romdata || romlen == 0 || NumInstances == 0)
        return false;

    // Keep our own copy: the client may have to rebuild its instances once the
    // host reports the real player count, and the caller's buffer is long gone.
    ROMData.assign(romdata, romdata + romlen);
    ROMHash = XXH64(romdata, romlen, 0);

    for (int i = 0; i < NumInstances; i++)
    {
        // Download play: only player 0 owns the cart. Everyone else boots the
        // firmware menu with an empty slot and receives the game over the
        // emulated wireless from instance 0 -- the real thing, not a copy.
        if (DownloadPlay && i != 0)
        {
            Instances[i]->SetNDSCart(nullptr);
            Instances[i]->Reset();
            continue;
        }

        auto cart = NDSCart::ParseROM(ROMData.data(), romlen, &InstData[i]);
        if (!cart)
        {
            Log(LogLevel::Error, "Netplay: failed to parse ROM for instance %d\n", i);
            return false;
        }

        Instances[i]->SetNDSCart(std::move(cart));
        Instances[i]->Reset();

        // Boot straight into the game. With real BIOS and firmware installed
        // NeedsDirectBoot() is false, so this console would otherwise sit in
        // the DS menu -- and in download play it is the one that has to be
        // running the game for the others to pull it off.
        Instances[i]->SetupDirectBoot("");
    }

    // Reset() leaves a console halted. Without Start() the CPUs never execute:
    // RunFrame falls straight out of `while (Running)` and returns a full
    // scanline count, so frames tick up at full speed while the picture never
    // changes and no wireless packet is ever sent. The frontend only ever
    // starts its own console, so the mirrors have to be started here.
    for (int i = 0; i < NumInstances; i++)
        Instances[i]->Start();

    Log(LogLevel::Info, "Netplay: ROM (%u bytes, hash %016llX) loaded and started, %d instances, download play %s\n",
        romlen, (unsigned long long)ROMHash, NumInstances, DownloadPlay ? "on" : "off");
    return true;
}

void NetplaySession::DestroyInstances()
{
    StopThreads();

    for (int i = 0; i < kNetplayMaxPlayers; i++)
    {
        if (Instances[i])
        {
            LMP.End(i);
            delete Instances[i];
            Instances[i] = nullptr;
        }
    }
    NumInstances = 0;
}

bool NetplaySession::RebuildInstances(int numPlayers)
{
    if (numPlayers < 2 || numPlayers > kNetplayMaxPlayers)
        return false;
    if (numPlayers == NumInstances)
        return true;
    if (!ArgsBuilder || ROMData.empty())
        return false;

    Log(LogLevel::Info, "Netplay: rebuilding %d -> %d instances\n", NumInstances, numPlayers);

    std::vector<u8> rom = std::move(ROMData);
    DestroyInstances();

    NumInstances = numPlayers;
    if (!CreateInstances(ArgsBuilder, OrigUserdata))
        return false;

    return LoadROMData(rom.data(), (u32)rom.size());
}

bool NetplaySession::TakeState(int inst, std::vector<u8>& out)
{
    if (inst < 0 || inst >= NumInstances || !Instances[inst])
        return false;

    Savestate state;
    if (state.Error) return false;

    if (!Instances[inst]->DoSavestate(&state))
        return false;

    if (state.Error) return false;

    out.resize(state.Length());
    memcpy(out.data(), state.Buffer(), state.Length());
    return true;
}

bool NetplaySession::LoadState(int inst, const void* data, u32 len)
{
    if (inst < 0 || inst >= NumInstances || !Instances[inst])
        return false;

    Savestate state((void*)data, len, false);
    if (state.Error) return false;

    if (!Instances[inst]->DoSavestate(&state))
        return false;

    return !state.Error;
}

bool NetplaySession::TakeAllStates(std::vector<std::vector<u8>>& out)
{
    out.resize(NumInstances);
    for (int i = 0; i < NumInstances; i++)
    {
        if (!TakeState(i, out[i]))
            return false;
    }
    return true;
}

bool NetplaySession::LoadAllStates(const std::vector<std::vector<u8>>& states)
{
    if ((int)states.size() != NumInstances)
        return false;

    for (int i = 0; i < NumInstances; i++)
    {
        if (!LoadState(i, states[i].data(), (u32)states[i].size()))
            return false;
    }
    return true;
}

// ---- Input handling ----

void NetplaySession::SetLocalInput(const InputFrame& input)
{
    std::lock_guard<std::mutex> lock(InputMutex);

    // Apply input delay: this input will be used at frame (currentFrame + inputDelay)
    InputFrame delayed = input;
    delayed.FrameNum = CurrentFrame + InputDelay;

    u32 bufIdx = delayed.FrameNum % INPUT_BUF_SIZE;
    InputBuf[LocalPlayerID][bufIdx] = delayed;
    InputReady[LocalPlayerID][bufIdx] = true;
}

void NetplaySession::SetRemoteInput(int playerID, const InputFrame& input)
{
    if (playerID < 0 || playerID >= NumInstances)
        return;

    std::lock_guard<std::mutex> lock(InputMutex);

    u32 bufIdx = input.FrameNum % INPUT_BUF_SIZE;
    InputBuf[playerID][bufIdx] = input;
    InputReady[playerID][bufIdx] = true;
}

bool NetplaySession::ReadyForFrame(u32 frameNum) const
{
    std::lock_guard<std::mutex> lock(InputMutex);

    u32 bufIdx = frameNum % INPUT_BUF_SIZE;
    for (int i = 0; i < NumInstances; i++)
    {
        // Entries are never cleared -- the instances consume them at their own
        // pace -- so the stored frame number is what tells a fresh slot from
        // one left over 256 frames ago.
        if (!InputReady[i][bufIdx] || InputBuf[i][bufIdx].FrameNum != frameNum)
            return false;
    }
    return true;
}

void NetplaySession::SeedIdleInputs()
{
    std::lock_guard<std::mutex> lock(InputMutex);

    for (int p = 0; p < NumInstances; p++)
    {
        for (u32 f = CurrentFrame; f <= CurrentFrame + (u32)InputDelay; f++)
        {
            u32 idx = f % INPUT_BUF_SIZE;
            if (InputReady[p][idx] && InputBuf[p][idx].FrameNum == f)
                continue; // keep real input (the local player's, typically)

            InputFrame neutral = {};
            neutral.FrameNum = f;
            neutral.KeyMask = 0xFFF;
            InputBuf[p][idx] = neutral;
            InputReady[p][idx] = true;
        }
    }
}

void NetplaySession::ResetInputBuffers(u32 startFrame)
{
    std::lock_guard<std::mutex> lock(InputMutex);

    // Kill the whole history: stale entries carry frame numbers that match the
    // frames about to be replayed (the host free-ran through them), and a
    // leftover real input on one side vs neutral on the other is a desync.
    memset(InputReady, 0, sizeof(InputReady));

    for (int p = 0; p < NumInstances; p++)
    {
        for (u32 f = startFrame; f < startFrame + (u32)InputDelay; f++)
        {
            u32 idx = f % INPUT_BUF_SIZE;
            InputFrame neutral = {};
            neutral.FrameNum = f;
            neutral.KeyMask = 0xFFF;
            InputBuf[p][idx] = neutral;
            InputReady[p][idx] = true;
        }
    }
}

void NetplaySession::ApplyInput(int instIdx, u32 frame)
{
    InputFrame input;
    {
        std::lock_guard<std::mutex> lock(InputMutex);
        input = InputBuf[instIdx][frame % INPUT_BUF_SIZE];
    }

    Instances[instIdx]->SetKeyMask(input.KeyMask);

    if (input.Touching)
        Instances[instIdx]->TouchScreen(input.TouchX, input.TouchY);
    else
        Instances[instIdx]->ReleaseScreen();

    // Trace the local player's taps: where a touch stops on its way to the
    // console is otherwise invisible. One line per press and per release.
    if (instIdx == LocalPlayerID && (input.Touching != 0) != LastLoggedTouch)
    {
        LastLoggedTouch = (input.Touching != 0);
        Log(LogLevel::Info, "Netplay: touch %s at frame %u (%u,%u) on instance %d\n",
            LastLoggedTouch ? "down" : "up", frame, input.TouchX, input.TouchY, instIdx);
    }

    // Only on change: SetLidClosed(false) raises IRQ_LidOpen on the ARM7
    // every time it is called. Calling it unconditionally every frame storms
    // the firmware with 60 wake-up interrupts a second, which wedges the real
    // DS menu solid (frozen clock, dead input) on every mirror console.
    // The normal frontend only calls this when the lid hotkey toggles.
    bool lid = (input.LidClosed != 0);
    if (lid != Instances[instIdx]->IsLidClosed())
        Instances[instIdx]->SetLidClosed(lid);
}

// ---- Threading ----

// Each console runs its own frames as fast as its inputs allow. There is
// deliberately NO per-frame barrier between them: DS local wireless needs the
// guest console to answer the host console *within* a frame, and a barrier
// parks whichever one finishes first. The guest would sit there while the host
// burned RecvTimeout on every single exchange, which is what made the game
// freeze the moment multiplayer started.
//
// They stay in step through melonDS's own machinery instead -- LocalMP's
// semaphores plus Wifi's NextSync, which pace the consoles against each other
// in *emulated* time. That is the same arrangement melonDS's ordinary
// two-instance local multiplayer uses, and it works.
void NetplaySession::InstanceThreadFunc(int instIdx)
{
    while (ThreadsRunning)
    {
        u32 frame = InstanceFrame[instIdx].load(std::memory_order_relaxed);

        if (!ReadyForFrame(frame))
        {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }

        // The lockstep clock is session frame + cycles into it, so the frame
        // has to be published before the console runs it.
        LMP.SetInstanceFrame(instIdx, frame);

        ApplyInput(instIdx, frame);
        InstanceScanlines[instIdx] = Instances[instIdx]->RunFrame();

        // This console is now parked until its next input, so publish the whole
        // frame it just finished: a peer still inside that frame would
        // otherwise wait out LocalMP's 2-second cap on a clock that cannot move.
        LMP.LockstepSlot(instIdx).FrameEnd(frame);

        // Hash here, between frames, while this console is standing still.
        // Sampling it from the outside now that the threads free-run would be
        // reading a moving target and would report a desync every time.
        if (((frame + 1) % DESYNC_CHECK_INTERVAL) == 0)
        {
            InstanceHash[instIdx].store(HashInstance(instIdx), std::memory_order_relaxed);
            InstanceHashFrame[instIdx].store(frame + 1, std::memory_order_release);
        }

        InstanceFrame[instIdx].store(frame + 1, std::memory_order_release);
    }
}

void NetplaySession::StartThreads()
{
    if (ThreadsRunning) return;

    ThreadsRunning = true;

    for (int i = 0; i < NumInstances; i++)
    {
        InstanceFrame[i].store(CurrentFrame, std::memory_order_relaxed);
        LMP.SetInstanceFrame(i, CurrentFrame);
    }

    LMP.SetLockstep(true);

    for (int i = 0; i < NumInstances; i++)
        InstanceThreads[i] = std::thread(&NetplaySession::InstanceThreadFunc, this, i);

    Log(LogLevel::Info, "Netplay: started %d instance threads\n", NumInstances);
}

void NetplaySession::StopThreads()
{
    if (!ThreadsRunning) return;

    ThreadsRunning = false;

    // Drop lockstep first or this deadlocks: a console waiting for a peer's
    // emulated clock would wait for a thread that has already left its loop.
    LMP.SetLockstep(false);

    for (int i = 0; i < NumInstances; i++)
    {
        if (InstanceThreads[i].joinable())
            InstanceThreads[i].join();
    }

    Log(LogLevel::Info, "Netplay: stopped instance threads\n");
}

u32 NetplaySession::AlignInstances()
{
    // First frame whose input set is incomplete. Inputs are only ever seeded
    // by the emu thread (which is sitting in this very function) and by
    // HandleInputMessage (same thread, inside Poll) -- so this cannot move
    // under us. All players' seeds are written together each frame, which
    // makes this the one frame every instance thread will stall on.
    u32 stallFrame = CurrentFrame;
    while (ReadyForFrame(stallFrame) &&
           stallFrame < CurrentFrame + (u32)INPUT_BUF_SIZE)
        stallFrame++;

    if (ThreadsRunning)
    {
        // The threads walk themselves there; wait, but never forever.
        u64 t0 = Platform::GetMSCount();
        for (;;)
        {
            bool all = true;
            for (int i = 0; i < NumInstances; i++)
            {
                if (InstanceFrame[i].load(std::memory_order_acquire) < stallFrame)
                {
                    all = false;
                    break;
                }
            }
            if (all || (Platform::GetMSCount() - t0) > 5000)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        StopThreads();
    }

    // Belt and suspenders: run any straggler up to the stall frame ourselves.
    // MP exchanges against already-parked consoles just hit LocalMP's recv
    // timeout, so this is slow but bounded -- and normally a no-op.
    for (int i = 0; i < NumInstances; i++)
    {
        while (InstanceFrame[i].load(std::memory_order_relaxed) < stallFrame)
        {
            u32 f = InstanceFrame[i].load(std::memory_order_relaxed);
            if (!ReadyForFrame(f)) break; // cannot happen; never hang here
            LMP.SetInstanceFrame(i, f);
            ApplyInput(i, f);
            InstanceScanlines[i] = Instances[i]->RunFrame();
            InstanceFrame[i].store(f + 1, std::memory_order_release);
        }
    }

    Log(LogLevel::Info, "Netplay: aligned %d instances on frame %u (was %u)\n",
        NumInstances, stallFrame, CurrentFrame);

    CurrentFrame = stallFrame;
    return stallFrame;
}

u32 NetplaySession::RunFrame()
{
    if (!Active.load() || NumInstances == 0)
        return 0;

    // Frame-level trace: without it a stalled game is indistinguishable from a
    // stalled emulator. Reports how long each instance actually spent running,
    // and whether the DS consoles are still talking to each other.
    if ((CurrentFrame % 60) == 0)
    {
        u32 mp = LMP.GetTrafficCount();
        LocalMP::WaitStats ws = LMP.TakeWaitStats();
        Log(LogLevel::Info,
            "Netplay: waits %llu (free %llu) spins %llu, %llu ms\n",
            (unsigned long long)ws.Calls, (unsigned long long)ws.Cached,
            (unsigned long long)ws.Spins, (unsigned long long)ws.SpinMS);
        Log(LogLevel::Info,
            "Netplay: frame %u | last frame %u ms | MP packets since last report: %u | scanlines %u/%u | h0 %016llX@%u h1 %016llX@%u | sync %u ok / %u bad\n",
            CurrentFrame, LastFrameMS, mp - LastMPTraffic,
            InstanceScanlines[0], InstanceScanlines[NumInstances > 1 ? 1 : 0],
            (unsigned long long)InstanceHash[0].load(std::memory_order_relaxed),
            InstanceHashFrame[0].load(std::memory_order_relaxed),
            (unsigned long long)InstanceHash[NumInstances > 1 ? 1 : 0].load(std::memory_order_relaxed),
            InstanceHashFrame[NumInstances > 1 ? 1 : 0].load(std::memory_order_relaxed),
            MatchedCheckpoints, MismatchedCheckpoints);
        LastMPTraffic = mp;
    }

    u32 t0 = (u32)Platform::GetMSCount();

    if (!ThreadsRunning)
        StartThreads();

    // The consoles run themselves. Pace the frontend off the one we display,
    // so the window keeps up with the local player's console and the others are
    // free to lag or lead within whatever LocalMP allows them.
    u32 target = CurrentFrame + 1;
    const int stageAtStart = Stage.load();

    while (ThreadsRunning && Active.load() &&
           InstanceFrame[LocalPlayerID].load(std::memory_order_acquire) < target)
    {
        // Keep the link serviced while we wait. This thread is the only one
        // that receives remote input, and the console we are waiting for can
        // need the NEXT frame's input to get there (its peer console sits at a
        // frame boundary until then). Not polling here deadlocked the three of
        // them until LocalMP's 2-second cap fired -- once every few frames
        // during a download-play transfer, which is what dropped the session to
        // single-digit fps and then timed the peer out entirely.
        ProcessNetwork(0);

        // A handshake or teardown may have moved CurrentFrame under us. Leave
        // without counting this frame; the emu loop comes back through its
        // syncing branch.
        if (Stage.load() != stageAtStart)
            return InstanceScanlines[LocalPlayerID];

        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    LastFrameMS = (u32)Platform::GetMSCount() - t0;

    // Desync check, keyed off the newest checkpoint every console has actually
    // reached -- not off our own frame counter. The mirror consoles never
    // finish a frame at the same instant, so keying it off CurrentFrame meant
    // the hashes almost never lined up and the detector sat silent through the
    // entire desync it exists to catch.
    {
        u32 hf = InstanceHashFrame[0].load(std::memory_order_acquire);
        for (int i = 1; i < NumInstances; i++)
            hf = std::min(hf, InstanceHashFrame[i].load(std::memory_order_acquire));

        if (hf > LastHashFrame)
        {
            u64 hash = ComputeStateHash(hf);
            if (hash)
            {
                if (Transport.IsConnected())
                {
                    MsgDesyncAlert msg;
                    msg.Type = Msg_DesyncAlert;
                    msg.Frame = hf;
                    msg.Hash = hash;
                    Transport.Broadcast(&msg, sizeof(msg), Chan_Control, true);
                }

                HashFrames[HashHistPos] = hf;
                HashValues[HashHistPos] = hash;
                HashHistPos = (HashHistPos + 1) % HASH_HISTORY;

                LastStateHash = hash;
                LastHashFrame = hf;
            }
        }
    }

    CurrentFrame++;

    // Return scanlines from the display instance
    return InstanceScanlines[LocalPlayerID];
}

// ---- Display ----

NDS* NetplaySession::GetDisplayInstance() const
{
    if (LocalPlayerID >= 0 && LocalPlayerID < NumInstances)
        return Instances[LocalPlayerID];
    return nullptr;
}

NDS* NetplaySession::GetInstance(int idx) const
{
    if (idx >= 0 && idx < NumInstances)
        return Instances[idx];
    return nullptr;
}

bool NetplaySession::IsPlayerConnected(int playerID) const
{
    if (playerID < 0 || playerID >= NumInstances)
        return false;

    if (playerID == LocalPlayerID)
        return true;

    if (HostMode)
        return playerID >= 1 && Transport.IsPeerConnected(playerID - 1);

    // A client only holds a link to the host; other clients' state is the
    // host's business.
    return playerID == 0 && Transport.IsConnected();
}

// ---- Desync detection ----

u64 NetplaySession::HashInstance(int instIdx) const
{
    NDS* nds = Instances[instIdx];
    if (!nds) return 0;

    XXH64_state_t* hashState = XXH64_createState();
    XXH64_reset(hashState, 0);
    XXH64_update(hashState, nds->MainRAM, nds->MainRAMMask + 1);
    XXH64_update(hashState, &nds->ARM9.R, sizeof(nds->ARM9.R));
    XXH64_update(hashState, &nds->ARM7.R, sizeof(nds->ARM7.R));
    u64 hash = XXH64_digest(hashState);
    XXH64_freeState(hashState);
    return hash;
}

// Combined hash for `frame`, or 0 if not every console has reached it yet.
u64 NetplaySession::ComputeStateHash(u32 frame) const
{
    u64 combined = 0;
    for (int i = 0; i < NumInstances; i++)
    {
        if (InstanceHashFrame[i].load(std::memory_order_acquire) != frame)
            return 0;
        combined ^= InstanceHash[i].load(std::memory_order_relaxed) * (0x9E3779B97F4A7C15ull + i);
    }
    return combined ? combined : 1;
}

// ---- Network ----

bool NetplaySession::HostStart(int port)
{
    // Only accept as many clients as this session has instances for; extra
    // joiners would have no mirror console and crash on GetDisplayInstance().
    if (!Transport.StartHost(port, std::max(1, NumInstances - 1)))
        return false;

    HostMode = true;
    Stage.store(Stage_Idle);

    Transport.SetEventCallback([this](int peerIdx, bool connected) {
        if (connected)
        {
            // Sending a multi-megabyte savestate from inside the ENet poll is
            // asking for trouble; queue it and do it right after the poll ends.
            PendingSyncPeers |= (1u << peerIdx);
            Stage.store(Stage_Syncing);
        }
        else
        {
            // Drop any sync still queued for this peer: if it connected and
            // dropped within the same Poll(), the sync would otherwise run
            // against a dead slot and bump PendingStartAcks for an ack that can
            // never arrive -- wedging every future join in Stage_Syncing.
            PendingSyncPeers &= ~(1u << peerIdx);

            bool anyLeft = false;
            for (int p = 0; p < kNetplayMaxPlayers; p++)
                if (p != peerIdx && Transport.IsPeerConnected(p))
                    anyLeft = true;

            if (!anyLeft)
            {
                // Back to waiting: the frame gate and the input wait loop both
                // key off Stage, and without this they would spin forever on a
                // peer that is gone.
                PendingStartAcks = 0;
                Stage.store(Stage_Idle);
            }
            else if (Stage.load() == Stage_Syncing)
            {
                // Mid-handshake: the acks we are still waiting for include this
                // peer's. Re-run the whole thing for whoever is still here.
                PendingStartAcks = 0;
                PendingSyncPeers |= kResyncRemaining;
            }
            // Otherwise the session keeps running and SeedAbsentPlayers takes
            // over the empty seat -- from the host, so every machine sees that
            // seat go neutral at the very same frame.

            NotifyDisconnect(peerIdx + 1, Disconnect_Normal);
        }
    });

    return true;
}

bool NetplaySession::ClientConnect(const char* host, int port, int timeoutMs,
                                   const std::function<void()>& pollCb)
{
    if (!Transport.StartClient(host, port, timeoutMs, pollCb))
        return false;

    HostMode = false;
    Stage.store(Stage_Syncing);

    Transport.SetEventCallback([this](int peerIdx, bool connected) {
        if (!connected)
        {
            // Leave the running/syncing stage so the emu thread's wait loops
            // can exit instead of freezing the app.
            Stage.store(Stage_Idle);
            NotifyDisconnect(0, Disconnect_Normal);
        }
    });

    return true;
}

// Host side of the handshake. Everyone in the session re-syncs whenever anyone
// joins, not just the newcomer: the players already running have to restart
// their input history from the same frame as the joiner, and the cartless
// download-play consoles get reset on every machine on every join (a savestate
// cannot carry them, see ResetCartlessInstance). Re-shipping the states is what
// makes all of that land identically on every machine.
//
// newPeerMask are the peers that have nothing yet -- only they need the cart,
// firmware and BIOS, which is the expensive part of a join.
void NetplaySession::HostSyncPeers(u32 newPeerMask)
{
    u32 peerMask = 0;
    for (int p = 0; p < kNetplayMaxPlayers; p++)
        if (Transport.IsPeerConnected(p))
            peerMask |= (1u << p);

    if (!peerMask)
    {
        // Everyone left again before we got here.
        PendingStartAcks = 0;
        Stage.store(Stage_Idle);
        return;
    }

    Stage.store(Stage_Syncing);
    PendingStartAcks = 0;

    for (int p = 0; p < kNetplayMaxPlayers; p++)
    {
        if (!(peerMask & (1u << p))) continue;

        MsgSessionOffer offer;
        offer.Type = Msg_SessionOffer;
        offer.ROMHash = ROMHash;
        offer.NumPlayers = (u8)NumInstances;
        offer.InputDelay = (u8)InputDelay;
        offer.DownloadPlay = DownloadPlay ? 1 : 0;
        offer.JITEnable = JITConfig.Enable ? 1 : 0;
        offer.JITMaxBlockSize = (u8)JITConfig.MaxBlockSize;
        offer.JITLiteralOpt = JITConfig.LiteralOpt ? 1 : 0;
        offer.JITBranchOpt = JITConfig.BranchOpt ? 1 : 0;
        offer.JITFastMemory = JITConfig.FastMemory ? 1 : 0;
        Transport.SendTo(p, &offer, sizeof(offer), Chan_Control, true);

        MsgSessionAccept accept;
        accept.Type = Msg_SessionAccept;
        accept.PlayerID = (u8)(p + 1);
        Transport.SendTo(p, &accept, sizeof(accept), Chan_Control, true);
    }

    // A joining player brings nothing but melonDS. Ship it the firmware and
    // BIOS every machine must agree on, then the cart itself. The cart goes
    // last: its arrival is what triggers the client's instance build, so
    // everything the build consumes has to be there first.
    for (int p = 0; p < kNetplayMaxPlayers; p++)
    {
        if (!(newPeerMask & peerMask & (1u << p))) continue;

        if (!FirmwareData.empty())
            BlobTransfer::Send(Transport, p, Blob_Firmware, FirmwareData.data(), (u32)FirmwareData.size());

        if (!BIOS9Data.empty())
            BlobTransfer::Send(Transport, p, Blob_BIOS9, BIOS9Data.data(), (u32)BIOS9Data.size());
        if (!BIOS7Data.empty())
            BlobTransfer::Send(Transport, p, Blob_BIOS7, BIOS7Data.data(), (u32)BIOS7Data.size());

        BlobTransfer::Send(Transport, p, Blob_CartROM, ROMData.data(), (u32)ROMData.size());
    }

    // Savestates are only needed if this session already ran. At frame 0 every
    // machine is a plain reset of the same cart and firmware -- byte-identical
    // already. Skipping them there takes ~38MB off the join. With the host
    // free-running while it waits, though, the normal case is frame != 0.
    if (CurrentFrame == 0)
    {
        Log(LogLevel::Info, "Netplay: peers joined at frame 0, skipping state transfer\n");
    }
    else
    {
        // Park every mirror console on one common frame first: snapshots taken
        // at whatever frame each instance happened to be on are a guaranteed
        // desync. This also stops the instance threads, so the states below
        // are taken from standing-still consoles.
        AlignInstances();

        // Every peer starts its input history fresh at this frame; ours still
        // holds the frames we just ran. All sides must agree: neutral input for
        // the first InputDelay frames. (Clients do the same in Msg_StartGame.)
        ResetInputBuffers(CurrentFrame);

        // Taken once and shipped to everyone -- taking them per peer would let
        // the consoles move in between and hand the peers different worlds.
        std::vector<std::vector<u8>> states;
        if (!TakeSyncStates(states))
        {
            Log(LogLevel::Error, "Netplay: failed to take sync states\n");
            Stage.store(Stage_Idle);
            return;
        }

        for (int p = 0; p < kNetplayMaxPlayers; p++)
        {
            if (!(peerMask & (1u << p))) continue;
            SendSyncStates(p, states);
        }
    }

    // Same starting point for the emulated wireless as every client will have.
    ResetWirelessWorld();

    for (int p = 0; p < kNetplayMaxPlayers; p++)
    {
        if (!(peerMask & (1u << p))) continue;

        // Chan_Control is reliable and ordered, so this lands after the blobs.
        MsgStartGame start;
        start.Type = Msg_StartGame;
        start.Frame = CurrentFrame;
        start.InputDelay = (u8)InputDelay;
        Transport.SendTo(p, &start, sizeof(start), Chan_Control, true);

        Log(LogLevel::Info, "Netplay: peer %d synced as player %d at frame %u\n",
            p, p + 1, CurrentFrame);

        // Do NOT go to Stage_Running yet: each client still has to receive all
        // of that, apply it and build its consoles, which can take a while.
        // They send Msg_StartAck when done; until every one of them has, stay
        // in Stage_Syncing (the frontend keeps drawing and pumping the UI).
        PendingStartAcks++;
    }
}

void NetplaySession::ProcessNetwork(int timeoutMs)
{
    if (!Transport.IsConnected())
    {
        // Nothing to poll yet -- honor the caller's pacing so wait loops
        // don't spin hot while the transport comes up.
        if (timeoutMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
        return;
    }

    Transport.Poll([this](int peerIdx, const u8* data, u32 len, int channel) {
        if (len < 1) return;

        if (channel == Chan_Control)
            HandleControlMessage(peerIdx, data, len);
        else if (channel == Chan_Input)
            HandleInputMessage(peerIdx, data, len);
    }, timeoutMs);

    if (HostMode && PendingSyncPeers)
    {
        u32 newPeers = PendingSyncPeers & ~kResyncRemaining;
        PendingSyncPeers = 0;
        HostSyncPeers(newPeers);
    }
}

void NetplaySession::SendLocalInput(const InputFrame& input)
{
    if (!Transport.IsConnected())
        return;

    MsgInputFrame msg;
    msg.Type = Msg_InputFrame;
    msg.PlayerID = (u8)LocalPlayerID;
    msg.Input = input;
    Transport.Broadcast(&msg, sizeof(msg), Chan_Input, true);
}

// Host only. Clients have no link to each other, so every seat's input reaches
// them through here. `fromPeer` is skipped (it is where the input came from);
// pass -1 to send to everybody.
void NetplaySession::RelayInput(int fromPeer, int playerID, const InputFrame& input)
{
    if (!HostMode || !Transport.IsConnected())
        return;

    MsgInputFrame msg;
    msg.Type = Msg_InputFrame;
    msg.PlayerID = (u8)playerID;
    msg.Input = input;

    for (int p = 0; p < kNetplayMaxPlayers; p++)
    {
        if (p == fromPeer || !Transport.IsPeerConnected(p))
            continue;
        Transport.SendTo(p, &msg, sizeof(msg), Chan_Input, true);
    }
}

// Host only. A seat with nobody in it still has to produce input every frame or
// ReadyForFrame never passes and every console in the session stalls. The host
// is the single source of truth for those seats -- it seeds them AND relays the
// same frames it seeded, so no machine has to decide for itself when a player
// went away (they would each notice at a different frame, which is a desync).
void NetplaySession::SeedAbsentPlayers()
{
    if (!HostMode)
        return;

    u32 frame = CurrentFrame + (u32)InputDelay;

    for (int p = 1; p < NumInstances; p++)
    {
        if (Transport.IsPeerConnected(p - 1))
            continue;

        InputFrame neutral = {};
        neutral.FrameNum = frame;
        neutral.KeyMask = 0xFFF;

        {
            std::lock_guard<std::mutex> lock(InputMutex);
            u32 idx = frame % INPUT_BUF_SIZE;
            if (InputReady[p][idx] && InputBuf[p][idx].FrameNum == frame)
                continue;
            InputBuf[p][idx] = neutral;
            InputReady[p][idx] = true;
        }

        RelayInput(-1, p, neutral);
    }
}

void NetplaySession::HandleControlMessage(int peerIdx, const u8* data, u32 len)
{
    if (len < 1) return;

    u8 type = data[0];

    switch (type)
    {
    case Msg_SessionOffer:
    {
        if (len < sizeof(MsgSessionOffer)) break;
        const MsgSessionOffer* msg = (const MsgSessionOffer*)data;
        Log(LogLevel::Info, "Netplay: received session offer (players: %d, delay: %d)\n",
            msg->NumPlayers, msg->InputDelay);

        // Everything we need is on its way: player count, delay, and the cart
        // and firmware themselves. Nothing has to exist locally beforehand.
        OfferedPlayers = msg->NumPlayers;
        InputDelay = msg->InputDelay;
        DownloadPlay = (msg->DownloadPlay != 0);

        // Run the host's recompiler settings, not our own: different block
        // sizes or optimizations mean different instruction timing, and the
        // two machines part ways on the first interrupt that lands in a
        // different block.
        JITConfig.Enable = (msg->JITEnable != 0);
        JITConfig.MaxBlockSize = msg->JITMaxBlockSize;
        JITConfig.LiteralOpt = (msg->JITLiteralOpt != 0);
        JITConfig.BranchOpt = (msg->JITBranchOpt != 0);
        JITConfig.FastMemory = (msg->JITFastMemory != 0);

        Stage.store(Stage_Syncing);

        MsgSyncReady ready;
        ready.Type = Msg_SyncReady;
        Transport.SendTo(peerIdx, &ready, sizeof(ready), Chan_Control, true);
        break;
    }

    case Msg_SessionAccept:
    {
        if (len < sizeof(MsgSessionAccept)) break;
        const MsgSessionAccept* msg = (const MsgSessionAccept*)data;
        LocalPlayerID = msg->PlayerID;
        Log(LogLevel::Info, "Netplay: session accepted, assigned player ID %d\n", msg->PlayerID);
        break;
    }

    case Msg_BlobStart:
    {
        if (len < sizeof(MsgBlobStart)) break;
        // Only BlobStart/BlobEnd carry the type, so latch it here and send the
        // chunks in between to the same receiver.
        CurBlobIdx = ((const MsgBlobStart*)data)->BlobType;
        if (CurBlobIdx < 0 || CurBlobIdx >= Blob_MAX) { CurBlobIdx = -1; break; }
        BlobRecv[CurBlobIdx].OnMessage(data, len);
        break;
    }

    case Msg_BlobChunk:
    {
        if (CurBlobIdx < 0) break;
        BlobRecv[CurBlobIdx].OnMessage(data, len);
        break;
    }

    case Msg_BlobEnd:
    {
        if (CurBlobIdx < 0) break;
        if (BlobRecv[CurBlobIdx].OnMessage(data, len))
        {
            Log(LogLevel::Info, "Netplay: blob %d received\n", CurBlobIdx);

            if (CurBlobIdx == Blob_Firmware)
            {
                const auto& fw = BlobRecv[Blob_Firmware].GetData();
                SetSharedData(fw.data(), (u32)fw.size());
                BlobRecv[Blob_Firmware].Reset();
            }
            else if (CurBlobIdx == Blob_BIOS9)
            {
                BIOS9Data = BlobRecv[Blob_BIOS9].GetData();
                BlobRecv[Blob_BIOS9].Reset();
            }
            else if (CurBlobIdx == Blob_BIOS7)
            {
                BIOS7Data = BlobRecv[Blob_BIOS7].GetData();
                BlobRecv[Blob_BIOS7].Reset();
            }
            else if (CurBlobIdx == Blob_CartROM)
            {
                // Cart and firmware are both here -- now we can build the same
                // set of consoles the host is running.
                const auto& rom = BlobRecv[Blob_CartROM].GetData();
                std::vector<u8> romcopy = rom;
                BlobRecv[Blob_CartROM].Reset();

                DestroyInstances();
                NumInstances = OfferedPlayers > 0 ? OfferedPlayers : 2;
                if (!CreateInstances(ArgsBuilder, OrigUserdata) ||
                    !LoadROMData(romcopy.data(), (u32)romcopy.size()))
                {
                    Log(LogLevel::Error, "Netplay: failed to build instances from host data\n");
                    Stage.store(Stage_Idle);
                    NotifyDisconnect(0, Disconnect_Error);
                }
            }
        }
        CurBlobIdx = -1;
        break;
    }

    case Msg_SyncReady:
    {
        Log(LogLevel::Info, "Netplay: peer %d is sync ready\n", peerIdx);
        break;
    }

    case Msg_StartGame:
    {
        if (len < sizeof(MsgStartGame)) break;
        const MsgStartGame* msg = (const MsgStartGame*)data;

        if (!HasInstances())
        {
            Log(LogLevel::Error, "Netplay: start received before the cart arrived\n");
            Stage.store(Stage_Idle);
            NotifyDisconnect(0, Disconnect_Error);
            break;
        }

        // A player already in the session gets this too whenever somebody else
        // joins -- and unlike a newcomer, its consoles are running. Park them
        // before anything is loaded into them.
        StopThreads();

        // Joining at frame 0 means the host skipped the state transfer: our
        // freshly reset instances already match it.
        if (msg->Frame != 0 && !ClientReceiveStates())
        {
            Log(LogLevel::Error, "Netplay: state sync incomplete, cannot start\n");
            Stage.store(Stage_Idle);
            NotifyDisconnect(0, Disconnect_Error);
            break;
        }

        CurrentFrame = msg->Frame;
        InputDelay = msg->InputDelay;

        // Same starting point for the emulated wireless as the host just took.
        ResetWirelessWorld();

        // Both sides owe each other the first InputDelay frames of input, and
        // neither has sent any yet -- wipe the history and seed those frames
        // as neutral, exactly like the host did before taking the states.
        ResetInputBuffers(CurrentFrame);

        Log(LogLevel::Info, "Netplay: starting game at frame %d with delay %d\n",
            msg->Frame, msg->InputDelay);
        Stage.store(Stage_Running);

        // Start barrier: tell the host our instances are actually built and
        // loaded, so it knows it is safe to start running frames.
        {
            MsgStartAck ack;
            ack.Type = Msg_StartAck;
            Transport.SendTo(peerIdx, &ack, sizeof(ack), Chan_Control, true);
        }

        // The transfer is over -- drop the dead-host detection from the
        // join-survival 30-60s down to something a player would call prompt.
        Transport.SetPeerTimeout(peerIdx, 4000, 10000);
        break;
    }

    case Msg_StartAck:
    {
        if (!HostMode) break;

        if (PendingStartAcks > 0)
            PendingStartAcks--;
        Log(LogLevel::Info, "Netplay: peer %d acked start (%d still pending)\n",
            peerIdx, PendingStartAcks);

        // This peer survived the transfer; a vanished client must now stall
        // the session for seconds, not the join-survival 30-60s.
        Transport.SetPeerTimeout(peerIdx, 4000, 10000);

        if (PendingStartAcks == 0 && Stage.load() == Stage_Syncing)
            Stage.store(Stage_Running);
        break;
    }

    case Msg_DesyncAlert:
    {
        if (len < sizeof(MsgDesyncAlert)) break;
        const MsgDesyncAlert* msg = (const MsgDesyncAlert*)data;

        // Compare against our own checkpoint for that frame. Keep a few of
        // them: the two machines take their checkpoints independently, so an
        // alert routinely arrives while we have already moved on to the next
        // one, and comparing only the newest threw those away unexamined.
        for (int i = 0; i < HASH_HISTORY; i++)
        {
            if (HashFrames[i] != msg->Frame || HashValues[i] == 0)
                continue;

            if (HashValues[i] != msg->Hash)
            {
                MismatchedCheckpoints++;
                Log(LogLevel::Error, "Netplay: DESYNC detected at frame %u! "
                    "Local hash: %016llX, remote hash: %016llX\n",
                    msg->Frame, (unsigned long long)HashValues[i], (unsigned long long)msg->Hash);

                NotifyDesync(msg->Frame, HashValues[i], msg->Hash);
            }
            else
                MatchedCheckpoints++;
            break;
        }
        break;
    }

    case Msg_Disconnect:
    {
        if (len < sizeof(MsgDisconnect)) break;
        const MsgDisconnect* msg = (const MsgDisconnect*)data;
        Log(LogLevel::Info, "Netplay: peer %d disconnected (reason: %d)\n",
            peerIdx, msg->Reason);

        // Leave the running stage so the wait loops can exit.
        Stage.store(Stage_Idle);
        NotifyDisconnect(peerIdx, (NetplayDisconnectReason)msg->Reason);
        break;
    }

    default:
        Log(LogLevel::Warn, "Netplay: unknown control message type 0x%02X\n", type);
        break;
    }
}

void NetplaySession::HandleInputMessage(int peerIdx, const u8* data, u32 len)
{
    if (len < 1) return;

    u8 type = data[0];

    switch (type)
    {
    case Msg_InputFrame:
    {
        if (len < sizeof(MsgInputFrame)) break;
        const MsgInputFrame* msg = (const MsgInputFrame*)data;

        int playerID;
        if (HostMode)
        {
            // A client only ever speaks for itself: its seat is the slot it
            // connected on, never what it claims. Then pass it on -- the other
            // clients have no way to hear it otherwise.
            playerID = peerIdx + 1;
            RelayInput(peerIdx, playerID, msg->Input);
        }
        else
        {
            // Everything arrives through the host, tagged with whose seat it
            // is: our own, we already have.
            playerID = msg->PlayerID;
            if (playerID < 0 || playerID >= NumInstances || playerID == LocalPlayerID)
                break;
        }

        SetRemoteInput(playerID, msg->Input);
        break;
    }

    case Msg_InputBatch:
    {
        if (len < sizeof(MsgInputBatch)) break;
        const MsgInputBatch* msg = (const MsgInputBatch*)data;
        u32 expectedLen = sizeof(MsgInputBatch) + msg->Count * sizeof(InputFrame);
        if (len < expectedLen) break;

        int playerID;
        if (HostMode)
            playerID = peerIdx + 1;
        else
        {
            playerID = msg->PlayerID;
            if (playerID < 0 || playerID >= NumInstances || playerID == LocalPlayerID)
                break;
        }

        const InputFrame* frames = (const InputFrame*)(data + sizeof(MsgInputBatch));
        for (int i = 0; i < msg->Count; i++)
        {
            SetRemoteInput(playerID, frames[i]);
        }
        break;
    }

    default:
        break;
    }
}

// ---- State sync ----

// Cartless download-play mirrors (the guest consoles sitting in the DS menu)
// do NOT get their state transferred on join. Resuming the firmware menu from
// a savestate reliably wedges it on the machine that loads it cold (the ARM7
// ends up spinning with IRQs masked in CPSR, every enabled interrupt stays
// pending, the menu freezes and ignores all input) -- while the game console's
// state transfers perfectly. There is nothing worth preserving on them anyway:
// until the joining player acts, they only ever received neutral input. So on
// every join, BOTH sides put them through the same reset ritual and let them
// boot the menu in lockstep from emulated time zero.
void NetplaySession::ResetCartlessInstance(int i)
{
    if (!Instances[i]) return;

    Instances[i]->Reset();
    // Same fixed RTC as CreateInstances: byte-identical on every machine, and
    // the cleared power-lost flag keeps the firmware out of the setup wizard.
    Instances[i]->RTC.SetDateTime(2000, 1, 1, 0, 0, 0);
    Instances[i]->Start();
}

// Everything about the emulated wireless that a savestate does not carry, put
// back to one known state on every machine at the same emulated moment.
//
// A host that has been running since the previous round has dirty firmware,
// packets still queued from it, and consoles the MP layer still counts as
// powered (Wifi::Reset clears PowerOn behind its back). A player joining brings
// none of that. Each one of those differences changes what the emulated
// wireless does -- the connected bitmask alone decides whether a CMD frame is
// considered answered -- so the first session after a rejoin desynced within
// seconds while the first one had been fine.
//
// Run this after the states are in place, on host and clients alike.
void NetplaySession::ResetWirelessWorld()
{
    LMP.ResetQueues();

    for (int i = 0; i < NumInstances; i++)
    {
        if (!Instances[i]) continue;

        // Say out loud whether this console's wireless is on, instead of
        // leaving the MP layer with whatever it happened to be told last.
        if (Instances[i]->Wifi.IsPowerOn())
            LMP.Begin(i);
        else
            LMP.End(i);
    }

    // ponytail: the firmware is NOT reinstalled here, though it is the third
    // thing a savestate does not carry. Doing that broke touch input on the
    // guest consoles, and it was speculative to begin with -- a console only
    // writes to its firmware when the user changes settings in the DS menu.
    // If a stale firmware ever does show up as a desync, ship the host's live
    // firmware bytes with the savestates instead of rebuilding them locally.
}

bool NetplaySession::IsStateTransferInstance(int i) const
{
    // Download play: only instance 0 owns the cart; the cartless menu consoles
    // are reset on join instead of savestated.
    return !(DownloadPlay && i != 0);
}

// Snapshot every console once. Entry i is empty for the instances that are not
// transferred (the cartless download-play mirrors); those are reset here, and
// every client resets them too when the start message lands.
bool NetplaySession::TakeSyncStates(std::vector<std::vector<u8>>& out)
{
    out.assign(NumInstances, std::vector<u8>());

    for (int i = 0; i < NumInstances; i++)
    {
        if (!IsStateTransferInstance(i))
        {
            ResetCartlessInstance(i);
            Log(LogLevel::Info, "Netplay: instance %d is cartless, reset for lockstep boot instead of state transfer\n", i);
            continue;
        }

        if (!TakeState(i, out[i]))
        {
            Log(LogLevel::Error, "Netplay: failed to take state for instance %d\n", i);
            return false;
        }

        // Resume from the exact bytes we just shipped. Loading a savestate is
        // not perfectly round-trip identical to having kept running (verified:
        // the cartless firmware-menu console diverges within a frame of the
        // client loading it, while the host continues from live state). The
        // cure is symmetry: whatever quirks the load path has, every machine
        // must experience the same ones -- so the host reloads its own states
        // and everyone evolves from identical footing.
        if (!LoadState(i, out[i].data(), (u32)out[i].size()))
        {
            Log(LogLevel::Error, "Netplay: failed to reload own state for instance %d\n", i);
            return false;
        }
    }

    return true;
}

void NetplaySession::SendSyncStates(int clientIdx, const std::vector<std::vector<u8>>& states)
{
    for (int i = 0; i < (int)states.size(); i++)
    {
        if (states[i].empty()) continue; // cartless: reset on both ends instead

        NetplayBlobType blobType = (NetplayBlobType)(Blob_Savestate0 + i);
        BlobTransfer::Send(Transport, clientIdx, blobType, states[i].data(), (u32)states[i].size());
    }

    // Also send SRAM for instance 0
    const u8* sram = Instances[0]->GetNDSSave();
    u32 sramLen = Instances[0]->GetNDSSaveLength();
    if (sram && sramLen > 0)
    {
        BlobTransfer::Send(Transport, clientIdx, Blob_SRAM, sram, sramLen);
    }
}

bool NetplaySession::ClientReceiveStates()
{
    // Wait for all instance states to be received
    // This is called in a polling loop
    for (int i = 0; i < NumInstances; i++)
    {
        if (!IsStateTransferInstance(i))
            continue; // reset on join, no state expected

        int blobIdx = Blob_Savestate0 + i;
        if (blobIdx >= Blob_MAX) break;

        if (!BlobRecv[blobIdx].IsComplete())
            return false;
    }

    // All states received - apply them
    for (int i = 0; i < NumInstances; i++)
    {
        if (!IsStateTransferInstance(i))
        {
            // Mirror of the host's join-time ritual, so both sides boot the
            // menu console in lockstep from emulated time zero.
            ResetCartlessInstance(i);
            Log(LogLevel::Info, "Netplay: instance %d is cartless, reset for lockstep boot\n", i);
            continue;
        }

        int blobIdx = Blob_Savestate0 + i;
        if (blobIdx >= Blob_MAX) break;

        const auto& blobData = BlobRecv[blobIdx].GetData();
        if (!LoadState(i, blobData.data(), (u32)blobData.size()))
        {
            Log(LogLevel::Error, "Netplay: failed to load state for instance %d\n", i);
            return false;
        }

        BlobRecv[blobIdx].Reset();
    }

    // Apply SRAM if received
    if (BlobRecv[Blob_SRAM].IsComplete())
    {
        const auto& sramData = BlobRecv[Blob_SRAM].GetData();
        Instances[0]->SetNDSSave(sramData.data(), (u32)sramData.size());
        BlobRecv[Blob_SRAM].Reset();
    }

    Log(LogLevel::Info, "Netplay: all states loaded successfully\n");
    return true;
}

// Hold CallbackMutex across the invocation, not just while copying: the whole
// point of ~NetplayDialog unhooking these is that after SetXCallback(nullptr)
// returns, no call into the (dying) dialog can still be in flight. A copy-then-
// call-unlocked pattern leaves exactly that window open. This is safe because
// the callbacks only post queued Qt events and never block -- a callback that
// re-enters SetXCallback would deadlock, so don't.
void NetplaySession::NotifyDesync(u32 frame, u64 localHash, u64 remoteHash)
{
    std::lock_guard<std::mutex> lock(CallbackMutex);
    if (OnDesync) OnDesync(frame, localHash, remoteHash);
}

void NetplaySession::NotifyDisconnect(int playerID, NetplayDisconnectReason reason)
{
    std::lock_guard<std::mutex> lock(CallbackMutex);
    if (OnDisconnect) OnDisconnect(playerID, reason);
}

}
