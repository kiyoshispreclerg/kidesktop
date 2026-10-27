# kimountd

Mounts the disks the user chose in kiconf's **Discos** tab, whenever they
are connected — including the ones already connected when the session
starts. Started by `kisession` (service `kimountd`, on by default; it does
nothing until a disk is checked).

## How it works

- Talks to **udisks2** on the system bus: `GetManagedObjects` at startup,
  then the `InterfacesAdded` (plug-in) and `PropertiesChanged` (mount points)
  signals. Nothing is polled.
- Mounts with udisks' own `Filesystem.Mount`, the call KDE, GNOME, XFCE and
  GIO make, so the mount point is wherever udisks puts it on the distribution:
  `/media/$USER/LABEL` on Debian, Ubuntu and Devuan, `/run/media/$USER/LABEL`
  on Fedora and Arch. The mount call is asynchronous, so a polkit password
  prompt never blocks other devices.
- Filesystems are identified by **UUID**, not by `/dev/sdX` or the udisks
  object path, which change with what else is plugged in.

## What gets listed

Every filesystem udisks reports, except: loop devices (snaps), swap, LUKS
containers, EFI system partitions, devices udisks hides (`HintIgnore`),
anything configured in `/etc/fstab`, and anything mounted outside
`/media` / `/run/media` (the system's own `/`, `/home`, ...).

## ki-mount.conf

`~/.config/ki-mount.conf`, tab separated, one line per filesystem:

    DEVICE <uuid> <auto 0|1> <removable 0|1> <last_seen> <fstype> <label> <description>

kimountd adds and updates lines; kiconf changes `auto` (and removes lines with
*Esquecer*), then sends `SIGHUP`. Both re-read the file before rewriting it.
On `SIGHUP`, kimountd also mounts newly checked disks that are connected.

## Internal disks and polkit

udisks' `org.freedesktop.udisks2.filesystem-mount-system` (non-removable
disks) asks for an administrator password by default. The Discos tab has an
opt-in checkbox that installs a rule granting it to active local users in
the `sudo`, `admin` or `wheel` groups, through `pkexec`:

- polkit < 0.106 (Ubuntu 22.04, Debian 11):
  `/etc/polkit-1/localauthority/50-local.d/50-kidesktop-mount-internal.pkla`
- polkit ≥ 0.106 (Debian 12+, Devuan 5+):
  `/etc/polkit-1/rules.d/50-kidesktop-mount-internal.rules`

"Active session" needs logind or elogind to have registered the session
(the default on Devuan desktops). Unchecking removes both files.
