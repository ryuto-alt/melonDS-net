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

#include "DebugBridge.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <sstream>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    typedef int socklen_t;
    #define BRIDGE_INVALID_SOCK (intptr_t)INVALID_SOCKET
    #define BRIDGE_CLOSE(s) closesocket((SOCKET)(s))
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <errno.h>
    #define BRIDGE_INVALID_SOCK (intptr_t)-1
    #define BRIDGE_CLOSE(s) ::close((int)(s))
#endif

#include <QImage>
#include <QString>

#include "types.h"
#include "NDS.h"
#include "DSi.h"
#include "GPU.h"
#include "NDSCart.h"
#include "SPI.h"
#include "Platform.h"
#include "PlatformOGL.h"
#include "version.h"

#include "EmuInstance.h"
#include "EmuThread.h"

using namespace melonDS;

namespace
{

constexpr int kMaxInlineRead = 256 * 1024;   // これを超えるなら dump を使わせる
constexpr int kRequestTimeoutMs = 8000;

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const u8* data, size_t len)
{
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3)
    {
        u32 v = (u32)data[i] << 16;
        if (i + 1 < len) v |= (u32)data[i+1] << 8;
        if (i + 2 < len) v |= (u32)data[i+2];

        out += kB64[(v >> 18) & 0x3F];
        out += kB64[(v >> 12) & 0x3F];
        out += (i + 1 < len) ? kB64[(v >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? kB64[v & 0x3F] : '=';
    }
    return out;
}

bool base64Decode(const std::string& in, std::vector<u8>& out)
{
    int tbl[256];
    for (int i = 0; i < 256; i++) tbl[i] = -1;
    for (int i = 0; i < 64; i++) tbl[(u8)kB64[i]] = i;

    u32 acc = 0;
    int bits = 0;
    for (char c : in)
    {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
        int v = tbl[(u8)c];
        if (v < 0) return false;
        acc = (acc << 6) | (u32)v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back((u8)((acc >> bits) & 0xFF));
        }
    }
    return true;
}

std::string jsonEscape(const std::string& s)
{
    std::string out;
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else out += (char)c;
        }
    }
    return out;
}

std::vector<std::string> tokenize(const std::string& line)
{
    std::vector<std::string> out;
    std::istringstream ss(line);
    std::string tok;
    while (ss >> tok) out.push_back(tok);
    return out;
}

// "0x02000000" も "02000000" も 10 進も受ける
bool parseU32(const std::string& s, u32& out)
{
    if (s.empty()) return false;
    char* end = nullptr;
    unsigned long long v;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        v = strtoull(s.c_str() + 2, &end, 16);
    else
        v = strtoull(s.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    out = (u32)v;
    return true;
}

bool parseS64(const std::string& s, s64& out)
{
    if (s.empty()) return false;
    char* end = nullptr;
    long long v;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        v = (long long)strtoull(s.c_str() + 2, &end, 16);
    else
        v = strtoll(s.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    out = v;
    return true;
}

int parseBus(const std::string& s)
{
    if (s == "main") return 0;
    if (s == "arm9") return 1;
    if (s == "arm7") return 2;
    return -1;
}

std::string errJson(const std::string& msg)
{
    return std::string("\x01") + msg;   // 先頭 \x01 = エラーマーカー(内部用)
}

bool isErr(const std::string& s) { return !s.empty() && s[0] == '\x01'; }

}   // namespace


DebugBridge::DebugBridge(EmuInstance* inst) : emuInstance(inst)
{
}

DebugBridge::~DebugBridge()
{
    stop();
}

bool DebugBridge::start(u16 basePort)
{
    if (listening.load()) return true;

#ifdef _WIN32
    {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);   // enet が既に呼んでいても参照カウントされるだけ
    }
#endif

    for (u16 p = basePort; p < basePort + 8; p++)
    {
        intptr_t s = (intptr_t)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == BRIDGE_INVALID_SOCK) continue;

        int one = 1;
        setsockopt((int)s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(p);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // ローカル専用

        if (bind((int)s, (sockaddr*)&addr, sizeof(addr)) != 0 || listen((int)s, 4) != 0)
        {
            BRIDGE_CLOSE(s);
            continue;
        }

        listenSock = s;
        port = p;
        quitting.store(false);
        listening.store(true);
        acceptThread = std::thread(&DebugBridge::acceptLoop, this);
        Platform::Log(Platform::LogLevel::Info, "DebugBridge: listening on 127.0.0.1:%d\n", (int)p);
        return true;
    }

    Platform::Log(Platform::LogLevel::Warn, "DebugBridge: no free port from %d\n", (int)basePort);
    return false;
}

void DebugBridge::stop()
{
    if (!listening.load() && !quitting.load()) return;

    quitting.store(true);
    listening.store(false);

    if (listenSock != BRIDGE_INVALID_SOCK)
    {
        BRIDGE_CLOSE(listenSock);
        listenSock = BRIDGE_INVALID_SOCK;
    }

    {
        std::lock_guard<std::mutex> lk(clientMtx);
        for (intptr_t s : clientSocks)
            BRIDGE_CLOSE(s);
        clientSocks.clear();
    }

    // 待たされているリクエストを叩き起こす
    {
        std::lock_guard<std::mutex> lk(queueMtx);
        for (auto& r : queue)
        {
            r->response = "err bridge shutting down";
            r->done = true;
        }
        queue.clear();
    }
    queueCV.notify_all();

    if (acceptThread.joinable()) acceptThread.join();

    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lk(clientMtx);
        threads.swap(clientThreads);
    }
    for (auto& t : threads)
        if (t.joinable()) t.join();
}

void DebugBridge::closeSocket(intptr_t sock)
{
    std::lock_guard<std::mutex> lk(clientMtx);
    for (auto it = clientSocks.begin(); it != clientSocks.end(); ++it)
    {
        if (*it == sock)
        {
            clientSocks.erase(it);
            break;
        }
    }
    BRIDGE_CLOSE(sock);
}

void DebugBridge::acceptLoop()
{
    while (!quitting.load())
    {
        sockaddr_in caddr {};
        socklen_t clen = sizeof(caddr);
        intptr_t cs = (intptr_t)accept((int)listenSock, (sockaddr*)&caddr, &clen);
        if (cs == BRIDGE_INVALID_SOCK)
        {
            if (quitting.load()) break;
            continue;
        }

        int one = 1;
        setsockopt((int)cs, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));

        std::lock_guard<std::mutex> lk(clientMtx);
        clientSocks.push_back(cs);
        clientThreads.emplace_back(&DebugBridge::clientLoop, this, cs);
    }
}

void DebugBridge::clientLoop(intptr_t sock)
{
    std::string buf;
    char chunk[16384];

    while (!quitting.load())
    {
        int n = (int)recv((int)sock, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        buf.append(chunk, n);

        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos)
        {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            auto req = std::make_shared<Request>();
            req->line = line;

            {
                std::lock_guard<std::mutex> lk(queueMtx);
                queue.push_back(req);
            }

            std::string resp;
            {
                std::unique_lock<std::mutex> lk(queueMtx);
                bool ok = queueCV.wait_for(lk, std::chrono::milliseconds(kRequestTimeoutMs),
                                           [&] { return req->done || quitting.load(); });
                if (!ok || !req->done)
                    resp = "err timeout (エミュスレッドが応答しない: ウィンドウが固まっていないか確認)";
                else
                    resp = req->response;
            }

            resp += "\n";
            size_t sent = 0;
            while (sent < resp.size())
            {
                int w = (int)send((int)sock, resp.data() + sent, (int)(resp.size() - sent), 0);
                if (w <= 0) { sent = resp.size(); break; }
                sent += (size_t)w;
            }
        }
    }

    closeSocket(sock);
}


// ============================================================
//  ここから下はすべてエミュスレッド上
// ============================================================

void DebugBridge::beforeFrame()
{
    if (!emuInstance) return;

    std::lock_guard<std::mutex> lk(inputMtx);

    if (holdFrames != 0 && holdMask != 0)
    {
        emuInstance->inputMask &= ~holdMask;   // 0 = 押下
        if (holdFrames > 0) holdFrames--;
    }
    else if (holdFrames == 0)
    {
        holdMask = 0;
    }

    if (touchFrames != 0 && touchHeld)
    {
        emuInstance->isTouching = true;
        emuInstance->touchX = touchPosX;
        emuInstance->touchY = touchPosY;
        if (touchFrames > 0) touchFrames--;
    }
    else if (touchFrames == 0 && touchHeld)
    {
        touchHeld = false;
    }
}

void DebugBridge::processPending()
{
    applyFreezes();

    for (;;)
    {
        ReqPtr req;
        {
            std::lock_guard<std::mutex> lk(queueMtx);
            if (queue.empty()) break;
            req = queue.front();
            queue.pop_front();
        }

        std::string id = "0";
        std::string body;
        {
            size_t sp = req->line.find(' ');
            if (sp == std::string::npos)
            {
                id = req->line;
                body.clear();
            }
            else
            {
                id = req->line.substr(0, sp);
                body = req->line.substr(sp + 1);
            }
        }

        std::string result = execute(body);

        {
            std::lock_guard<std::mutex> lk(queueMtx);
            if (isErr(result))
                req->response = id + " err " + result.substr(1);
            else
                req->response = id + " ok " + result;
            req->done = true;
        }
        queueCV.notify_all();
    }
}

void DebugBridge::applyFreezes()
{
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds) return;

    std::lock_guard<std::mutex> lk(stateMtx);
    for (const auto& f : freezes)
        busWrite(f.bus, f.addr, f.size, f.value);
}

u32 DebugBridge::mainRAMSize() const
{
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds) return 0;
    return nds->MainRAMMask + 1;
}

u32 DebugBridge::busRead(int bus, u32 addr, int size)
{
    NDS* nds = emuInstance->getNDS();
    if (!nds) return 0;

    if (bus == 0)
    {
        u32 off = (addr - 0x02000000) & nds->MainRAMMask;
        u32 v = 0;
        for (int i = 0; i < size; i++)
            v |= (u32)nds->MainRAM[(off + i) & nds->MainRAMMask] << (i * 8);
        return v;
    }

    if (bus == 1)
    {
        if (size == 1) return nds->ARM9Read8(addr);
        if (size == 2) return nds->ARM9Read16(addr);
        return nds->ARM9Read32(addr);
    }

    if (size == 1) return nds->ARM7Read8(addr);
    if (size == 2) return nds->ARM7Read16(addr);
    return nds->ARM7Read32(addr);
}

void DebugBridge::busWrite(int bus, u32 addr, int size, u32 val)
{
    NDS* nds = emuInstance->getNDS();
    if (!nds) return;

    if (bus == 0)
    {
        u32 off = (addr - 0x02000000) & nds->MainRAMMask;
        for (int i = 0; i < size; i++)
            nds->MainRAM[(off + i) & nds->MainRAMMask] = (u8)((val >> (i * 8)) & 0xFF);
        return;
    }

    if (bus == 1)
    {
        if (size == 1) nds->ARM9Write8(addr, (u8)val);
        else if (size == 2) nds->ARM9Write16(addr, (u16)val);
        else nds->ARM9Write32(addr, val);
        return;
    }

    if (size == 1) nds->ARM7Write8(addr, (u8)val);
    else if (size == 2) nds->ARM7Write16(addr, (u16)val);
    else nds->ARM7Write32(addr, val);
}

u32 DebugBridge::readCandidate(u32 off) const
{
    NDS* nds = emuInstance->getNDS();
    u32 mask = nds->MainRAMMask;
    u32 v = 0;
    for (int i = 0; i < searchSize; i++)
        v |= (u32)nds->MainRAM[(off + i) & mask] << (i * 8);
    return v;
}


std::string DebugBridge::execute(const std::string& line)
{
    auto a = tokenize(line);
    if (a.empty()) return errJson("empty command");

    const std::string& cmd = a[0];

    if (cmd == "ping")
    {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"emu\":\"RyuE\",\"version\":\"%s\",\"protocol\":1,\"instance\":%d}",
                 MELONDS_VERSION, emuInstance ? emuInstance->getInstanceID() : -1);
        return buf;
    }

    if (cmd == "status")      return cmdStatus();
    if (cmd == "read")        return cmdRead(a);
    if (cmd == "write")       return cmdWrite(a);
    if (cmd == "poke")        return cmdPoke(a);
    if (cmd == "dump")        return cmdDump(a);
    if (cmd == "regs")        return cmdRegs(a);
    if (cmd == "search_new")  return cmdSearchNew(a);
    if (cmd == "search_next") return cmdSearchNext(a);
    if (cmd == "search_list") return cmdSearchList(a);
    if (cmd == "search_reset")
    {
        candidates.clear();
        prevValues.clear();
        return "{\"count\":0}";
    }
    if (cmd == "freeze_add")  return cmdFreezeAdd(a);
    if (cmd == "freeze_del")  return cmdFreezeDel(a);
    if (cmd == "freeze_list") return cmdFreezeList();
    if (cmd == "freeze_clear")
    {
        std::lock_guard<std::mutex> lk(stateMtx);
        freezes.clear();
        return "{}";
    }
    if (cmd == "keys")        return cmdKeys(a);
    if (cmd == "touch")       return cmdTouch(a);
    if (cmd == "screenshot")  return cmdScreenshot(a);
    if (cmd == "savestate")   return cmdState(a, true);
    if (cmd == "loadstate")   return cmdState(a, false);

    EmuThread* thread = emuInstance ? emuInstance->getEmuThread() : nullptr;
    if (!thread) return errJson("no emu thread");

    if (cmd == "pause")
    {
        thread->sendMessage(EmuThread::msg_EmuPause);
        return "{}";
    }
    if (cmd == "resume")
    {
        thread->sendMessage(EmuThread::msg_EmuUnpause);
        return "{}";
    }
    if (cmd == "reset")
    {
        thread->sendMessage(EmuThread::msg_EmuReset);
        return "{}";
    }
    if (cmd == "step")
    {
        u32 n = 1;
        if (a.size() >= 2 && !parseU32(a[1], n)) return errJson("bad frame count");
        if (n > 600) return errJson("step は 600 フレームまで");
        for (u32 i = 0; i < n; i++)
            thread->sendMessage(EmuThread::msg_EmuFrameStep);
        return "{}";
    }

    return errJson("unknown command: " + cmd);
}

std::string DebugBridge::cmdStatus()
{
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    EmuThread* thread = emuInstance ? emuInstance->getEmuThread() : nullptr;

    std::string title, gamecode, romPath;
    u32 romLen = 0;
    bool cart = false;

    if (nds)
    {
        auto* c = nds->NDSCartSlot.GetCart();
        if (c)
        {
            cart = true;
            const NDSHeader& h = c->GetHeader();
            title.assign(h.GameTitle, strnlen(h.GameTitle, 12));
            gamecode.assign(h.GameCode, 4);
            romLen = c->GetROMLength();
        }
    }

    if (emuInstance)
    {
        romPath = emuInstance->baseROMDir;
        if (!romPath.empty() && !emuInstance->baseROMName.empty())
        {
            if (romPath.back() != '/' && romPath.back() != '\\') romPath += '/';
            romPath += emuInstance->baseROMName;
        }
        else if (!emuInstance->baseROMName.empty())
        {
            romPath = emuInstance->baseROMName;
        }
    }

    bool running = thread && thread->emuIsRunning();
    bool active = thread && thread->emuIsActive();

    char buf[1024];
    snprintf(buf, sizeof(buf),
             "{\"running\":%s,\"active\":%s,\"cartInserted\":%s,\"frame\":%u,"
             "\"console\":\"%s\",\"mainRAMSize\":%u,\"title\":\"%s\",\"gameCode\":\"%s\","
             "\"romLength\":%u,\"romPath\":\"%s\",\"port\":%d}",
             running ? "true" : "false",
             active ? "true" : "false",
             cart ? "true" : "false",
             nds ? nds->NumFrames : 0,
             (nds && nds->ConsoleType == 1) ? "dsi" : "nds",
             nds ? (nds->MainRAMMask + 1) : 0,
             jsonEscape(title).c_str(),
             jsonEscape(gamecode).c_str(),
             romLen,
             jsonEscape(romPath).c_str(),
             (int)port);
    return buf;
}

std::string DebugBridge::cmdRead(const std::vector<std::string>& a)
{
    if (a.size() < 4) return errJson("usage: read <main|arm9|arm7> <addr> <len>");
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds) return errJson("コンソールが起動していない");

    int bus = parseBus(a[1]);
    u32 addr, len;
    if (bus < 0) return errJson("bad bus: " + a[1]);
    if (!parseU32(a[2], addr) || !parseU32(a[3], len)) return errJson("bad addr/len");
    if (len == 0 || len > kMaxInlineRead)
        return errJson("len は 1..262144 (それ以上は dump を使う)");

    std::vector<u8> data(len);
    if (bus == 0)
    {
        u32 off = (addr - 0x02000000) & nds->MainRAMMask;
        for (u32 i = 0; i < len; i++)
            data[i] = nds->MainRAM[(off + i) & nds->MainRAMMask];
    }
    else
    {
        for (u32 i = 0; i < len; i++)
            data[i] = (u8)busRead(bus, addr + i, 1);
    }

    return "{\"addr\":" + std::to_string(addr) + ",\"len\":" + std::to_string(len) +
           ",\"data\":\"" + base64Encode(data.data(), data.size()) + "\"}";
}

std::string DebugBridge::cmdWrite(const std::vector<std::string>& a)
{
    if (a.size() < 4) return errJson("usage: write <bus> <addr> <base64>");
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds) return errJson("コンソールが起動していない");

    int bus = parseBus(a[1]);
    u32 addr;
    if (bus < 0) return errJson("bad bus: " + a[1]);
    if (!parseU32(a[2], addr)) return errJson("bad addr");

    std::vector<u8> data;
    if (!base64Decode(a[3], data)) return errJson("bad base64");
    if (data.empty()) return errJson("empty payload");

    for (size_t i = 0; i < data.size(); i++)
        busWrite(bus, addr + (u32)i, 1, data[i]);

    return "{\"written\":" + std::to_string(data.size()) + "}";
}

std::string DebugBridge::cmdPoke(const std::vector<std::string>& a)
{
    if (a.size() < 5) return errJson("usage: poke <bus> <addr> <1|2|4> <value>");
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds) return errJson("コンソールが起動していない");

    int bus = parseBus(a[1]);
    u32 addr, size, val;
    if (bus < 0) return errJson("bad bus: " + a[1]);
    if (!parseU32(a[2], addr) || !parseU32(a[3], size)) return errJson("bad addr/size");
    if (size != 1 && size != 2 && size != 4) return errJson("size は 1/2/4");

    s64 sval;
    if (!parseS64(a[4], sval)) return errJson("bad value");
    val = (u32)sval;

    busWrite(bus, addr, (int)size, val);
    return "{\"addr\":" + std::to_string(addr) + ",\"value\":" + std::to_string(val) + "}";
}

std::string DebugBridge::cmdDump(const std::vector<std::string>& a)
{
    if (a.size() < 5) return errJson("usage: dump <bus> <addr> <len> <path>");
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds) return errJson("コンソールが起動していない");

    int bus = parseBus(a[1]);
    u32 addr, len;
    if (bus < 0) return errJson("bad bus: " + a[1]);
    if (!parseU32(a[2], addr) || !parseU32(a[3], len)) return errJson("bad addr/len");
    if (len == 0 || len > 64u * 1024 * 1024) return errJson("len が大きすぎる");

    // パスは行末まで(空白を含みうる)
    size_t p = a[0].size() + 1 + a[1].size() + 1 + a[2].size() + 1 + a[3].size() + 1;
    std::string path;
    for (size_t i = 4; i < a.size(); i++)
    {
        if (i > 4) path += ' ';
        path += a[i];
    }
    (void)p;

    std::vector<u8> data(len);
    if (bus == 0)
    {
        u32 off = (addr - 0x02000000) & nds->MainRAMMask;
        for (u32 i = 0; i < len; i++)
            data[i] = nds->MainRAM[(off + i) & nds->MainRAMMask];
    }
    else
    {
        for (u32 i = 0; i < len; i++)
            data[i] = (u8)busRead(bus, addr + i, 1);
    }

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return errJson("ファイルを開けない: " + path);
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);

    return "{\"path\":\"" + jsonEscape(path) + "\",\"len\":" + std::to_string(len) + "}";
}

std::string DebugBridge::cmdRegs(const std::vector<std::string>& a)
{
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds) return errJson("コンソールが起動していない");

    bool arm7 = (a.size() >= 2 && a[1] == "arm7");
    const u32* R = arm7 ? nds->ARM7.R : nds->ARM9.R;
    u32 cpsr = arm7 ? nds->ARM7.CPSR : nds->ARM9.CPSR;

    std::string out = "{\"cpu\":\"";
    out += arm7 ? "arm7" : "arm9";
    out += "\",\"r\":[";
    for (int i = 0; i < 16; i++)
    {
        if (i) out += ",";
        out += std::to_string(R[i]);
    }
    out += "],\"cpsr\":" + std::to_string(cpsr);
    out += ",\"thumb\":" + std::string((cpsr & 0x20) ? "true" : "false");
    out += ",\"pc\":" + std::to_string(R[15]);
    out += "}";
    return out;
}

std::string DebugBridge::cmdSearchNew(const std::vector<std::string>& a)
{
    if (a.size() < 3)
        return errJson("usage: search_new <u8|s8|u16|s16|u32|s32|f32> <eq|ne|gt|lt|ge|le|unknown> [value]");
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds) return errJson("コンソールが起動していない");

    const std::string& type = a[1];
    if (type == "u8")       { searchSize = 1; searchSigned = false; searchFloat = false; }
    else if (type == "s8")  { searchSize = 1; searchSigned = true;  searchFloat = false; }
    else if (type == "u16") { searchSize = 2; searchSigned = false; searchFloat = false; }
    else if (type == "s16") { searchSize = 2; searchSigned = true;  searchFloat = false; }
    else if (type == "u32") { searchSize = 4; searchSigned = false; searchFloat = false; }
    else if (type == "s32") { searchSize = 4; searchSigned = true;  searchFloat = false; }
    else if (type == "f32") { searchSize = 4; searchSigned = true;  searchFloat = true;  }
    else return errJson("bad type: " + type);

    const std::string& op = a[2];
    bool unknown = (op == "unknown");
    s64 want = 0;
    float fwant = 0.0f;
    if (!unknown)
    {
        if (a.size() < 4) return errJson("value がない");
        if (searchFloat) fwant = strtof(a[3].c_str(), nullptr);
        else if (!parseS64(a[3], want)) return errJson("bad value");
    }

    u32 size = mainRAMSize();
    u32 step = (u32)searchSize;

    candidates.clear();
    prevValues.clear();
    candidates.reserve(unknown ? (size / step) : 4096);
    prevValues.reserve(unknown ? (size / step) : 4096);

    for (u32 off = 0; off + step <= size; off += step)
    {
        u32 raw = readCandidate(off);
        bool hit;

        if (unknown) hit = true;
        else if (searchFloat)
        {
            float v;
            memcpy(&v, &raw, 4);
            if (!std::isfinite(v)) { hit = false; }
            else if (op == "eq") hit = std::fabs(v - fwant) < 0.001f;
            else if (op == "ne") hit = std::fabs(v - fwant) >= 0.001f;
            else if (op == "gt") hit = v > fwant;
            else if (op == "lt") hit = v < fwant;
            else if (op == "ge") hit = v >= fwant;
            else if (op == "le") hit = v <= fwant;
            else return errJson("bad op: " + op);
        }
        else
        {
            s64 v;
            if (searchSigned)
            {
                if (searchSize == 1) v = (s8)raw;
                else if (searchSize == 2) v = (s16)raw;
                else v = (s32)raw;
            }
            else v = (s64)raw;

            if (op == "eq") hit = (v == want);
            else if (op == "ne") hit = (v != want);
            else if (op == "gt") hit = (v > want);
            else if (op == "lt") hit = (v < want);
            else if (op == "ge") hit = (v >= want);
            else if (op == "le") hit = (v <= want);
            else return errJson("bad op: " + op);
        }

        if (hit)
        {
            candidates.push_back(off);
            prevValues.push_back(raw);
        }
    }

    return "{\"count\":" + std::to_string(candidates.size()) + "}";
}

std::string DebugBridge::cmdSearchNext(const std::vector<std::string>& a)
{
    if (a.size() < 2)
        return errJson("usage: search_next <eq|ne|gt|lt|ge|le|changed|unchanged|inc|dec|delta> [value]");
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds) return errJson("コンソールが起動していない");
    if (candidates.empty()) return errJson("候補がない。先に search_new を実行");

    const std::string& op = a[1];
    bool needValue = (op == "eq" || op == "ne" || op == "gt" || op == "lt" ||
                      op == "ge" || op == "le" || op == "delta");
    s64 want = 0;
    float fwant = 0.0f;
    if (needValue)
    {
        if (a.size() < 3) return errJson("value がない");
        if (searchFloat) fwant = strtof(a[2].c_str(), nullptr);
        else if (!parseS64(a[2], want)) return errJson("bad value");
    }

    std::vector<u32> newCand;
    std::vector<u32> newPrev;
    newCand.reserve(candidates.size());
    newPrev.reserve(candidates.size());

    auto toSigned = [&](u32 raw) -> s64
    {
        if (!searchSigned) return (s64)raw;
        if (searchSize == 1) return (s8)raw;
        if (searchSize == 2) return (s16)raw;
        return (s32)raw;
    };

    for (size_t i = 0; i < candidates.size(); i++)
    {
        u32 off = candidates[i];
        u32 raw = readCandidate(off);
        u32 praw = prevValues[i];
        bool hit;

        if (searchFloat)
        {
            float v, pv;
            memcpy(&v, &raw, 4);
            memcpy(&pv, &praw, 4);
            if (op == "eq") hit = std::fabs(v - fwant) < 0.001f;
            else if (op == "ne") hit = std::fabs(v - fwant) >= 0.001f;
            else if (op == "gt") hit = v > fwant;
            else if (op == "lt") hit = v < fwant;
            else if (op == "ge") hit = v >= fwant;
            else if (op == "le") hit = v <= fwant;
            else if (op == "changed") hit = (raw != praw);
            else if (op == "unchanged") hit = (raw == praw);
            else if (op == "inc") hit = (v > pv);
            else if (op == "dec") hit = (v < pv);
            else if (op == "delta") hit = std::fabs((v - pv) - fwant) < 0.001f;
            else return errJson("bad op: " + op);
        }
        else
        {
            s64 v = toSigned(raw);
            s64 pv = toSigned(praw);

            if (op == "eq") hit = (v == want);
            else if (op == "ne") hit = (v != want);
            else if (op == "gt") hit = (v > want);
            else if (op == "lt") hit = (v < want);
            else if (op == "ge") hit = (v >= want);
            else if (op == "le") hit = (v <= want);
            else if (op == "changed") hit = (raw != praw);
            else if (op == "unchanged") hit = (raw == praw);
            else if (op == "inc") hit = (v > pv);
            else if (op == "dec") hit = (v < pv);
            else if (op == "delta") hit = ((v - pv) == want);
            else return errJson("bad op: " + op);
        }

        if (hit)
        {
            newCand.push_back(off);
            newPrev.push_back(raw);
        }
    }

    candidates.swap(newCand);
    prevValues.swap(newPrev);
    return "{\"count\":" + std::to_string(candidates.size()) + "}";
}

std::string DebugBridge::cmdSearchList(const std::vector<std::string>& a)
{
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds) return errJson("コンソールが起動していない");

    u32 maxn = 64;
    if (a.size() >= 2 && !parseU32(a[1], maxn)) return errJson("bad max");
    if (maxn > 2000) maxn = 2000;

    std::string out = "{\"count\":" + std::to_string(candidates.size()) + ",\"results\":[";
    u32 n = (u32)std::min<size_t>(candidates.size(), maxn);
    for (u32 i = 0; i < n; i++)
    {
        if (i) out += ",";
        u32 off = candidates[i];
        u32 raw = readCandidate(off);
        out += "{\"addr\":" + std::to_string(0x02000000u + off) +
               ",\"value\":" + std::to_string(raw) +
               ",\"prev\":" + std::to_string(prevValues[i]) + "}";
    }
    out += "],\"size\":" + std::to_string(searchSize);
    out += ",\"float\":" + std::string(searchFloat ? "true" : "false");
    out += ",\"signed\":" + std::string(searchSigned ? "true" : "false");
    out += "}";
    return out;
}

std::string DebugBridge::cmdFreezeAdd(const std::vector<std::string>& a)
{
    if (a.size() < 5) return errJson("usage: freeze_add <bus> <addr> <1|2|4> <value>");
    int bus = parseBus(a[1]);
    u32 addr, size;
    s64 val;
    if (bus < 0) return errJson("bad bus: " + a[1]);
    if (!parseU32(a[2], addr) || !parseU32(a[3], size)) return errJson("bad addr/size");
    if (size != 1 && size != 2 && size != 4) return errJson("size は 1/2/4");
    if (!parseS64(a[4], val)) return errJson("bad value");

    std::lock_guard<std::mutex> lk(stateMtx);
    if (freezes.size() >= 256) return errJson("フリーズ登録が多すぎる");
    Freeze f { nextFreezeID++, bus, addr, (int)size, (u32)val };
    freezes.push_back(f);
    return "{\"id\":" + std::to_string(f.id) + "}";
}

std::string DebugBridge::cmdFreezeDel(const std::vector<std::string>& a)
{
    if (a.size() < 2) return errJson("usage: freeze_del <id>");
    u32 id;
    if (!parseU32(a[1], id)) return errJson("bad id");

    std::lock_guard<std::mutex> lk(stateMtx);
    for (auto it = freezes.begin(); it != freezes.end(); ++it)
    {
        if (it->id == (int)id)
        {
            freezes.erase(it);
            return "{\"removed\":" + std::to_string(id) + "}";
        }
    }
    return errJson("そんな id はない");
}

std::string DebugBridge::cmdFreezeList()
{
    std::lock_guard<std::mutex> lk(stateMtx);
    std::string out = "{\"freezes\":[";
    for (size_t i = 0; i < freezes.size(); i++)
    {
        const auto& f = freezes[i];
        if (i) out += ",";
        out += "{\"id\":" + std::to_string(f.id) +
               ",\"bus\":\"" + (f.bus == 0 ? "main" : (f.bus == 1 ? "arm9" : "arm7")) + "\"" +
               ",\"addr\":" + std::to_string(f.addr) +
               ",\"size\":" + std::to_string(f.size) +
               ",\"value\":" + std::to_string(f.value) + "}";
    }
    out += "]}";
    return out;
}

std::string DebugBridge::cmdKeys(const std::vector<std::string>& a)
{
    if (a.size() < 2) return errJson("usage: keys <mask> [frames]");
    u32 mask;
    if (!parseU32(a[1], mask)) return errJson("bad mask");
    mask &= 0xFFF;

    s64 frames = 1;
    if (a.size() >= 3 && !parseS64(a[2], frames)) return errJson("bad frames");
    if (frames > 3600) return errJson("frames は 3600 まで");

    std::lock_guard<std::mutex> lk(inputMtx);
    holdMask = mask;
    holdFrames = (mask == 0) ? 0 : (int)frames;
    return "{\"mask\":" + std::to_string(mask) + ",\"frames\":" + std::to_string(holdFrames) + "}";
}

std::string DebugBridge::cmdTouch(const std::vector<std::string>& a)
{
    if (a.size() >= 2 && a[1] == "up")
    {
        std::lock_guard<std::mutex> lk(inputMtx);
        touchHeld = false;
        touchFrames = 0;
        return "{\"touching\":false}";
    }

    if (a.size() < 3) return errJson("usage: touch <x> <y> [frames] | touch up");
    u32 x, y;
    if (!parseU32(a[1], x) || !parseU32(a[2], y)) return errJson("bad x/y");
    if (x > 255 || y > 191) return errJson("x は 0..255, y は 0..191");

    s64 frames = 1;
    if (a.size() >= 4 && !parseS64(a[3], frames)) return errJson("bad frames");
    if (frames > 3600) return errJson("frames は 3600 まで");

    std::lock_guard<std::mutex> lk(inputMtx);
    touchHeld = true;
    touchFrames = (int)frames;
    touchPosX = (u16)x;
    touchPosY = (u16)y;
    return "{\"touching\":true,\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) +
           ",\"frames\":" + std::to_string(frames) + "}";
}

std::string DebugBridge::cmdScreenshot(const std::vector<std::string>& a)
{
    if (a.size() < 2) return errJson("usage: screenshot <path> [top|bottom|both]");
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds) return errJson("コンソールが起動していない");

    std::string path = a[1];
    std::string which = (a.size() >= 3) ? a[2] : "both";

    void* topbuf = nullptr;
    void* botbuf = nullptr;
    bool hasRAMBuffers = nds->GPU.GetFramebuffers(&topbuf, &botbuf);

    QImage top, bottom;

    if (hasRAMBuffers)
    {
        top = QImage((const uchar*)topbuf, 256, 192, QImage::Format_RGB32).copy();
        bottom = QImage((const uchar*)botbuf, 256, 192, QImage::Format_RGB32).copy();
    }
    else
    {
#ifdef OGLRENDERER_ENABLED
        // OpenGL レンダラ: 画面は配列テクスチャの中にある。GL コンテキストを持つのは
        // このエミュスレッドなので、ここで読み戻せる。
        if (!topbuf) return errJson("GL テクスチャが取れない");
        emuInstance->makeCurrentGL();

        GLuint tex = *(GLuint*)topbuf;
        glBindTexture(GL_TEXTURE_2D_ARRAY, tex);

        GLint w = 0, h = 0, d = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_WIDTH, &w);
        glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_HEIGHT, &h);
        glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_DEPTH, &d);
        if (w <= 0 || h <= 0 || d < 2)
            return errJson("GL テクスチャの寸法が読めない");

        std::vector<u8> px((size_t)w * h * d * 4);
        glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_BGRA, GL_UNSIGNED_BYTE, px.data());

        size_t layer = (size_t)w * h * 4;
        top = QImage(px.data(), w, h, QImage::Format_RGB32).copy();
        bottom = QImage(px.data() + layer, w, h, QImage::Format_RGB32).copy();
#else
        return errJson("このビルドは OpenGL レンダラの読み戻しに未対応");
#endif
    }

    QImage out;
    if (which == "top") out = top;
    else if (which == "bottom") out = bottom;
    else
    {
        out = QImage(top.width(), top.height() * 2, QImage::Format_RGB32);
        for (int y = 0; y < top.height(); y++)
            memcpy(out.scanLine(y), top.constScanLine(y), top.width() * 4);
        for (int y = 0; y < bottom.height(); y++)
            memcpy(out.scanLine(top.height() + y), bottom.constScanLine(y), bottom.width() * 4);
    }

    if (out.isNull()) return errJson("画面がまだ描かれていない");
    if (!out.save(QString::fromStdString(path), "PNG"))
        return errJson("PNG を書けない: " + path);

    return "{\"path\":\"" + jsonEscape(path) + "\",\"width\":" + std::to_string(out.width()) +
           ",\"height\":" + std::to_string(out.height()) + "}";
}

std::string DebugBridge::cmdState(const std::vector<std::string>& a, bool save)
{
    if (a.size() < 2) return errJson(save ? "usage: savestate <path>" : "usage: loadstate <path>");
    EmuThread* thread = emuInstance ? emuInstance->getEmuThread() : nullptr;
    if (!thread) return errJson("no emu thread");

    std::string path;
    for (size_t i = 1; i < a.size(); i++)
    {
        if (i > 1) path += ' ';
        path += a[i];
    }

    // saveState/loadState は【UI スレッドから】メッセージを投げる想定の API。
    // ここはエミュスレッドなので、EmuInstance の同期版を直接呼ぶ。
    bool ok = save ? emuInstance->saveState(path) : emuInstance->loadState(path);
    if (!ok) return errJson(save ? "セーブに失敗" : "ロードに失敗");

    return "{\"path\":\"" + jsonEscape(path) + "\"}";
}
