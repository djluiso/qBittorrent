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

#pragma once

#include <QString>

// Over-the-air (OTA) updates for Windows.
//
// qBittorrent is installed per-machine (Program Files), so installing an update needs admin
// rights. The installer runs `qbittorrent.exe --ota-register`, which registers the SYSTEM
// scheduled task "qBittorrent OTA Update" (any signed-in user may run it, only admins may
// change it). That task runs `qbittorrent.exe --ota-update`, a windowless mode that:
//   - checks the OTA repository's latest stable GitHub release (vX.Y.Z-ota.N),
//   - downloads the installer into an admin-only folder and verifies its SHA-256,
//   - installs it silently (/S) once no qBittorrent window is running.
// The regular app triggers the task (startup, periodically, on demand, on exit) and reads
// the task's status from statusFilePath().
namespace Ota
{
    // Returns -1 if `argv` is not an OTA command, otherwise the command's exit code.
    int runCommand(int argc, char *argv[]);

    bool triggerTask();
    QString statusFilePath();
}
