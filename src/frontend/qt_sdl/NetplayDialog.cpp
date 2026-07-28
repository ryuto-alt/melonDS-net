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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <QStandardItemModel>
#include <QMessageBox>
#include <QGuiApplication>
#include <QCoreApplication>
#include <QClipboard>

#include "LAN.h"
#include "NDS.h"
#include "NDSCart.h"
#include "main.h"
#include "NetplayDialog.h"
#include "EmuInstance.h"
#include "Config.h"
#include "Platform.h"
#include "NetplaySession.h"
#include "NetplayProtocol.h"

#include "ui_NetplayStartHostDialog.h"
#include "ui_NetplayStartClientDialog.h"
#include "ui_NetplayDialog.h"

using namespace melonDS;


NetplayStartHostDialog::NetplayStartHostDialog(QWidget* parent) : QDialog(parent), ui(new Ui::NetplayStartHostDialog)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);

    ui->txtPort->setText(QString::number(kNetplayDefaultPort));
}

NetplayStartHostDialog::~NetplayStartHostDialog()
{
    delete ui;
}

void NetplayStartHostDialog::done(int r)
{
    MainWindow* mainWin = (MainWindow*)parent();
    EmuInstance* inst = mainWin->getEmuInstance();

    if (!inst)
    {
        QDialog::done(r);
        return;
    }

    if (r == QDialog::Accepted)
    {
        std::string player = ui->txtPlayerName->text().toStdString();
        int numPlayers = ui->sbNumPlayers->value();
        int inputDelay = ui->sbInputDelay->value();
        int port = ui->txtPort->text().toInt();

        if (player.empty())
        {
            QMessageBox::warning(this, "Error", "Please enter a player name.");
            return;
        }

        if (!inst->getNDS() || !inst->getNDS()->CartInserted())
        {
            QMessageBox::warning(this, "Error", "Please load a ROM before starting netplay.");
            return;
        }

        // Start the netplay session as host (player 0).
        // The session is created/destroyed on the emu thread: that thread
        // dereferences it every loop iteration, so the UI thread must not
        // swap it out from under it.
        if (!inst->getEmuThread()->netplayStart(0, numPlayers, inputDelay))
        {
            QMessageBox::critical(this, "Error", "Failed to initialize netplay session.");
            return;
        }

        // Start network host
        NetplaySession* session = inst->getNetplaySession();
        if (!session->HostStart(port))
        {
            inst->getEmuThread()->netplayStop();
            QMessageBox::critical(this, "Error", "Failed to start host on the specified port.");
            return;
        }

        // Open the port and hand the user something they can paste to a friend
        // on another network.
        std::string extaddr;
        if (UPnPForward(port, extaddr) && !extaddr.empty())
        {
            QString target = QString("%0:%1").arg(QString::fromStdString(extaddr)).arg(port);
            QGuiApplication::clipboard()->setText(target);
            QMessageBox::information(this, "melonDS",
                QString("インターネット越しの接続先:\n\n    %0\n\n"
                        "クリップボードにコピーしました。\n"
                        "相手は「ネットでダウンロードプレイ (参加)」に貼り付けるだけです。ROMは不要です。").arg(target));
        }
        else
        {
            QMessageBox::warning(this, "melonDS",
                QString("UPnPポート開放に失敗しました。\n"
                        "同じLAN内でなら遊べますが、インターネット越しには %0/UDP の手動開放が必要です。").arg(port));
        }

        // Save settings
        auto& cfg = inst->getGlobalConfig();
        cfg.SetString("Netplay.PlayerName", player);
        cfg.SetInt("Netplay.Port", port);
        cfg.SetInt("Netplay.NumPlayers", numPlayers);
        cfg.SetInt("Netplay.InputDelay", inputDelay);
        Config::Save();

        // Open session dialog
        NetplayDialog::openDlg(parentWidget(), inst);
    }

    QDialog::done(r);
}


NetplayStartClientDialog::NetplayStartClientDialog(QWidget* parent) : QDialog(parent), ui(new Ui::NetplayStartClientDialog)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);

    ui->txtPort->setText(QString::number(kNetplayDefaultPort));
}

NetplayStartClientDialog::~NetplayStartClientDialog()
{
    delete ui;
}

void NetplayStartClientDialog::done(int r)
{
    MainWindow* mainWin = (MainWindow*)parent();
    EmuInstance* inst = mainWin->getEmuInstance();

    if (!inst)
    {
        QDialog::done(r);
        return;
    }

    if (r == QDialog::Accepted)
    {
        std::string player = ui->txtPlayerName->text().toStdString();
        QString hostStr = ui->txtIPAddress->text().trimmed();
        int port = ui->txtPort->text().toInt();

        // Let the host's "IP:port" be pasted straight in.
        int colon = hostStr.lastIndexOf(':');
        if (colon > 0 && !hostStr.contains('['))
        {
            bool ok = false;
            int p = hostStr.mid(colon+1).toInt(&ok);
            if (ok && p >= 1024 && p <= 65535)
            {
                port = p;
                hostStr = hostStr.left(colon);
                ui->txtIPAddress->setText(hostStr);
                ui->txtPort->setText(QString::number(port));
            }
        }
        std::string host = hostStr.toStdString();

        if (player.empty() || host.empty())
        {
            QMessageBox::warning(this, "Error", "Please enter player name and host address.");
            return;
        }

        // No ROM needed on this side: the host sends its cart and firmware, and
        // the consoles are built from those once they arrive.
        if (!inst->getEmuThread()->netplayStart(1, 2, 4))
        {
            QMessageBox::critical(this, "Error", "Failed to initialize netplay session.");
            return;
        }

        // Pump the Qt event loop between connect slices so the window keeps
        // repainting; user input is excluded to avoid re-entering this code.
        NetplaySession* session = inst->getNetplaySession();
        auto pumpUI = []() {
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        };
        if (!session->ClientConnect(host.c_str(), port, 5000, pumpUI))
        {
            inst->getEmuThread()->netplayStop();
            QMessageBox::critical(this, "Error",
                QString("Failed to connect to %1:%2").arg(QString::fromStdString(host)).arg(port));
            return;
        }

        // Save settings
        auto& cfg = inst->getGlobalConfig();
        cfg.SetString("Netplay.PlayerName", player);
        cfg.SetString("Netplay.HostAddress", host);
        cfg.SetInt("Netplay.Port", port);
        Config::Save();

        // Open session dialog
        NetplayDialog::openDlg(parentWidget(), inst);
    }

    QDialog::done(r);
}


NetplayDialog::NetplayDialog(QWidget* parent, EmuInstance* inst)
    : QDialog(parent), ui(new Ui::NetplayDialog), emuInstance(inst)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);

    QStandardItemModel* model = new QStandardItemModel();
    ui->tvPlayerList->setModel(model);

    connect(ui->btnDisconnect, &QPushButton::clicked, this, &NetplayDialog::onDisconnect);

    // Update timer for status display
    updateTimer = new QTimer(this);
    connect(updateTimer, &QTimer::timeout, this, &NetplayDialog::onUpdateTimer);
    updateTimer->start(500); // update every 500ms

    NetplaySession* session = inst->getNetplaySession();
    if (session)
    {
        if (session->IsHost())
            ui->lblStatus->setText("状態: ホスト中（参加待ち…）");
        else
            ui->lblStatus->setText("状態: ホストに接続済み");

        // Set up desync callback
        session->SetDesyncCallback([this](melonDS::u32 frame, melonDS::u64 localHash, melonDS::u64 remoteHash) {
            // Qt signal/slot across threads - use QMetaObject::invokeMethod
            QMetaObject::invokeMethod(this, [this, frame]() {
                setDesyncWarning(QString("DESYNC detected at frame %1!").arg(frame));
            }, Qt::QueuedConnection);
        });

        session->SetDisconnectCallback([this](int playerID, melonDS::NetplayDisconnectReason reason) {
            QMetaObject::invokeMethod(this, [this, playerID]() {
                setStatus(QString("Player %1 disconnected").arg(playerID));
            }, Qt::QueuedConnection);
        });
    }
}

NetplayDialog::~NetplayDialog()
{
    // The desync/disconnect callbacks capture `this`, and the session can
    // outlive this WA_DeleteOnClose dialog. Unhook them before the dialog
    // dies or the emu thread will call into freed memory.
    if (emuInstance)
    {
        if (NetplaySession* session = emuInstance->getNetplaySession())
        {
            session->SetDesyncCallback(nullptr);
            session->SetDisconnectCallback(nullptr);
        }
    }

    if (updateTimer)
    {
        updateTimer->stop();
        delete updateTimer;
    }
    delete ui;
}

void NetplayDialog::done(int r)
{
    if (updateTimer) updateTimer->stop();
    QDialog::done(r);
}

void NetplayDialog::onDisconnect()
{
    if (emuInstance)
    {
        // Tear the session down on the emu thread (it is the thread that
        // touches the session every loop iteration).
        emuInstance->getEmuThread()->netplayStop();
    }
    close();
}

void NetplayDialog::onUpdateTimer()
{
    if (!emuInstance) return;

    NetplaySession* session = emuInstance->getNetplaySession();
    if (!session || !session->IsActive())
    {
        ui->lblStatus->setText("Status: Disconnected");
        return;
    }

    // Update frame counter
    QString status;
    if (session->IsHost())
        status = QString("Status: Hosting | Frame: %1").arg(session->GetFrameNum());
    else
        status = QString("Status: Connected | Frame: %1").arg(session->GetFrameNum());

    ui->lblStatus->setText(status);

    // Update player list
    QStandardItemModel* model = (QStandardItemModel*)ui->tvPlayerList->model();
    model->clear();

    const QStringList header = {"#", "Player", "Status"};
    model->setHorizontalHeaderLabels(header);
    model->setRowCount(session->GetNumInstances());

    for (int i = 0; i < session->GetNumInstances(); i++)
    {
        QString id = QString("%1").arg(i);
        model->setItem(i, 0, new QStandardItem(id));

        QString name = (i == session->GetLocalPlayerID()) ? "You" : QString("Player %1").arg(i);
        model->setItem(i, 1, new QStandardItem(name));

        // Actual connection state, not GetInstance() -- the mirror instances
        // all exist locally from the start, which made every seat look
        // "Active" before anyone had even connected.
        QString playerStatus;
        if (i == session->GetLocalPlayerID())
            playerStatus = "You";
        else if (session->IsPlayerConnected(i))
            playerStatus = "Connected";
        else
            playerStatus = "Waiting";
        model->setItem(i, 2, new QStandardItem(playerStatus));
    }
}

void NetplayDialog::setStatus(const QString& status)
{
    ui->lblStatus->setText("Status: " + status);
}

void NetplayDialog::setDesyncWarning(const QString& warning)
{
    ui->lblDesync->setText(warning);
}
