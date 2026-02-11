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
    StopThreads();

    Active.store(false);

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

bool NetplaySession::CreateInstances(const NDSArgsBuilder& argsBuilder)
{
    for (int i = 0; i < NumInstances; i++)
    {
        NDSArgs args = argsBuilder();

        // Each instance gets a NetplayInstanceData as userdata
        // so that Platform::MP_* callbacks can route to the correct LocalMP instance
        InstData[i].InstID = i;
        InstData[i].Session = this;

        Instances[i] = new NDS(std::move(args), &InstData[i]);

        if (!Instances[i])
        {
            Log(LogLevel::Error, "Netplay: failed to create NDS instance %d\n", i);
            return false;
        }

        Instances[i]->Reset();

        // Register this instance with LocalMP
        LMP.Begin(i);
    }

    MuteNonLocalInstances();

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
    u32 bufIdx = frameNum % INPUT_BUF_SIZE;
    for (int i = 0; i < NumInstances; i++)
    {
        if (!InputReady[i][bufIdx])
            return false;
    }
    return true;
}

void NetplaySession::ApplyInputs(u32 frame)
{
    u32 bufIdx = frame % INPUT_BUF_SIZE;

    for (int i = 0; i < NumInstances; i++)
    {
        const InputFrame& input = InputBuf[i][bufIdx];
        Instances[i]->SetKeyMask(input.KeyMask);

        if (input.Touching)
            Instances[i]->TouchScreen(input.TouchX, input.TouchY);
        else
            Instances[i]->ReleaseScreen();

        Instances[i]->SetLidClosed(input.LidClosed != 0);
    }

    // Mark this frame's inputs as consumed
    for (int i = 0; i < NumInstances; i++)
        InputReady[i][bufIdx] = false;
}

// ---- Threading ----

void NetplaySession::InstanceThreadFunc(int instIdx)
{
    while (ThreadsRunning)
    {
        // Wait at the barrier for all threads to be ready
        FrameBarrier->arrive_and_wait();

        if (!ThreadsRunning) break;

        // Run one frame
        InstanceScanlines[instIdx] = Instances[instIdx]->RunFrame();

        // Wait at the barrier for all threads to finish
        FrameBarrier->arrive_and_wait();
    }
}

void NetplaySession::StartThreads()
{
    if (ThreadsRunning) return;

    ThreadsRunning = true;

    // Create barrier for (NumInstances + 1) participants:
    // NumInstances instance threads + 1 main thread
    FrameBarrier = std::make_unique<SimpleBarrier>(NumInstances + 1);

    for (int i = 0; i < NumInstances; i++)
    {
        InstanceThreads[i] = std::thread(&NetplaySession::InstanceThreadFunc, this, i);
    }

    Log(LogLevel::Info, "Netplay: started %d instance threads\n", NumInstances);
}

void NetplaySession::StopThreads()
{
    if (!ThreadsRunning) return;

    ThreadsRunning = false;

    // Unblock all threads waiting at the barrier
    if (FrameBarrier)
        FrameBarrier->arrive_and_wait();

    for (int i = 0; i < NumInstances; i++)
    {
        if (InstanceThreads[i].joinable())
            InstanceThreads[i].join();
    }

    FrameBarrier.reset();

    Log(LogLevel::Info, "Netplay: stopped instance threads\n");
}

u32 NetplaySession::RunFrame()
{
    if (!Active.load() || NumInstances == 0)
        return 0;

    // Apply inputs for current frame to all instances
    ApplyInputs(CurrentFrame);

    if (!ThreadsRunning)
    {
        StartThreads();
    }

    // Signal all instance threads to run one frame
    FrameBarrier->arrive_and_wait();

    // Wait for all instance threads to finish
    FrameBarrier->arrive_and_wait();

    // Desync check every DESYNC_CHECK_INTERVAL frames
    if (CurrentFrame > 0 && (CurrentFrame % DESYNC_CHECK_INTERVAL) == 0)
    {
        u64 hash = ComputeStateHash();

        if (Transport.IsConnected())
        {
            // Send our hash
            MsgDesyncAlert msg;
            msg.Type = Msg_DesyncAlert;
            msg.Frame = CurrentFrame;
            msg.Hash = hash;
            Transport.Broadcast(&msg, sizeof(msg), Chan_Control, true);
        }

        LastStateHash = hash;
        LastHashFrame = CurrentFrame;
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

// ---- Desync detection ----

u64 NetplaySession::ComputeStateHash() const
{
    XXH64_state_t* hashState = XXH64_createState();
    XXH64_reset(hashState, 0);

    for (int i = 0; i < NumInstances; i++)
    {
        if (!Instances[i]) continue;

        // Hash main RAM
        XXH64_update(hashState, Instances[i]->MainRAM, Instances[i]->MainRAMMask + 1);

        // Hash CPU registers
        XXH64_update(hashState, &Instances[i]->ARM9.R, sizeof(Instances[i]->ARM9.R));
        XXH64_update(hashState, &Instances[i]->ARM7.R, sizeof(Instances[i]->ARM7.R));
    }

    u64 hash = XXH64_digest(hashState);
    XXH64_freeState(hashState);
    return hash;
}

// ---- Network ----

bool NetplaySession::HostStart(int port)
{
    if (!Transport.StartHost(port, kNetplayMaxPlayers - 1))
        return false;

    HostMode = true;

    Transport.SetEventCallback([this](int peerIdx, bool connected) {
        if (!connected && OnDisconnect)
        {
            OnDisconnect(peerIdx + 1, Disconnect_Normal);
        }
    });

    return true;
}

bool NetplaySession::ClientConnect(const char* host, int port)
{
    if (!Transport.StartClient(host, port))
        return false;

    HostMode = false;

    Transport.SetEventCallback([this](int peerIdx, bool connected) {
        if (!connected && OnDisconnect)
        {
            OnDisconnect(0, Disconnect_Normal);
        }
    });

    return true;
}

void NetplaySession::ProcessNetwork()
{
    if (!Transport.IsConnected())
        return;

    Transport.Poll([this](int peerIdx, const u8* data, u32 len, int channel) {
        if (len < 1) return;

        if (channel == Chan_Control)
            HandleControlMessage(peerIdx, data, len);
        else if (channel == Chan_Input)
            HandleInputMessage(peerIdx, data, len);
    });
}

void NetplaySession::SendLocalInput(const InputFrame& input)
{
    if (!Transport.IsConnected())
        return;

    MsgInputFrame msg;
    msg.Type = Msg_InputFrame;
    msg.Input = input;
    Transport.Broadcast(&msg, sizeof(msg), Chan_Input, true);
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
        // Client accepts
        MsgSessionAccept accept;
        accept.Type = Msg_SessionAccept;
        accept.PlayerID = 0; // will be assigned by host
        Transport.SendTo(peerIdx, &accept, sizeof(accept), Chan_Control, true);
        break;
    }

    case Msg_SessionAccept:
    {
        if (len < sizeof(MsgSessionAccept)) break;
        const MsgSessionAccept* msg = (const MsgSessionAccept*)data;
        Log(LogLevel::Info, "Netplay: session accepted, assigned player ID %d\n", msg->PlayerID);
        break;
    }

    case Msg_BlobStart:
    case Msg_BlobChunk:
    case Msg_BlobEnd:
    {
        // Route to appropriate blob receiver based on blob type
        for (int i = 0; i < Blob_MAX; i++)
        {
            if (BlobRecv[i].OnMessage(data, len))
            {
                // Blob complete
                Log(LogLevel::Info, "Netplay: blob %d received\n", i);
                break;
            }
        }
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
        CurrentFrame = msg->Frame;
        InputDelay = msg->InputDelay;
        Log(LogLevel::Info, "Netplay: starting game at frame %d with delay %d\n",
            msg->Frame, msg->InputDelay);
        break;
    }

    case Msg_DesyncAlert:
    {
        if (len < sizeof(MsgDesyncAlert)) break;
        const MsgDesyncAlert* msg = (const MsgDesyncAlert*)data;

        // Compare with our hash at the same frame
        if (msg->Frame == LastHashFrame && msg->Hash != LastStateHash)
        {
            Log(LogLevel::Error, "Netplay: DESYNC detected at frame %d! "
                "Local hash: %016llX, remote hash: %016llX\n",
                msg->Frame, (unsigned long long)LastStateHash, (unsigned long long)msg->Hash);

            if (OnDesync)
                OnDesync(msg->Frame, LastStateHash, msg->Hash);
        }
        break;
    }

    case Msg_Disconnect:
    {
        if (len < sizeof(MsgDisconnect)) break;
        const MsgDisconnect* msg = (const MsgDisconnect*)data;
        Log(LogLevel::Info, "Netplay: peer %d disconnected (reason: %d)\n",
            peerIdx, msg->Reason);

        if (OnDisconnect)
            OnDisconnect(peerIdx, (NetplayDisconnectReason)msg->Reason);
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

        // Determine which player this input belongs to
        // For host: peerIdx + 1 (clients are players 1, 2, 3)
        // For client: peerIdx 0 = host = player 0
        int playerID;
        if (HostMode)
            playerID = peerIdx + 1;
        else
            playerID = 0; // from host

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
            playerID = 0;

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

bool NetplaySession::HostSendStates(int clientIdx)
{
    for (int i = 0; i < NumInstances; i++)
    {
        std::vector<u8> stateData;
        if (!TakeState(i, stateData))
        {
            Log(LogLevel::Error, "Netplay: failed to take state for instance %d\n", i);
            return false;
        }

        NetplayBlobType blobType = (NetplayBlobType)(Blob_Savestate0 + i);
        BlobTransfer::Send(Transport, clientIdx, blobType, stateData.data(), (u32)stateData.size());
    }

    // Also send SRAM for instance 0
    const u8* sram = Instances[0]->GetNDSSave();
    u32 sramLen = Instances[0]->GetNDSSaveLength();
    if (sram && sramLen > 0)
    {
        BlobTransfer::Send(Transport, clientIdx, Blob_SRAM, sram, sramLen);
    }

    return true;
}

bool NetplaySession::ClientReceiveStates()
{
    // Wait for all instance states to be received
    // This is called in a polling loop
    for (int i = 0; i < NumInstances; i++)
    {
        int blobIdx = Blob_Savestate0 + i;
        if (blobIdx >= Blob_MAX) break;

        if (!BlobRecv[blobIdx].IsComplete())
            return false;
    }

    // All states received - apply them
    for (int i = 0; i < NumInstances; i++)
    {
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

void NetplaySession::MuteNonLocalInstances()
{
    for (int i = 0; i < NumInstances; i++)
    {
        if (i != LocalPlayerID && Instances[i])
        {
            // Set master volume to 0 to mute audio on non-local instances.
            // The SPU MasterVolume is applied in the audio mix path.
            // We write 0 to the SOUNDCNT register's volume field.
            Instances[i]->SPU.SetPowerCnt(0);
        }
    }
}

}
