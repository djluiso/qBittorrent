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

#include "ota_win.h"

#include <cstring>
#include <optional>
#include <string>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QThread>
#include <QVersionNumber>

#include <windows.h>
#include <aclapi.h>
#include <objbase.h>
#include <oleauto.h>
#include <sddl.h>
#include <taskschd.h>
#include <tlhelp32.h>

#include "base/global.h"
#include "base/version.h"

#ifndef QBT_OTA_REPO
#define QBT_OTA_REPO ""
#endif
#ifndef QBT_OTA_BUILD
#define QBT_OTA_BUILD 0
#endif

namespace
{
    const wchar_t TASK_NAME[] = L"qBittorrent OTA Update";
    // SYSTEM & Administrators: full control; Authenticated Users: read + run
    const wchar_t TASK_SDDL[] = L"D:(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;AU)";
    // SYSTEM & Administrators: full control; Users: read & execute (protected, inherited by children)
    const wchar_t DATA_DIR_SDDL[] = L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)";

    const int WAIT_FOR_EXIT_SECS = 90;
    const int MAX_INSTALL_ATTEMPTS = 3;

    struct Release
    {
        QVersionNumber version;
        int build = 0;

        bool isNewerThan(const Release &other) const
        {
            const int cmp = QVersionNumber::compare(version, other.version);
            return (cmp > 0) || ((cmp == 0) && (build > other.build));
        }

        QString toString() const
        {
            return (build > 0)
                ? u"%1 (OTA build %2)"_s.arg(version.toString(), QString::number(build))
                : version.toString();
        }
    };

    const Release CURRENT_RELEASE {QVersionNumber(QBT_VERSION_MAJOR, QBT_VERSION_MINOR, QBT_VERSION_BUGFIX), QBT_OTA_BUILD};

    QString dataDir()
    {
        return qEnvironmentVariable("ProgramData", u"C:\\ProgramData"_s) + u"\\qBittorrentOTA"_s;
    }

    QString stageDir()
    {
        return dataDir() + u"\\staged"_s;
    }

    void writeLog(const QString &message)
    {
        const QString path = dataDir() + u"\\ota.log"_s;
        if (QFileInfo(path).size() > (1024 * 1024))
        {
            QFile::remove(path + u".old"_s);
            QFile::rename(path, (path + u".old"_s));
        }
        QFile file {path};
        if (file.open(QIODevice::Append | QIODevice::Text))
            file.write((QDateTime::currentDateTime().toString(u"yyyy-MM-dd HH:mm:ss"_s) + u"  "_s + message + u'\n').toUtf8());
    }

    void setStatus(const QString &state, const QString &available = {}, const QString &message = {}
        , const Release &installed = CURRENT_RELEASE)
    {
        const QJsonObject obj {
            {u"state"_s, state},
            {u"installed"_s, installed.toString()},
            {u"available"_s, available},
            {u"message"_s, message},
            {u"updatedAt"_s, QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}
        };
        QSaveFile file {Ota::statusFilePath()};
        if (file.open(QIODevice::WriteOnly))
        {
            file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
            file.commit();
        }
        writeLog(u"status=%1 %2 %3"_s.arg(state, available, message));
    }

    // Honour the in-app setting "Automatically download and install updates" of every local profile.
    bool isDisabledByUser()
    {
        const QSettings profileList {u"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList"_s, QSettings::NativeFormat};
        for (const QString &sid : profileList.childGroups())
        {
            const QString profileDir = profileList.value(sid + u"/ProfileImagePath"_s).toString();
            if (profileDir.isEmpty())
                continue;

            const QString iniPath = profileDir + u"\\AppData\\Roaming\\qBittorrent\\qBittorrent.ini"_s;
            if (!QFile::exists(iniPath))
                continue;

            const QSettings ini {iniPath, QSettings::IniFormat};
            if (!ini.value(u"Preferences/Advanced/otaUpdates"_s, true).toBool())
                return true;
        }
        return false;
    }

    bool isOtherInstanceRunning()
    {
        bool found = false;
        const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return false;

        PROCESSENTRY32W entry {};
        entry.dwSize = sizeof(entry);
        const DWORD selfPid = ::GetCurrentProcessId();
        for (BOOL ok = ::Process32FirstW(snapshot, &entry); ok; ok = ::Process32NextW(snapshot, &entry))
        {
            if ((entry.th32ProcessID != selfPid) && (_wcsicmp(entry.szExeFile, L"qbittorrent.exe") == 0))
            {
                found = true;
                break;
            }
        }
        ::CloseHandle(snapshot);
        return found;
    }

    // ---- networking (synchronous, this mode has no UI) ----

    QNetworkRequest makeRequest(const QString &url)
    {
        QNetworkRequest request {QUrl(url)};
        request.setHeader(QNetworkRequest::UserAgentHeader, u"qBittorrent-OTA/" QBT_VERSION_2 ""_s);
        request.setRawHeader("Accept", "application/vnd.github+json");
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setTransferTimeout(60000);
        return request;
    }

    std::optional<QByteArray> httpGet(QNetworkAccessManager &nam, const QString &url, QString &error)
    {
        QNetworkReply *reply = nam.get(makeRequest(url));
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            error = reply->errorString();
            return std::nullopt;
        }
        return reply->readAll();
    }

    bool httpDownload(QNetworkAccessManager &nam, const QString &url, const QString &path, QString &error)
    {
        QFile file {path};
        if (!file.open(QIODevice::WriteOnly))
        {
            error = file.errorString();
            return false;
        }

        QNetworkReply *reply = nam.get(makeRequest(url));
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::readyRead, &loop, [reply, &file]() { file.write(reply->readAll()); });
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        file.write(reply->readAll());
        file.close();
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            error = reply->errorString();
            return false;
        }
        return true;
    }

    QString sha256OfFile(const QString &path)
    {
        QFile file {path};
        if (!file.open(QIODevice::ReadOnly))
            return {};
        QCryptographicHash hash {QCryptographicHash::Sha256};
        hash.addData(&file);
        return QString::fromLatin1(hash.result().toHex());
    }

    // ---- staged update ----

    struct Staged
    {
        Release release;
        QString filePath;
        QJsonObject info;
    };

    void removeStaged()
    {
        QDir(stageDir()).removeRecursively();
    }

    void saveStagedInfo(const QJsonObject &info)
    {
        QSaveFile file {stageDir() + u"\\info.json"_s};
        if (file.open(QIODevice::WriteOnly))
        {
            file.write(QJsonDocument(info).toJson(QJsonDocument::Compact));
            file.commit();
        }
    }

    std::optional<Staged> loadStaged()
    {
        QFile file {stageDir() + u"\\info.json"_s};
        if (!file.open(QIODevice::ReadOnly))
            return std::nullopt;

        const QJsonObject info = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        Staged staged {
            {QVersionNumber::fromString(info[u"version"_s].toString()), info[u"build"_s].toInt()},
            (stageDir() + u'\\' + info[u"file"_s].toString()),
            info
        };
        const QString expected = info[u"sha256"_s].toString();
        if (staged.release.version.isNull() || expected.isEmpty()
            || (sha256OfFile(staged.filePath).compare(expected, Qt::CaseInsensitive) != 0))
        {
            writeLog(u"Discarding invalid staged update."_s);
            removeStaged();
            return std::nullopt;
        }
        return staged;
    }

    QString expectedSha256(QNetworkAccessManager &nam, const QJsonObject &releaseObj, const QJsonObject &asset, QString &error)
    {
        const QRegularExpressionMatch digest = QRegularExpression(u"^sha256:([0-9a-fA-F]{64})$"_s).match(asset[u"digest"_s].toString());
        if (digest.hasMatch())
            return digest.captured(1).toLower();

        const QJsonArray assets = releaseObj[u"assets"_s].toArray();
        for (const QJsonValue &value : assets)
        {
            const QJsonObject sums = value.toObject();
            if (sums[u"name"_s].toString() != u"SHA256SUMS")
                continue;

            const auto data = httpGet(nam, sums[u"browser_download_url"_s].toString(), error);
            if (!data)
                return {};
            const QRegularExpression lineRegex {u"^([0-9a-fA-F]{64})\\s+\\*?(\\S+)\\s*$"_s, QRegularExpression::MultilineOption};
            for (const QRegularExpressionMatch &m : lineRegex.globalMatch(QString::fromUtf8(*data)))
            {
                if (m.captured(2) == asset[u"name"_s].toString())
                    return m.captured(1).toLower();
            }
        }
        error = u"No SHA-256 is published for %1."_s.arg(asset[u"name"_s].toString());
        return {};
    }

    bool launchInstallerAfterExit(const QString &installerPath)
    {
        // The installer replaces qbittorrent.exe, which is the image of this very process,
        // so start it detached after a short delay that lets this process exit first.
        QProcess proc;
        proc.setProgram(qEnvironmentVariable("ComSpec", u"C:\\Windows\\System32\\cmd.exe"_s));
        proc.setNativeArguments(u"/d /c \"ping -n 6 127.0.0.1 >nul & \"%1\" /S\""_s.arg(QDir::toNativeSeparators(installerPath)));
        proc.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args)
        {
            args->flags |= CREATE_NO_WINDOW;
        });
        proc.setStandardOutputFile(QProcess::nullDevice());
        proc.setStandardErrorFile(QProcess::nullDevice());
        return proc.startDetached();
    }

    // ---- commands ----

    int runUpdate()
    {
        if (!ensureDataDir())
            return 2;

        if (std::strlen(QBT_OTA_REPO) == 0)
        {
            setStatus(u"disabled"_s, {}, u"This build has no OTA update source."_s);
            return 0;
        }
        if (isDisabledByUser())
        {
            removeStaged();
            setStatus(u"disabled"_s, {}, u"Automatic updates are turned off in qBittorrent options."_s);
            return 0;
        }

        setStatus(u"checking"_s);

        std::optional<Staged> staged = loadStaged();
        if (staged && !staged->release.isNewerThan(CURRENT_RELEASE))
        {
            // Either the staged update has been installed (this is the new binary), or it is obsolete.
            writeLog(u"Running %1; removing staged %2."_s.arg(CURRENT_RELEASE.toString(), staged->release.toString()));
            removeStaged();
            staged.reset();
        }

        QNetworkAccessManager nam;
        QString error;
        const auto releaseData = httpGet(nam, u"https://api.github.com/repos/" QBT_OTA_REPO "/releases/latest"_s, error);
        if (releaseData)
        {
            const QJsonObject releaseObj = QJsonDocument::fromJson(*releaseData).object();
            const QString tag = releaseObj[u"tag_name"_s].toString();
            const QRegularExpressionMatch m = QRegularExpression(u"^v(\\d+\\.\\d+\\.\\d+)-ota\\.(\\d+)$"_s).match(tag);
            if (!releaseObj[u"draft"_s].toBool() && !releaseObj[u"prerelease"_s].toBool() && m.hasMatch())
            {
                const Release candidate {QVersionNumber::fromString(m.captured(1)), m.captured(2).toInt()};
                if (candidate.isNewerThan(CURRENT_RELEASE) && (!staged || candidate.isNewerThan(staged->release)))
                {
                    const QString assetName = u"qbittorrent_%1_x64_setup.exe"_s.arg(m.captured(1));
                    QJsonObject asset;
                    const QJsonArray assets = releaseObj[u"assets"_s].toArray();
                    for (const QJsonValue &value : assets)
                    {
                        if (value.toObject()[u"name"_s].toString() == assetName)
                            asset = value.toObject();
                    }
                    if (asset.isEmpty())
                    {
                        setStatus(u"error"_s, candidate.toString(), u"Release %1 has no %2."_s.arg(tag, assetName));
                        return 1;
                    }

                    const QString expected = expectedSha256(nam, releaseObj, asset, error);
                    if (expected.isEmpty())
                    {
                        setStatus(u"error"_s, candidate.toString(), error);
                        return 1;
                    }

                    setStatus(u"downloading"_s, candidate.toString());
                    removeStaged();
                    QDir().mkpath(stageDir());
                    const QString filePath = stageDir() + u'\\' + assetName;
                    if (!httpDownload(nam, asset[u"browser_download_url"_s].toString(), filePath, error))
                    {
                        removeStaged();
                        setStatus(u"error"_s, candidate.toString(), u"Download failed: %1"_s.arg(error));
                        return 1;
                    }
                    const QString actual = sha256OfFile(filePath);
                    if (actual.compare(expected, Qt::CaseInsensitive) != 0)
                    {
                        removeStaged();
                        setStatus(u"error"_s, candidate.toString(), u"SHA-256 mismatch for %1 (got %2, expected %3)."_s.arg(assetName, actual, expected));
                        return 1;
                    }
                    writeLog(u"Downloaded and verified %1 (%2) from %3."_s.arg(assetName, actual, tag));
                    saveStagedInfo({
                        {u"tag"_s, tag},
                        {u"version"_s, candidate.version.toString()},
                        {u"build"_s, candidate.build},
                        {u"file"_s, assetName},
                        {u"sha256"_s, actual},
                        {u"attempts"_s, 0}
                    });
                    staged = loadStaged();
                }
            }
            else
            {
                writeLog(u"Ignoring latest release '%1' (not a stable OTA release)."_s.arg(tag));
            }
        }
        else if (!staged)
        {
            setStatus(u"error"_s, {}, u"Could not reach GitHub to check for updates: %1"_s.arg(error));
            return 1;
        }

        if (!staged)
        {
            setStatus(u"uptodate"_s);
            return 0;
        }

        const QString available = staged->release.toString();
        setStatus(u"staged"_s, available, u"Will be installed automatically after qBittorrent exits."_s);

        for (int i = 0; (i < WAIT_FOR_EXIT_SECS) && isOtherInstanceRunning(); i += 2)
            QThread::sleep(2);
        if (isOtherInstanceRunning())
        {
            writeLog(u"qBittorrent is still running; the update stays staged."_s);
            return 0;
        }

        const int attempts = staged->info[u"attempts"_s].toInt() + 1;
        if (attempts > MAX_INSTALL_ATTEMPTS)
        {
            removeStaged();
            setStatus(u"error"_s, available, u"Installing the update failed %1 times; it will be downloaded again."_s.arg(MAX_INSTALL_ATTEMPTS));
            return 1;
        }
        staged->info[u"attempts"_s] = attempts;
        saveStagedInfo(staged->info);

        if (!launchInstallerAfterExit(staged->filePath))
        {
            setStatus(u"error"_s, available, u"Could not start the installer."_s);
            return 1;
        }
        // The next run (qBittorrent triggers one on startup) confirms the new version.
        setStatus(u"installing"_s, available, u"Installing silently (attempt %1)."_s.arg(attempts));
        return 0;
    }

    template <typename T>
    void releaseCom(T *&ptr)
    {
        if (ptr)
        {
            ptr->Release();
            ptr = nullptr;
        }
    }

    class ComScope
    {
    public:
        ComScope() : m_hr {::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)} {}
        ~ComScope()
        {
            if (SUCCEEDED(m_hr))
                ::CoUninitialize();
        }

    private:
        HRESULT m_hr;
    };

    ITaskFolder *openRootTaskFolder()
    {
        ITaskService *service = nullptr;
        if (FAILED(::CoCreateInstance(__uuidof(TaskScheduler), nullptr, CLSCTX_INPROC_SERVER, __uuidof(ITaskService), reinterpret_cast<void **>(&service))))
            return nullptr;

        VARIANT empty;
        ::VariantInit(&empty);
        ITaskFolder *folder = nullptr;
        BSTR root = ::SysAllocString(L"\\");
        if (SUCCEEDED(service->Connect(empty, empty, empty, empty)))
            service->GetFolder(root, &folder);
        ::SysFreeString(root);
        releaseCom(service);
        return folder;
    }

    QString xmlEscape(QString text)
    {
        return text.replace(u'&', u"&amp;"_s).replace(u'<', u"&lt;"_s).replace(u'>', u"&gt;"_s).replace(u'"', u"&quot;"_s);
    }

    bool isOwnedByAdmins(const std::wstring &path)
    {
        PSID owner = nullptr;
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd) != ERROR_SUCCESS)
            return false;
        const bool trusted = ::IsWellKnownSid(owner, WinLocalSystemSid) || ::IsWellKnownSid(owner, WinBuiltinAdministratorsSid);
        ::LocalFree(sd);
        return trusted;
    }

    // The staging folder is executed from as SYSTEM, so it must only ever be writable by admins.
    // A folder pre-created by someone else is discarded.
    bool ensureDataDir()
    {
        const std::wstring path = QDir::toNativeSeparators(dataDir()).toStdWString();
        if (QFileInfo::exists(dataDir()) && !isOwnedByAdmins(path))
        {
            if (!QDir(dataDir()).removeRecursively())
                return false;
        }

        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(DATA_DIR_SDDL, SDDL_REVISION_1, &sd, nullptr))
            return false;

        bool ok = false;
        if (!QFileInfo::exists(dataDir()))
        {
            SECURITY_ATTRIBUTES sa {sizeof(SECURITY_ATTRIBUTES), sd, FALSE};
            ok = ::CreateDirectoryW(path.c_str(), &sa);
        }
        else
        {
            BOOL daclPresent = FALSE;
            BOOL daclDefaulted = FALSE;
            PACL dacl = nullptr;
            PSID owner = nullptr;
            BOOL ownerDefaulted = FALSE;
            ::GetSecurityDescriptorDacl(sd, &daclPresent, &dacl, &daclDefaulted);
            ::GetSecurityDescriptorOwner(sd, &owner, &ownerDefaulted);
            ok = (::SetNamedSecurityInfoW(const_cast<wchar_t *>(path.c_str()), SE_FILE_OBJECT
                , (OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION)
                , owner, nullptr, dacl, nullptr) == ERROR_SUCCESS);
        }
        ::LocalFree(sd);
        return ok && isOwnedByAdmins(path);
    }

    int runRegister()
    {
        if (!ensureDataDir())
            return 2;

        const QString exePath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        const QString xml = uR"(<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo>
    <Description>Checks for, verifies and silently installs stable qBittorrent OTA releases.</Description>
  </RegistrationInfo>
  <Triggers>
    <BootTrigger>
      <Enabled>true</Enabled>
      <Delay>PT3M</Delay>
    </BootTrigger>
    <CalendarTrigger>
      <StartBoundary>2020-01-01T00:00:00</StartBoundary>
      <Enabled>true</Enabled>
      <ScheduleByDay><DaysInterval>1</DaysInterval></ScheduleByDay>
      <Repetition><Interval>PT6H</Interval><Duration>P1D</Duration></Repetition>
    </CalendarTrigger>
  </Triggers>
  <Principals>
    <Principal id="Author">
      <UserId>S-1-5-18</UserId>
      <RunLevel>HighestAvailable</RunLevel>
    </Principal>
  </Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <StartWhenAvailable>true</StartWhenAvailable>
    <ExecutionTimeLimit>PT1H</ExecutionTimeLimit>
    <Enabled>true</Enabled>
  </Settings>
  <Actions Context="Author">
    <Exec>
      <Command>"%1"</Command>
      <Arguments>--ota-update</Arguments>
    </Exec>
  </Actions>
</Task>
)"_s.arg(xmlEscape(exePath));

        const ComScope com;
        ITaskFolder *folder = openRootTaskFolder();
        if (!folder)
            return 3;

        VARIANT empty;
        ::VariantInit(&empty);
        VARIANT sddl;
        ::VariantInit(&sddl);
        sddl.vt = VT_BSTR;
        sddl.bstrVal = ::SysAllocString(TASK_SDDL);
        BSTR name = ::SysAllocString(TASK_NAME);
        BSTR xmlText = ::SysAllocString(reinterpret_cast<const wchar_t *>(xml.utf16()));
        IRegisteredTask *task = nullptr;
        const HRESULT hr = folder->RegisterTask(name, xmlText, TASK_CREATE_OR_UPDATE, empty, empty
            , TASK_LOGON_SERVICE_ACCOUNT, sddl, &task);
        ::SysFreeString(xmlText);
        ::SysFreeString(name);
        ::VariantClear(&sddl);
        releaseCom(task);
        releaseCom(folder);
        return SUCCEEDED(hr) ? 0 : 4;
    }

    int runUnregister()
    {
        {
            const ComScope com;
            if (ITaskFolder *folder = openRootTaskFolder())
            {
                BSTR name = ::SysAllocString(TASK_NAME);
                folder->DeleteTask(name, 0);
                ::SysFreeString(name);
                releaseCom(folder);
            }
        }
        QDir(dataDir()).removeRecursively();
        return 0;
    }
}

QString Ota::statusFilePath()
{
    return dataDir() + u"\\status.json"_s;
}

bool Ota::triggerTask()
{
    const ComScope com;
    ITaskFolder *folder = openRootTaskFolder();
    if (!folder)
        return false;

    BSTR name = ::SysAllocString(TASK_NAME);
    IRegisteredTask *task = nullptr;
    HRESULT hr = folder->GetTask(name, &task);
    ::SysFreeString(name);
    releaseCom(folder);
    if (FAILED(hr) || !task)
        return false;

    VARIANT empty;
    ::VariantInit(&empty);
    IRunningTask *running = nullptr;
    hr = task->Run(empty, &running);
    releaseCom(running);
    releaseCom(task);
    return SUCCEEDED(hr);
}

int Ota::runCommand(int argc, char *argv[])
{
    if (argc != 2)
        return -1;

    int (*command)() = nullptr;
    if (std::strcmp(argv[1], "--ota-update") == 0)
        command = runUpdate;
    else if (std::strcmp(argv[1], "--ota-register") == 0)
        command = runRegister;
    else if (std::strcmp(argv[1], "--ota-unregister") == 0)
        command = runUnregister;
    else
        return -1;

    const QCoreApplication app {argc, argv};
    return command();
}
