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

        if (!inst->getNDS()->CartInserted())
        {
            QMessageBox::warning(this, "Error", "Please load a ROM before starting netplay.");
            return;
        }

        // Start the netplay session as host (player 0)
        if (!inst->startNetplaySession(0, numPlayers, inputDelay))
        {
            QMessageBox::critical(this, "Error", "Failed to initialize netplay session.");
            return;
        }

        // Start network host
        NetplaySession* session = inst->getNetplaySession();
        if (!session->HostStart(port))
        {
            inst->stopNetplaySession();
            QMessageBox::critical(this, "Error", "Failed to start host on the specified port.");
            return;
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
        std::string host = ui->txtIPAddress->text().toStdString();
        int port = ui->txtPort->text().toInt();

        if (player.empty() || host.empty())
        {
            QMessageBox::warning(this, "Error", "Please enter player name and host address.");
            return;
        }

        if (!inst->getNDS()->CartInserted())
        {
            QMessageBox::warning(this, "Error", "Please load the same ROM as the host before joining.");
            return;
        }

        // Client will receive numPlayers and inputDelay from host during handshake.
        // For now, use defaults - they'll be updated after connection.
        if (!inst->startNetplaySession(1, 2, 4))
        {
            QMessageBox::critical(this, "Error", "Failed to initialize netplay session.");
            return;
        }

        NetplaySession* session = inst->getNetplaySession();
        if (!session->ClientConnect(host.c_str(), port))
        {
            inst->stopNetplaySession();
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
            ui->lblStatus->setText("Status: Hosting (waiting for players...)");
        else
            ui->lblStatus->setText("Status: Connected to host");

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
        emuInstance->stopNetplaySession();
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

        QString playerStatus = session->GetInstance(i) ? "Active" : "N/A";
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
