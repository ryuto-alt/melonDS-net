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

#ifndef DEBUGBRIDGE_H
#define DEBUGBRIDGE_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "types.h"

class EmuInstance;

// 外部ツール(MCP サーバ等)から動作中のエミュを解析するための TCP ブリッジ。
//
// 受信スレッドが 1 行 1 リクエストで積み、【エミュスレッドが】フレーム境界で
// 実行する。コンソールの状態に触るのは常にエミュスレッドだけなので、
// RunFrame の途中で RAM を覗いて壊す、というレースが起きない。
//
// プロトコル(行指向 UTF-8、\n 区切り):
//   要求: "<id> <cmd> [args...]"
//   応答: "<id> ok <json>"  /  "<id> err <message>"
// json は必ず 1 行。バイナリは base64 で data フィールドに入れる。
class DebugBridge
{
public:
    explicit DebugBridge(EmuInstance* inst);
    ~DebugBridge();

    // 127.0.0.1 の basePort から空きを 8 個まで探して listen する
    bool start(melonDS::u16 basePort);
    void stop();

    bool isListening() const { return listening.load(); }
    melonDS::u16 getPort() const { return port; }

    // --- エミュスレッドから呼ぶ ---
    // inputProcess() の直後: ブリッジからの入力注入を inputMask に被せる
    void beforeFrame();
    // ループ末尾: 溜まったリクエストの実行と、フリーズ値の再書き込み
    void processPending();

private:
    struct Request
    {
        std::string line;
        std::string response;
        bool done = false;
    };
    using ReqPtr = std::shared_ptr<Request>;

    struct Freeze
    {
        int id;
        int bus;
        melonDS::u32 addr;
        int size;
        melonDS::u32 value;
    };

    void acceptLoop();
    void clientLoop(intptr_t sock);
    void closeSocket(intptr_t sock);

    // --- 以下すべてエミュスレッド上で実行される ---
    std::string execute(const std::string& line);
    std::string cmdStatus();
    std::string cmdRead(const std::vector<std::string>& a);
    std::string cmdWrite(const std::vector<std::string>& a);
    std::string cmdPoke(const std::vector<std::string>& a);
    std::string cmdDump(const std::vector<std::string>& a);
    std::string cmdRegs(const std::vector<std::string>& a);
    std::string cmdSearchNew(const std::vector<std::string>& a);
    std::string cmdSearchNext(const std::vector<std::string>& a);
    std::string cmdSearchList(const std::vector<std::string>& a);
    std::string cmdFreezeAdd(const std::vector<std::string>& a);
    std::string cmdFreezeDel(const std::vector<std::string>& a);
    std::string cmdFreezeList();
    std::string cmdKeys(const std::vector<std::string>& a);
    std::string cmdTouch(const std::vector<std::string>& a);
    std::string cmdScreenshot(const std::vector<std::string>& a);
    std::string cmdState(const std::vector<std::string>& a, bool save);

    melonDS::u32 busRead(int bus, melonDS::u32 addr, int size);
    void busWrite(int bus, melonDS::u32 addr, int size, melonDS::u32 val);
    melonDS::u32 mainRAMSize() const;
    melonDS::u32 readCandidate(melonDS::u32 off) const;
    void applyFreezes();

    EmuInstance* emuInstance;

    melonDS::u16 port = 0;
    std::atomic<bool> listening {false};
    std::atomic<bool> quitting {false};
    intptr_t listenSock = -1;
    std::thread acceptThread;

    std::mutex clientMtx;
    std::vector<intptr_t> clientSocks;
    std::vector<std::thread> clientThreads;

    std::mutex queueMtx;
    std::condition_variable queueCV;
    std::deque<ReqPtr> queue;

    // RAM サーチ状態(メインRAM オフセット基準)
    int searchSize = 4;
    bool searchSigned = false;
    bool searchFloat = false;
    std::vector<melonDS::u32> candidates;
    std::vector<melonDS::u32> prevValues;

    // フリーズ(毎フレーム書き戻し)
    std::mutex stateMtx;
    std::vector<Freeze> freezes;
    int nextFreezeID = 1;

    // 入力注入
    std::mutex inputMtx;
    melonDS::u32 holdMask = 0;   // 1 が立っているビット = 押しっぱなし
    int holdFrames = 0;          // -1 = 無期限
    bool touchHeld = false;
    int touchFrames = 0;
    melonDS::u16 touchPosX = 0, touchPosY = 0;
};

#endif // DEBUGBRIDGE_H
