# qBittorrent with silent OTA updates (Windows)

A fork of [qBittorrent](https://github.com/qbittorrent/qBittorrent) whose Windows build updates itself
over the air: in the background, silently, with no "new version available" dialog and no website.
Only **stable** upstream releases are used; betas and release candidates are ignored.

**Install:** download `qbittorrent_X.Y.Z_x64_setup.exe` from the [latest release](../../releases/latest)
and run it once. From then on, updates install themselves.

## How it works

| Piece | What it does |
|---|---|
| `qbittorrent.exe --ota-register` | Run by the installer. Registers the SYSTEM scheduled task **qBittorrent OTA Update** (users may trigger it, only admins may change it). |
| `qbittorrent.exe --ota-update` | Run by that task: checks this repo's latest stable release (`vX.Y.Z-ota.N`), downloads the installer into an admin-only folder, verifies its SHA-256, and installs it silently once qBittorrent is closed. |
| qBittorrent itself | Triggers the task on startup, every 24 h, from **Help > Check for Updates**, and on exit. A downloaded update is installed right after you exit; nothing interrupts your downloads. |
| [OTA release workflow](.github/workflows/ota-release.yml) | Every 6 hours, checks upstream for a new stable release, applies [`patches/`](patches), builds it with upstream's own Windows build recipe, and publishes `vX.Y.Z-ota.N`. |

No scripts or extra programs are installed: the updater is part of `qbittorrent.exe`.

- **Turn it off:** Tools > Options > Advanced > *Automatically download and install updates in the background*.
- **Status and log:** `C:\ProgramData\qBittorrentOTA\status.json` and `ota.log`.

## Maintenance

The OTA change is a single patch on top of each upstream release:
[`patches/0001-Windows-silent-OTA-updates-from-stable-releases.patch`](patches).
If a future upstream release changes the same code, the workflow fails with
*"The OTA patch no longer applies"*. Rebase the patch onto the new `release-X.Y.Z` tag, commit it
here, and run the workflow again (Actions > OTA release > Run workflow).

## Trust

Releases here are built by this repository's GitHub Actions from upstream's tagged source plus the
patch in `patches/`. They are not the binaries signed by the qBittorrent maintainers.
