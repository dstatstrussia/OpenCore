NP300 / Phoenix BIOS boot fix
=============================

Helper scripts for machines where the firmware supports both UEFI and legacy
boot, but selecting the USB drive in the boot menu silently starts the
installed OS instead of OpenCore.

Typical target: Samsung NP300 (Sandy Bridge) with Phoenix SecureCore, where

- the UEFI entry may be missing from the one-time boot menu, and
- the legacy entry has no MBR boot code, so the firmware falls through to the
  next boot option (usually Windows).

What it does
------------

`install_legacy_boot.sh`

Installs the OpenDuet legacy boot chain on a GPT disk so the legacy/CSM entry
boots OpenCore as well:

1. `boot0` into the MBR boot code area (partition table untouched).
2. `boot1f32` into the EFI system partition VBR, keeping the FAT32 BPB.
3. `bootX64` at the root of the EFI system partition as `boot`.

`boot0` is GPT aware: it finds the EFI system partition by its GPT type GUID,
so no hybrid MBR is required and the UEFI boot path keeps working.

`patch_config.py`

Adjusts a config.plist for diagnosis and for avoiding a silent fallback to the
installed OS:

- `Misc/Boot/Timeout = 0` - never auto-boot the default entry.
- `Misc/Boot/PickerMode = Builtin` - text picker, reliable on legacy GOP.
- `Misc/Debug/Target = 67` - console + data hub + file logging, so OpenCore
  writes `OpenCore-*.txt` to the EFI partition.

Usage
-----

```sh
# 1. Legacy boot chain (macOS, needs root)
sudo ./install_legacy_boot.sh disk6

# 2. Config hardening, keeps a .bak next to the original
./patch_config.py /Volumes/EFI/EFI/OC/config.plist
```

Diagnostics after a failed boot
-------------------------------

The fork writes breadcrumbs directly to the EFI partition root:

| File            | Written by          | Meaning                                              |
|-----------------|---------------------|------------------------------------------------------|
| `bstage.txt`    | `BOOTx64.efi`       | Launcher reached the volume / failed to load OpenCore |
| `ocstage.txt`   | `OpenCore.efi`      | Last early init stage before crash/reboot             |
| `dbtstage.txt`  | `OpenDbvX64Dxe.efi` | DBT driver init and installer scan progress           |
| `OpenCore-*.txt`| OpenCore log        | Full log when `Misc/Debug/Target` has bit `0x40`      |

If none of them appear, the firmware never executed `\EFI\BOOT\BOOTX64.EFI` and
the problem is boot media selection, not OpenCore itself.
