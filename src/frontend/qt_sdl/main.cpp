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

#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

#include <optional>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#endif

#include <QApplication>
#include <QStyle>
#include <QMessageBox>
#include <QMenuBar>
#include <QFileDialog>
#include <QInputDialog>
#include <QPainter>
#include <QKeyEvent>
#include <QMimeData>
#include <QVector>
#include <QCommandLineParser>
#include <QStandardPaths>
#ifndef _WIN32
#include <QGuiApplication>
#include <QSocketNotifier>
#include <unistd.h>
#include <sys/socket.h>
#include <signal.h>
#endif

#include <SDL2/SDL.h>

#include "OpenGLSupport.h"
#include "duckstation/gl/context.h"

#include "main.h"
#include "version.h"

#include "Config.h"

#include "EmuInstance.h"
#include "ArchiveUtil.h"
#include "CameraManager.h"
#include "MPInterface.h"
#include "Net.h"

#include "CLI.h"

#include "Net_PCap.h"
#include "Net_Slirp.h"

using namespace melonDS;

QString* systemThemeName;


QString emuDirectory;

const int kMaxEmuInstances = 16;
EmuInstance* emuInstances[kMaxEmuInstances];

CameraManager* camManager[2];
bool camStarted[2];

std::optional<LibPCap> pcap;
Net net;


QElapsedTimer sysTimer;


void NetInit()
{
    Config::Table cfg = Config::GetGlobalTable();
    if (cfg.GetBool("LAN.DirectMode"))
    {
        if (!pcap)
            pcap = LibPCap::New();

        if (pcap)
        {
            std::string devicename = cfg.GetString("LAN.Device");
            std::unique_ptr<Net_PCap> netPcap = pcap->Open(devicename, [](const u8* data, int len) {
                net.RXEnqueue(data, len);
            });

            if (netPcap)
            {
                net.SetDriver(std::move(netPcap));
            }
        }
    }
    else
    {
        net.SetDriver(std::make_unique<Net_Slirp>([](const u8* data, int len) {
            net.RXEnqueue(data, len);
        }));
    }
}


bool createEmuInstance()
{
    int id = -1;
    for (int i = 0; i < kMaxEmuInstances; i++)
    {
        if (!emuInstances[i])
        {
            id = i;
            break;
        }
    }

    if (id == -1)
        return false;

    auto inst = new EmuInstance(id);
    emuInstances[id] = inst;

    return true;
}

void deleteEmuInstance(int id)
{
    auto inst = emuInstances[id];
    if (!inst) return;

    delete inst;
    emuInstances[id] = nullptr;
}

void deleteAllEmuInstances(int first)
{
    for (int i = first; i < kMaxEmuInstances; i++)
        deleteEmuInstance(i);
}

int numEmuInstances()
{
    int ret = 0;

    for (int i = 0; i < kMaxEmuInstances; i++)
    {
        if (emuInstances[i])
            ret++;
    }

    return ret;
}


void broadcastInstanceCommand(int cmd, QVariant& param, int sourceinst)
{
    for (int i = 0; i < kMaxEmuInstances; i++)
    {
        if (i == sourceinst) continue;
        if (!emuInstances[i]) continue;

        emuInstances[i]->handleCommand(cmd, param);
    }
}


void pathInit()
{
    // First, check for the portable directory next to the executable.
    QString appdirpath = QCoreApplication::applicationDirPath();
    QString portablepath = appdirpath + QDir::separator() + "portable";

#if defined(__APPLE__)
    // On Apple platforms we may need to navigate outside an app bundle.
    // The executable directory would be "melonDS.app/Contents/MacOS", so we need to go a total of three steps up.
    QDir bundledir(appdirpath);
    if (bundledir.cd("..") && bundledir.cd("..") && bundledir.dirName().endsWith(".app") && bundledir.cd(".."))
    {
        portablepath = bundledir.absolutePath() + QDir::separator() + "portable";
    }
#endif

    QDir portabledir(portablepath);
    if (portabledir.exists())
    {
        emuDirectory = portabledir.absolutePath();
    }
    else
    {
        // If no overrides are specified, use the default path.
#if defined(__WIN32__) && defined(WIN32_PORTABLE)
        emuDirectory = appdirpath;
#else
        QString confdir;
        QDir config(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation));
        config.mkdir("melonDS");
        confdir = config.absolutePath() + QDir::separator() + "melonDS";
        emuDirectory = confdir;
#endif
    }
}


void setMPInterface(MPInterfaceType type)
{
    // switch to the requested MP interface
    MPInterface::Set(type);

    // set receive timeout
    // TODO: different settings per interface?
    MPInterface::Get().SetRecvTimeout(Config::GetGlobalTable().GetInt("MP.RecvTimeout"));

    // update UI appropriately
    // TODO: decide how to deal with multi-window when it becomes a thing
    for (int i = 0; i < kMaxEmuInstances; i++)
    {
        EmuInstance* inst = emuInstances[i];
        if (!inst) continue;

        MainWindow* win = inst->getMainWindow();
        if (win) win->updateMPInterface(type);
    }
}



MelonApplication::MelonApplication(int& argc, char** argv)
    : QApplication(argc, argv)
{
#if !defined(Q_OS_APPLE)
    setWindowIcon(QIcon(":/melon-icon"));
    #if defined(Q_OS_UNIX)
        setDesktopFileName(QString("net.kuribo64.melonDS"));
    #endif
#endif
}

// TODO: ROM loading should be moved to EmuInstance
// especially so the preloading below and in main() can be done in a nicer fashion

bool MelonApplication::event(QEvent *event)
{
    if (event->type() == QEvent::FileOpen)
    {
        EmuInstance* inst = emuInstances[0];
        MainWindow* win = inst->getMainWindow();
        QFileOpenEvent *openEvent = static_cast<QFileOpenEvent*>(event);

        const QStringList file = win->splitArchivePath(openEvent->file(), true);
        win->preloadROMs(file, {}, true);
    }

    return QApplication::event(event);
}

#ifdef _WIN32
static int readLocalVersion()
{
    FILE* f = fopen("version.txt", "r");
    if (!f) return 0;
    int ver = 0;
    fscanf(f, "%d", &ver);
    fclose(f);
    return ver;
}

static void writeLocalVersion(int ver)
{
    FILE* f = fopen("version.txt", "w");
    if (!f) return;
    fprintf(f, "%d\n", ver);
    fclose(f);
}

static int checkRemoteVersion()
{
    HINTERNET hSession = WinHttpOpen(L"melonDS-updater/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return -1;

    HINTERNET hConnect = WinHttpConnect(hSession,
        L"raw.githubusercontent.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return -1; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET",
        L"/ryuto-alt/melonDS-net/main/version.txt",
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return -1; }

    if (!WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, NULL))
    {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return -1;
    }

    char buf[64] = {0};
    DWORD bytesRead = 0;
    WinHttpReadData(hRequest, buf, sizeof(buf) - 1, &bytesRead);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return atoi(buf);
}

static bool downloadRelease(int version)
{
    wchar_t path[512];
    swprintf(path, 512, L"/ryuto-alt/melonDS-net/releases/download/v%d/melonDS-dist.zip", version);

    HINTERNET hSession = WinHttpOpen(L"melonDS-updater/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, L"github.com",
        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    if (!WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, NULL))
    {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return false;
    }

    // GitHub releases redirect to CDN
    DWORD statusCode = 0, statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        NULL, &statusCode, &statusCodeSize, NULL);

    if (statusCode == 301 || statusCode == 302)
    {
        wchar_t redirectUrl[2048] = {0};
        DWORD redirectSize = sizeof(redirectUrl);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION, NULL,
            redirectUrl, &redirectSize, NULL);

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        URL_COMPONENTSW urlComp = {};
        urlComp.dwStructSize = sizeof(urlComp);
        wchar_t hostName[256] = {0}, urlPath[2048] = {0};
        urlComp.lpszHostName = hostName; urlComp.dwHostNameLength = 256;
        urlComp.lpszUrlPath = urlPath; urlComp.dwUrlPathLength = 2048;

        if (!WinHttpCrackUrl(redirectUrl, 0, 0, &urlComp)) return false;

        hSession = WinHttpOpen(L"melonDS-updater/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
        if (!hSession) return false;

        hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);
        if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

        hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath,
            NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

        if (!WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0) ||
            !WinHttpReceiveResponse(hRequest, NULL))
        {
            WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
            return false;
        }
    }

    FILE* f = fopen("_update.zip", "wb");
    if (!f)
    {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return false;
    }

    char dlBuf[8192];
    DWORD bytesRead;
    while (WinHttpReadData(hRequest, dlBuf, sizeof(dlBuf), &bytesRead) && bytesRead > 0)
        fwrite(dlBuf, 1, bytesRead, f);
    fclose(f);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return true;
}

static void checkForUpdates()
{
    int localVer = readLocalVersion();
    int remoteVer = checkRemoteVersion();

    if (remoteVer <= 0 || remoteVer <= localVer)
        return;

    QString msg = QString("melonDS v%1 が利用可能です！（現在: v%2）\n今すぐ更新しますか？")
        .arg(remoteVer).arg(localVer);

    if (QMessageBox::question(nullptr, "melonDS Update", msg,
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;

    QMessageBox* progress = new QMessageBox(QMessageBox::Information,
        "melonDS Update", "ダウンロード中...", QMessageBox::NoButton);
    progress->show();
    QApplication::processEvents();

    if (!downloadRelease(remoteVer))
    {
        delete progress;
        QMessageBox::warning(nullptr, "melonDS Update", "ダウンロードに失敗しました。");
        return;
    }

    delete progress;

    // Extract and apply (keep config, BIOS, saves)
    system("powershell -NoProfile -Command \""
        "Expand-Archive -Path '_update.zip' -DestinationPath '_update_tmp' -Force"
        "\" 2>nul || python3 -c \""
        "import zipfile,os; z=zipfile.ZipFile('_update.zip'); z.extractall('_update_tmp'); z.close()\"");

    system("robocopy _update_tmp . /E /XF melonDS.toml version.txt /XD BIOS _update_tmp >nul 2>&1");

    writeLocalVersion(remoteVer);

    // Cleanup
    system("rmdir /s /q _update_tmp >nul 2>&1");
    DeleteFileA("_update.zip");

    QMessageBox::information(nullptr, "melonDS Update",
        QString("v%1 に更新しました！\n再起動してください。").arg(remoteVer));

    // Restart
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    ShellExecuteW(NULL, L"open", exePath, NULL, NULL, SW_SHOW);
    exit(0);
}
#endif

int main(int argc, char** argv)
{
    sysTimer.start();
    srand(time(nullptr));

    for (int i = 0; i < kMaxEmuInstances; i++)
        emuInstances[i] = nullptr;

    qputenv("QT_SCALE_FACTOR", "1");

#if QT_VERSION_MAJOR == 6 && defined(__WIN32__)
    // Allow using the system dark theme palette on Windows
    qputenv("QT_QPA_PLATFORM", "windows:darkmode=2");
#endif

    printf("melonDS " MELONDS_VERSION "\n");
    printf(MELONDS_URL "\n");

    // easter egg - not worth checking other cases for something so dumb
    if (argc != 0 && (!strcasecmp(argv[0], "derpDS") || !strcasecmp(argv[0], "./derpDS")))
        printf("did you just call me a derp???\n");

#ifdef _WIN32
    // argc and argv are passed as UTF8 by SDL's WinMain function
    // QT checks for the original value in local encoding though
    // to see whether it is unmodified to activate its hack that
    // retrieves the unicode value via CommandLineToArgvW.
    argc = __argc;
    argv = __argv;
#endif
    MelonApplication melon(argc, argv);
    pathInit();

    CLI::CommandLineOptions* options = CLI::ManageArgs(melon);

    // http://stackoverflow.com/questions/14543333/joystick-wont-work-using-sdl
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    SDL_SetHint(SDL_HINT_APP_NAME, "melonDS");

    if (SDL_Init(SDL_INIT_HAPTIC) < 0)
    {
        printf("SDL couldn't init rumble\n");
    }
    if (SDL_Init(SDL_INIT_JOYSTICK) < 0)
    {
        printf("SDL couldn't init joystick\n");
    }
    if (SDL_Init(SDL_INIT_SENSOR) < 0)
    {
        printf("SDL couldn't init motion sensors\n");
    }
    if (SDL_Init(SDL_INIT_AUDIO) < 0)
    {
        const char* err = SDL_GetError();
        QString errorStr = "Failed to initialize SDL. This could indicate an issue with your audio driver.\n\nThe error was: ";
        errorStr += err;

        QMessageBox::critical(nullptr, "melonDS", errorStr);
        return 1;
    }

    SDL_JoystickEventState(SDL_ENABLE);

    SDL_InitSubSystem(SDL_INIT_VIDEO);
    SDL_EnableScreenSaver(); SDL_DisableScreenSaver();

    if (!Config::Load())
        QMessageBox::critical(nullptr,
                              "melonDS",
                              "Unable to write to config.\nPlease check the write permissions of the folder you placed melonDS in.");

#ifdef _WIN32
    checkForUpdates();
#endif

    camStarted[0] = false;
    camStarted[1] = false;
    camManager[0] = new CameraManager(0, 640, 480, true);
    camManager[1] = new CameraManager(1, 640, 480, true);

    systemThemeName = new QString(QApplication::style()->objectName());

    {
        Config::Table cfg = Config::GetGlobalTable();
        QString uitheme = cfg.GetQString("UITheme");
        if (!uitheme.isEmpty())
        {
            QApplication::setStyle(uitheme);
        }
    }

    // fix for Wayland OpenGL glitches
    QGuiApplication::setAttribute(Qt::AA_NativeWindows, false);
    QGuiApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings, true);

    // default MP interface type is local MP
    // this will be changed if a LAN or netplay session is initiated
    setMPInterface(MPInterface_Local);

    NetInit();

    createEmuInstance();

    {
        MainWindow* win = emuInstances[0]->getMainWindow();
        bool memberSyntaxUsed = false;
        const auto prepareRomPath = [&](const std::optional<QString> &romPath,
                                        const std::optional<QString> &romArchivePath) -> QStringList
        {
            if (!romPath.has_value())
                return {};

            if (romArchivePath.has_value())
                return {*romPath, *romArchivePath};

            const QStringList path = win->splitArchivePath(*romPath, true);
            if (path.size() > 1) memberSyntaxUsed = true;
            return path;
        };

        const QStringList dsfile = prepareRomPath(options->dsRomPath, options->dsRomArchivePath);
        const QStringList gbafile = prepareRomPath(options->gbaRomPath, options->gbaRomArchivePath);

        if (memberSyntaxUsed) printf("Warning: use the a.zip|b.nds format at your own risk!\n");

        win->preloadROMs(dsfile, gbafile, options->boot);

        if (options->fullscreen)
            win->toggleFullscreen();
    }

    int ret = melon.exec();

    delete options;

    // if we get here, all the existing emu instances should have been deleted already
    // but with this we make extra sure they are all deleted
    deleteAllEmuInstances();

    delete camManager[0];
    delete camManager[1];

    Config::Save();

    SDL_Quit();
    return ret;
}
