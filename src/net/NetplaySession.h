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

#ifndef NETPLAYSESSION_H
#define NETPLAYSESSION_H

#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <functional>

#include "types.h"
#include "NDS.h"
#include "NetplayProtocol.h"
#include "LocalMP.h"
#include "Savestate.h"

namespace melonDS
{

// Simple C++17 barrier implementation (std::barrier requires C++20)
class SimpleBarrier
{
public:
    explicit SimpleBarrier(int count) : Threshold(count), Count(count), Generation(0) {}

    void arrive_and_wait()
    {
        std::unique_lock<std::mutex> lock(Mtx);
        int gen = Generation;
        if (--Count == 0)
        {
            Generation++;
            Count = Threshold;
            Cv.notify_all();
        }
        else
        {
            Cv.wait(lock, [this, gen] { return gen != Generation; });
        }
    }

private:
    std::mutex Mtx;
    std::condition_variable Cv;
    int Threshold;
    int Count;
    int Generation;
};

// Userdata struct for Platform::MP_* callbacks during netplay.
// In normal mode, userdata is an EmuInstance*; in netplay mode
// we need to route MP calls to the correct LocalMP instance.
struct NetplayInstanceData
{
    static constexpr u32 kMagic = 0x4E504944; // "NPID"
    u32 Magic = kMagic;
    int InstID;
    void* Session; // NetplaySession*
    void* OrigUserdata; // EmuInstance* for non-MP Platform callbacks
};

class NetplaySession
{
public:
    NetplaySession();
    ~NetplaySession();

    // ---- Setup ----

    // Initialize as host or client
    // localPlayerID: which player this PC controls (0 for host)
    // numPlayers: total player count (2-4)
    bool Init(int localPlayerID, int numPlayers, int inputDelay = 4);
    void DeInit();

    // ROM must be loaded before starting.
    // Creates NDS instances internally using the provided args builder.
    // Takes the mirror instance index and the firmware image every machine in
    // the session must share (empty = build a default one). Each instance still
    // needs its own MAC address.
    using NDSArgsBuilder = std::function<NDSArgs(int instIdx, const std::vector<u8>& firmware)>;
    bool CreateInstances(const NDSArgsBuilder& argsBuilder, void* origUserdata = nullptr);

    // Load ROM into all instances
    bool LoadROM(std::unique_ptr<NDSCart::CartCommon> cart);

    // Give the session its own copy of the ROM. In download-play mode only
    // player 0's instance gets the cart; the others boot the firmware menu and
    // pull the game off it over the emulated wireless, exactly like real DSes.
    bool LoadROMData(const u8* romdata, u32 romlen);
    u64 GetROMHash() const { return ROMHash; }

    // Host only: the cart and firmware bytes handed to joining players.
    void SetSharedData(const u8* firmware, u32 fwlen);
    void SetDownloadPlay(bool enable) { DownloadPlay = enable; }
    bool IsDownloadPlay() const { return DownloadPlay; }

    // A joining player has no ROM of its own: instances are built once the
    // host's cart and firmware have arrived.
    bool HasInstances() const { return NumInstances > 0 && Instances[0] != nullptr; }
    void SetArgsBuilder(const NDSArgsBuilder& builder, void* origUserdata)
    {
        ArgsBuilder = builder;
        OrigUserdata = origUserdata;
    }

    // ---- Session lifecycle ----
    // Idle    -- transport up, no peer synced yet
    // Syncing -- handshake / savestate transfer in flight, do NOT run frames
    // Running -- every instance holds identical state, frames may advance
    enum SyncStage { Stage_Idle = 0, Stage_Syncing, Stage_Running };
    int GetStage() const { return Stage.load(); }
    bool IsRunning() const { return Stage.load() == Stage_Running; }

    // State transfer (for client joining)
    bool TakeState(int inst, std::vector<u8>& out);
    bool LoadState(int inst, const void* data, u32 len);

    // Take/load savestate for all instances
    bool TakeAllStates(std::vector<std::vector<u8>>& out);
    bool LoadAllStates(const std::vector<std::vector<u8>>& states);

    // ---- Frame execution ----

    // Set input for local player at current frame + inputDelay
    void SetLocalInput(const InputFrame& input);

    // Set input received from remote player
    void SetRemoteInput(int playerID, const InputFrame& input);

    // Check if all inputs are available for the given frame
    bool ReadyForFrame(u32 frameNum) const;

    // Run one frame on all instances in parallel
    // Returns number of scanlines rendered (from display instance)
    u32 RunFrame();

    // ---- Display ----

    // Get the NDS instance that should be displayed (local player's instance)
    NDS* GetDisplayInstance() const;

    // Get all instances (for state operations)
    NDS* GetInstance(int idx) const;
    int GetNumInstances() const { return NumInstances; }

    u32 GetFrameNum() const { return CurrentFrame; }
    int GetLocalPlayerID() const { return LocalPlayerID; }
    int GetInputDelay() const { return InputDelay; }
    void SetInputDelay(int delay) { InputDelay = delay; }

    // ---- Desync detection ----

    // Combined hash for a frame, built from what each console published
    // between its own frames. 0 until all of them have reached that frame.
    u64 ComputeStateHash(u32 frame) const;

    // ---- Network ----

    // Host: start listening for clients
    bool HostStart(int port = kNetplayDefaultPort);

    // Client: connect to host
    bool ClientConnect(const char* host, int port = kNetplayDefaultPort);

    // Process network events (call every frame)
    void ProcessNetwork();

    // Send local input to all remote players
    void SendLocalInput(const InputFrame& input);

    // ---- State sync (host -> client) ----

    // Host: send all instance states to a specific client
    bool HostSendStates(int clientIdx);

    // Client: receive and apply states from host
    bool ClientReceiveStates();

    // ---- Session state ----

    bool IsActive() const { return Active.load(); }
    bool IsHost() const { return HostMode; }

    // Global accessor for Platform callbacks
    static NetplaySession* Current;
    static bool IsNetplayActive() { return Current != nullptr && Current->IsActive(); }

    // Callbacks for UI
    using DesyncCallback = std::function<void(u32 frame, u64 localHash, u64 remoteHash)>;
    using DisconnectCallback = std::function<void(int playerID, NetplayDisconnectReason reason)>;
    void SetDesyncCallback(const DesyncCallback& cb) { OnDesync = cb; }
    void SetDisconnectCallback(const DisconnectCallback& cb) { OnDisconnect = cb; }

    // Access LocalMP for Platform.cpp routing
    LocalMP& GetLocalMP() { return LMP; }

private:
    // ---- Instances ----
    LocalMP LMP;                        // shared among all instances
    NDS* Instances[kNetplayMaxPlayers] = {};
    NetplayInstanceData InstData[kNetplayMaxPlayers] = {};
    int NumInstances = 0;
    int LocalPlayerID = 0;
    std::atomic<bool> Active{false};
    bool HostMode = false;

    // ---- Input buffer (ring buffer) ----
    static constexpr int INPUT_BUF_SIZE = 256;
    InputFrame InputBuf[kNetplayMaxPlayers][INPUT_BUF_SIZE] = {};
    bool InputReady[kNetplayMaxPlayers][INPUT_BUF_SIZE] = {};
    mutable std::mutex InputMutex;
    int InputDelay = 4;

    // ---- Frame state ----
    u32 CurrentFrame = 0;
    u32 StartFrame = 0;
    u32 LastFrameMS = 0;      // wall time the last frame took on all instances
    u32 LastMPTraffic = 0;    // LocalMP packet count at the previous report

    // ---- Threading ----
    // Each instance runs in its own thread during RunFrame().
    // LocalMP semaphores handle CMD/REPLY sync between instances.
    // A barrier synchronizes frame boundaries.
    std::thread InstanceThreads[kNetplayMaxPlayers];
    std::atomic<u32> InstanceFrame[kNetplayMaxPlayers] = {};  // each console's own frame counter
    u32 InstanceScanlines[kNetplayMaxPlayers] = {};
    std::atomic<bool> ThreadsRunning{false};

    void InstanceThreadFunc(int instIdx);
    void StartThreads();
    void StopThreads();

    // ---- Desync detection ----
    static constexpr int DESYNC_CHECK_INTERVAL = 60; // every 60 frames (1 sec)
    u64 LastStateHash = 0;
    u32 LastHashFrame = 0;
    std::atomic<u64> InstanceHash[kNetplayMaxPlayers] = {};
    std::atomic<u32> InstanceHashFrame[kNetplayMaxPlayers] = {};
    u64 HashInstance(int instIdx) const;

    // ---- Network ----
    NetplayTransport Transport;
    BlobTransfer BlobRecv[Blob_MAX];

    std::atomic<int> Stage{Stage_Idle};
    std::vector<u8> ROMData;
    std::vector<u8> FirmwareData;   // shared by every machine, or empty for generated
    u64 ROMHash = 0;
    bool DownloadPlay = true;
    int OfferedPlayers = 2;     // player count the host announced
    int PendingSyncPeer = -1;   // host: peer waiting to be sent the session state
    int CurBlobIdx = -1;        // which BlobRecv the in-flight chunks belong to

    // Kept so the client can rebuild its instances once the host tells it how
    // many players the session actually has.
    NDSArgsBuilder ArgsBuilder;
    void* OrigUserdata = nullptr;
    bool RebuildInstances(int numPlayers);
    void DestroyInstances();
    void HostSyncPeer(int peerIdx);

    void HandleControlMessage(int peerIdx, const u8* data, u32 len);
    void HandleInputMessage(int peerIdx, const u8* data, u32 len);

    // ---- Callbacks ----
    DesyncCallback OnDesync;
    DisconnectCallback OnDisconnect;

    // Apply buffered inputs to all instances for the given frame
    void ApplyInput(int instIdx, u32 frame);

    // Mute audio on non-display instances
    void MuteNonLocalInstances();
};

}

#endif // NETPLAYSESSION_H
