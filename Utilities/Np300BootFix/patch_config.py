#!/usr/bin/env python3
import plistlib
import sys
import os
import subprocess
from datetime import datetime

def backup_config(path):
    bak_path = f"{path}.bak-{datetime.now().strftime('%Y%m%d-%H%M%S')}"
    subprocess.run(['cp', path, bak_path], check=True)
    print(f"Backup created: {bak_path}")
    return bak_path

def patch_config(path):
    try:
        with open(path, 'rb') as f:
            d = plistlib.load(f)
    except Exception as e:
        print(f"Error loading plist: {e}")
        sys.exit(1)

    # 1. Misc/Boot/Timeout = 0 (Don't auto-boot default)
    if 'Misc' in d and 'Boot' in d['Misc']:
        d['Misc']['Boot']['Timeout'] = 0
        print("[+] Set Misc/Boot/Timeout = 0")

    # 2. Misc/Boot/PickerMode = Builtin (Reliable for legacy GOP)
    if 'Misc' in d and 'Boot' in d['Misc']:
        d['Misc']['Boot']['PickerMode'] = 'Builtin'
        print("[+] Set Misc/Boot/PickerMode = Builtin")

    # 3. Misc/Debug/Target = 67 (Full debug logging)
    # 0x43 = 67 (Console + File Logging)
    if 'Misc' in d and 'Debug' in d['Misc']:
        d['Misc']['Debug']['Target'] = 67
        print("[+] Set Misc/Debug/Target = 67 (Console + File)")

    # Save changes
    backup_config(path)
    with open(path, 'wb') as f:
        plistlib.dump(d, f)
    print("[+] Config patched successfully.")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 patch_config.py <path_to_config.plist>")
        sys.exit(1)
    
    target_path = sys.argv[1]
    if not os.path.exists(target_path):
        print(f"Error: File {target_path} not found.")
        sys.exit(1)
        
    patch_config(target_path)
