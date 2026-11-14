/*
 * Copyright (C) 2026 MikuUI liuqin local build
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "install/install.h"
#include "recovery_ui/device.h"

// TWRP-style helpers for the text-menu recovery. Everything here works without
// /data decryption: files are read from / written to /tmp (RAM, use adb push /
// adb pull) or to a mountable external volume (USB OTG / SD card).

// "Apply update" -> "Choose from /tmp": browse /tmp for a .zip and install it.
InstallResult ApplyFromTmp(Device* device);

// Tools -> Backup partitions: raw-copy the current slot's boot chain plus the
// unslotted persist/modem EFS partitions, with a SHA256SUMS file.
void BackupPartitions(Device* device);

// Tools -> Flash image to partition: write a raw .img to a non-dynamic partition.
void FlashImageToPartition(Device* device);

// Tools -> Mount partitions: toggle read-only mounts of the dynamic partitions.
void MountPartitionsMenu(Device* device);

// Tools -> Export logs: recovery.log, dmesg, pstore, properties, input devices.
void ExportLogs(Device* device);

// Tools -> Switch active slot (boot HAL).
void SwitchActiveSlot(Device* device);

// Tools -> Decrypt /data: start the TEE, mount /data (metadata-decrypted, ro or
// rw) and unlock device-encrypted storage, via /system/bin/fbe_helper.
void DecryptData(Device* device, bool rw);
void UnmountData(Device* device);

// Tools -> Wipe Dalvik / ART cache (mounts /data read-write first; confirms).
void WipeDalvikCache(Device* device);

// Tools -> Terminal: line-mode root shell using the on-screen keyboard.
void TerminalMenu(Device* device);

// Tools -> Unlock internal storage: lock-screen PIN via the on-screen keyboard,
// then fbe_helper unlock-ce (Gatekeeper + synthetic password -> CE key).
void UnlockInternalStorage(Device* device);

// Home -> Files: browse, view, copy/cut/paste, rename, delete, chmod, install zips.
void FileManager(Device* device);
