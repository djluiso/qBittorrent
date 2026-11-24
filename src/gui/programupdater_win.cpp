/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2026  qBittorrent OTA contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

// Windows implementation of ProgramUpdater for OTA builds.
//
// qBittorrent is installed per-machine (Program Files), so installing an update
// needs admin rights. Instead of prompting, the installer registers a SYSTEM
// scheduled task ("qBittorrent OTA Update", see base/ota_win.cpp)
// that ordinary users may trigger but not modify. The task:
//   - checks this fork's latest *stable* GitHub release,
//   - downloads and SHA-256 verifies the installer into an admin-only folder,
//   - installs it silently (/S) once qBittorrent is not running.
// The app triggers the task on startup, periodically, on demand and on exit,
// and reads the task's status from %ProgramData%\qBittorrentOTA\status.json.

#include "programupdater.h"

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include "base/global.h"
#include "base/ota_win.h"
#include "base/net/downloadmanager.h"

namespace
{
    const int POLL_INTERVAL_MS = 1000;
    const int POLL_TIMEOUT_SECS = 300;

    struct StatusFile
    {
        ProgramUpdater::OtaStatus status;
        QDateTime updatedAt;
        bool isValid = false;
    };

    StatusFile readStatusFile()
    {
        StatusFile result;
        QFile file {Ota::statusFilePath()};
        if (!file.open(QIODevice::ReadOnly))
            return result;

        const QJsonObject obj = QJsonDocument::fromJson(file.readAll()).object();
        if (obj.isEmpty())
            return result;

        result.status.state = obj[u"state"_s].toString();
        result.status.installedVersion = obj[u"installed"_s].toString();
        result.status.availableVersion = obj[u"available"_s].toString();
        result.status.message = obj[u"message"_s].toString();
        result.updatedAt = QDateTime::fromString(obj[u"updatedAt"_s].toString(), Qt::ISODate);
        result.isValid = true;
        return result;
    }

    bool isBusyState(const QString &state)
    {
        return (state == u"checking") || (state == u"downloading") || (state == u"installing");
    }
}

bool ProgramUpdater::triggerOtaTask()
{
    return Ota::triggerTask();
}

void ProgramUpdater::checkForUpdates()
{
    m_otaStatus = {};
    const QDateTime triggeredAt = QDateTime::currentDateTimeUtc().addSecs(-2);

    if (!triggerOtaTask())
    {
        m_otaStatus.state = u"error"_s;
        m_otaStatus.message = tr("The OTA update task is not installed. Reinstall qBittorrent to enable automatic updates.");
        QTimer::singleShot(0, this, &ProgramUpdater::updateCheckFinished);
        return;
    }

    auto *timer = new QTimer(this);
    timer->setInterval(POLL_INTERVAL_MS);
    connect(timer, &QTimer::timeout, this, [this, timer, triggeredAt]()
    {
        const StatusFile file = readStatusFile();
        const bool isFresh = file.isValid && file.updatedAt.isValid() && (file.updatedAt >= triggeredAt);
        const bool isTimedOut = (triggeredAt.secsTo(QDateTime::currentDateTimeUtc()) > POLL_TIMEOUT_SECS);

        if ((isFresh && !isBusyState(file.status.state)) || isTimedOut)
        {
            timer->stop();
            timer->deleteLater();
            m_otaStatus = file.status;
            if (!isFresh || isBusyState(m_otaStatus.state))
                m_otaStatus.state = u"timeout"_s;
            emit updateCheckFinished();
        }
    });
    timer->start();
}

ProgramUpdater::OtaStatus ProgramUpdater::otaStatus() const
{
    return m_otaStatus;
}

ProgramUpdater::Version ProgramUpdater::getNewVersion() const
{
    if (m_otaStatus.state == u"staged")
        return Version::fromString(m_otaStatus.availableVersion);
    return {};
}

bool ProgramUpdater::updateProgram() const
{
    // A staged update is installed by the OTA task as soon as qBittorrent exits.
    return triggerOtaTask();
}

void ProgramUpdater::fallbackDownloadFinished([[maybe_unused]] const Net::DownloadResult &result, [[maybe_unused]] Version &version)
{
    // Not used by OTA builds (declared as a slot in the shared header).
}
