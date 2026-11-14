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

#include "recovery_tools.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <poll.h>
#include <signal.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <BootControlClient.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <openssl/sha.h>
#include <volume_manager/VolumeManager.h>

#include "bootloader_message/bootloader_message.h"
#include "install/fuse_install.h"
#include "install/snapshot_utils.h"
#include "recovery_ui/ui.h"
#include "recovery_utils/roots.h"

using android::base::StringPrintf;
using android::base::unique_fd;
using android::volmgr::VolumeInfo;
using android::volmgr::VolumeManager;

namespace {

constexpr const char* kTmpDir = "/tmp";
constexpr const char* kInternalStorage = "/data/media/0";
constexpr const char* kByName = "/dev/block/by-name/";
constexpr const char* kMapper = "/dev/block/mapper/";
constexpr uint32_t kSparseMagic = 0xed26ff3a;

// Non-dynamic partitions that exist per slot on liuqin.
const std::vector<std::string> kSlottedPartitions = {
  "boot", "vendor_boot", "recovery", "dtbo", "vbmeta", "vbmeta_system",
};
// Unslotted partitions worth keeping a copy of (calibration / EFS).
const std::vector<std::string> kUnslottedPartitions = {
  "persist", "modemst1", "modemst2", "fsg", "fsc",
};
// Dynamic partitions inside super.
const std::vector<std::string> kLogicalPartitions = {
  "system", "system_ext", "product", "vendor", "odm", "vendor_dlkm",
};

size_t Menu(Device* device, const std::vector<std::string>& headers,
            const std::vector<std::string>& items, size_t initial = 0) {
  return device->GetUI()->ShowMenu(
      headers, items, initial, true,
      std::bind(&Device::HandleMenuKey, device, std::placeholders::_1, std::placeholders::_2));
}

bool Confirm(Device* device, const std::vector<std::string>& headers) {
  return Menu(device, headers, { " No", " Yes" }) == 1;
}

std::string CurrentSuffix() {
  return android::base::GetProperty("ro.boot.slot_suffix", "");
}

std::string OtherSuffix(const std::string& suffix) {
  if (suffix == "_a") return "_b";
  if (suffix == "_b") return "_a";
  return "";
}

std::string Timestamp() {
  time_t now = time(nullptr);
  struct tm tm {};
  localtime_r(&now, &tm);
  char buf[32];
  if (tm.tm_year + 1900 < 2020) {
    // The Qualcomm RTC is read-only and the time offset lives in /data, so the
    // recovery clock often starts at 1970. Use the uptime instead of a fake date.
    struct timespec ts {};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    snprintf(buf, sizeof(buf), "boot%lds", static_cast<long>(ts.tv_sec));
    return buf;
  }
  strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
  return buf;
}

std::string Basename(const std::string& path) {
  auto pos = path.find_last_of('/');
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

uint64_t BlockDeviceSize(const std::string& path) {
  unique_fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC));
  uint64_t size = 0;
  if (fd == -1 || ioctl(fd.get(), BLKGETSIZE64, &size) != 0) return 0;
  return size;
}

std::string ToHex(const uint8_t* data, size_t len) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out.push_back(kHex[data[i] >> 4]);
    out.push_back(kHex[data[i] & 0xf]);
  }
  return out;
}

// Copies |src| to |dst|. |dst_is_block| opens an existing block device instead
// of creating a file. Returns the SHA-256 of the copied bytes.
bool CopyData(const std::string& src, const std::string& dst, bool dst_is_block,
              std::string* sha256_hex, uint64_t* bytes) {
  unique_fd in(open(src.c_str(), O_RDONLY | O_CLOEXEC));
  if (in == -1) {
    PLOG(ERROR) << "Failed to open " << src;
    return false;
  }
  int flags = dst_is_block ? (O_WRONLY | O_CLOEXEC) : (O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC);
  unique_fd out(open(dst.c_str(), flags, 0644));
  if (out == -1) {
    PLOG(ERROR) << "Failed to open " << dst;
    return false;
  }

  std::vector<uint8_t> buf(1 << 20);
  SHA256_CTX ctx;
  SHA256_Init(&ctx);
  uint64_t total = 0;
  while (true) {
    ssize_t n = TEMP_FAILURE_RETRY(read(in.get(), buf.data(), buf.size()));
    if (n < 0) {
      PLOG(ERROR) << "Failed to read " << src;
      return false;
    }
    if (n == 0) break;
    if (!android::base::WriteFully(out.get(), buf.data(), n)) {
      PLOG(ERROR) << "Failed to write " << dst;
      return false;
    }
    SHA256_Update(&ctx, buf.data(), n);
    total += n;
  }
  if (fsync(out.get()) != 0) {
    PLOG(ERROR) << "Failed to fsync " << dst;
    return false;
  }
  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA256_Final(digest, &ctx);
  if (sha256_hex) *sha256_hex = ToHex(digest, sizeof(digest));
  if (bytes) *bytes = total;
  return true;
}

// A place to read files from or write files to: /tmp or an external volume.
struct Location {
  std::string path;
  std::string volume_id;  // Empty for /tmp; otherwise unmount when done.
};

std::optional<Location> ChooseLocation(Device* device, const std::string& title,
                                       const std::string& prompt) {
  auto ui = device->GetUI();
  std::vector<VolumeInfo> volumes;
  VolumeManager::Instance()->getVolumeInfo(volumes);

  std::vector<std::string> items{ "/tmp (RAM; adb push / adb pull)" };
  // Internal storage, once /data is decrypted and the CE key is unlocked
  // (Tools > Unlock internal storage). Plaintext names mean CE is unlocked.
  const bool internal = access(kInternalStorage, F_OK) == 0 &&
                        access((std::string(kInternalStorage) + "/Android").c_str(), F_OK) == 0;
  if (internal) items.push_back(std::string("Internal storage (") + kInternalStorage + ")");
  const size_t first_volume = items.size();
  std::vector<VolumeInfo> usable;
  for (auto& vol : volumes) {
    // The emulated volume is handled above via /data/media/0.
    if (!vol.mMountable || android::base::StartsWith(vol.mId, "emulated")) continue;
    items.push_back(vol.mLabel + " (" + vol.mPath + ")");
    usable.push_back(vol);
  }

  size_t chosen = Menu(device, { title, prompt }, items);
  if (chosen >= items.size()) return std::nullopt;
  if (chosen == 0) return Location{ kTmpDir, "" };
  if (internal && chosen == 1) return Location{ kInternalStorage, "" };

  VolumeInfo& vol = usable[chosen - first_volume];
  if (!VolumeManager::Instance()->volumeMount(vol.mId)) {
    ui->Print("Failed to mount %s.\n", vol.mLabel.c_str());
    return std::nullopt;
  }
  return Location{ vol.mPath, vol.mId };
}

void ReleaseLocation(const Location& loc) {
  sync();
  if (!loc.volume_id.empty()) VolumeManager::Instance()->volumeUnmount(loc.volume_id);
}

bool HasExtension(const std::string& name, const std::vector<std::string>& exts) {
  return std::any_of(exts.begin(), exts.end(), [&name](const std::string& ext) {
    return android::base::EndsWithIgnoreCase(name, ext);
  });
}

// Returns the chosen file's path, or an empty string when the user backed out.
std::string BrowseForFile(Device* device, const std::string& path,
                          const std::vector<std::string>& exts, const std::string& title) {
  std::unique_ptr<DIR, decltype(&closedir)> d(opendir(path.c_str()), closedir);
  if (!d) {
    PLOG(ERROR) << "error opening " << path;
    device->GetUI()->Print("Cannot open %s.\n", path.c_str());
    return "";
  }

  std::vector<std::string> dirs;
  std::vector<std::string> entries{ "../" };
  dirent* de;
  while ((de = readdir(d.get())) != nullptr) {
    std::string name(de->d_name);
    if (de->d_type == DT_DIR) {
      if (name == "." || name == "..") continue;
      dirs.push_back(name + "/");
    } else if (de->d_type == DT_REG && HasExtension(name, exts)) {
      entries.push_back(name);
    }
  }
  d.reset();
  std::sort(dirs.begin(), dirs.end());
  std::sort(entries.begin() + 1, entries.end());
  entries.insert(entries.end(), dirs.begin(), dirs.end());

  size_t chosen = 0;
  while (true) {
    chosen = Menu(device, { title, path }, entries, chosen);
    if (chosen >= entries.size() || chosen == 0) return "";

    const std::string& item = entries[chosen];
    std::string new_path = path + "/" + item;
    if (new_path.back() == '/') {
      new_path.pop_back();
      std::string result = BrowseForFile(device, new_path, exts, title);
      if (!result.empty()) return result;
    } else {
      return new_path;
    }
  }
}

bool IsMounted(const std::string& mount_point) {
  // Not /proc/mounts: it is a symlink and ReadFileToString() does not follow
  // symlinks by default (O_NOFOLLOW -> ELOOP).
  std::string mounts;
  if (!android::base::ReadFileToString("/proc/self/mounts", &mounts)) return false;
  for (const auto& line : android::base::Split(mounts, "\n")) {
    auto fields = android::base::Split(line, " ");
    if (fields.size() > 1 && fields[1] == mount_point) return true;
  }
  return false;
}

// Returns the device-mapper node of a dynamic partition in the current slot.
std::string LogicalPartitionPath(RecoveryUI* ui, const std::string& name) {
  if (!logical_partitions_mapped()) {
    // For Virtual A/B, set up the snapshot devices (if any) first, like
    // "Mount/unmount system" does.
    if (!CreateSnapshotPartitions()) {
      ui->Print("Virtual A/B: snapshot partitions creation failed.\n");
      return "";
    }
    map_logical_partitions();
  }
  const std::string candidates[] = { kMapper + name + CurrentSuffix(), kMapper + name };
  // Mapping is asynchronous (ueventd creates the nodes); wait up to ~3s.
  for (int i = 0; i < 300; ++i) {
    for (const auto& path : candidates) {
      if (access(path.c_str(), F_OK) == 0) return path;
    }
    usleep(10000);
  }
  return "";
}

}  // namespace

InstallResult ApplyFromTmp(Device* device) {
  auto ui = device->GetUI();
  std::string dir = kTmpDir;
  if (access((std::string(kInternalStorage) + "/Android").c_str(), F_OK) == 0) {
    size_t c = Menu(device, { "Apply update", "Where is the package?" },
                    { "/tmp (adb push)", std::string("Internal storage (") + kInternalStorage + ")" });
    if (c > 1) return INSTALL_NONE;
    if (c == 1) dir = kInternalStorage;
  }
  std::string path = BrowseForFile(device, dir, { ".zip" }, "Choose a package to install:");
  if (path.empty()) return INSTALL_NONE;

  ui->Print("\n-- Install %s ...\n", path.c_str());
  // Like installing from storage: reboot back into recovery if interrupted.
  std::string err;
  if (!update_bootloader_message({}, &err)) {
    LOG(ERROR) << "Failed to set BCB message: " << err;
  }
  return InstallWithFuseFromPath(path, device);
}

void BackupPartitions(Device* device) {
  auto ui = device->GetUI();
  const std::string suffix = CurrentSuffix();
  auto loc = ChooseLocation(device, "Backup partitions", "Choose where to save the backup:");
  if (!loc) return;

  std::string dir = loc->path + "/recovery-backup-" + Timestamp() + suffix;
  if (mkdir(dir.c_str(), 0755) != 0) {
    ui->Print("Failed to create %s: %s\n", dir.c_str(), strerror(errno));
    ReleaseLocation(*loc);
    return;
  }

  std::vector<std::string> names;
  for (const auto& p : kSlottedPartitions) names.push_back(p + suffix);
  for (const auto& p : kUnslottedPartitions) names.push_back(p);

  ui->Print("\n-- Backing up to %s\n", dir.c_str());
  std::string sums;
  int failed = 0;
  for (const auto& name : names) {
    std::string src = kByName + name;
    if (access(src.c_str(), F_OK) != 0) {
      ui->Print("  %s: not found, skipped\n", name.c_str());
      continue;
    }
    std::string file = name + ".img";
    std::string hex;
    uint64_t bytes = 0;
    if (CopyData(src, dir + "/" + file, false, &hex, &bytes)) {
      ui->Print("  %s: %llu KiB\n", name.c_str(), static_cast<unsigned long long>(bytes / 1024));
      sums += hex + "  " + file + "\n";
    } else {
      ui->Print("  %s: FAILED (%s)\n", name.c_str(), strerror(errno));
      ++failed;
    }
  }
  android::base::WriteStringToFile(sums, dir + "/SHA256SUMS");
  ReleaseLocation(*loc);

  ui->Print("Backup %s: %s\n", failed ? "finished WITH ERRORS" : "done", dir.c_str());
  if (loc->volume_id.empty()) {
    ui->Print("Copy it off the device: adb pull %s\n", dir.c_str());
  }
}

void FlashImageToPartition(Device* device) {
  auto ui = device->GetUI();
  auto loc = ChooseLocation(device, "Flash image to partition", "Where is the image?");
  if (!loc) return;

  std::string image = BrowseForFile(device, loc->path, { ".img", ".bin" }, "Choose an image:");
  if (image.empty()) {
    ReleaseLocation(*loc);
    return;
  }

  const std::string cur = CurrentSuffix();
  const std::string other = OtherSuffix(cur);
  std::vector<std::string> targets;
  std::vector<std::string> items;
  for (const auto& p : kSlottedPartitions) {
    targets.push_back(p + cur);
    items.push_back(p + cur + "  (current slot)");
  }
  if (!other.empty()) {
    for (const auto& p : kSlottedPartitions) {
      targets.push_back(p + other);
      items.push_back(p + other + "  (other slot)");
    }
  }
  for (const auto& p : kUnslottedPartitions) {
    targets.push_back(p);
    items.push_back(p + "  (restore your own backup only!)");
  }

  size_t chosen = Menu(device, { "Flash " + Basename(image), "Choose the target partition:" }, items);
  if (chosen >= targets.size()) {
    ReleaseLocation(*loc);
    return;
  }
  const std::string& target = targets[chosen];
  const std::string blk = kByName + target;

  struct stat st {};
  if (stat(image.c_str(), &st) != 0) {
    ui->Print("Cannot stat %s: %s\n", image.c_str(), strerror(errno));
    ReleaseLocation(*loc);
    return;
  }
  uint64_t part_size = BlockDeviceSize(blk);
  if (part_size == 0) {
    ui->Print("Cannot open partition %s.\n", target.c_str());
    ReleaseLocation(*loc);
    return;
  }
  if (static_cast<uint64_t>(st.st_size) > part_size) {
    ui->Print("Image (%lld bytes) is larger than %s (%llu bytes). Aborted.\n",
              static_cast<long long>(st.st_size), target.c_str(),
              static_cast<unsigned long long>(part_size));
    ReleaseLocation(*loc);
    return;
  }
  uint32_t magic = 0;
  {
    unique_fd fd(open(image.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd == -1 || !android::base::ReadFully(fd.get(), &magic, sizeof(magic))) magic = 0;
  }
  if (magic == kSparseMagic) {
    ui->Print("%s is a sparse image; only raw images are supported. Aborted.\n",
              Basename(image).c_str());
    ReleaseLocation(*loc);
    return;
  }

  if (!Confirm(device, { "Flash " + Basename(image) + " to " + target + "?",
                         StringPrintf("%lld bytes into a %llu byte partition.",
                                      static_cast<long long>(st.st_size),
                                      static_cast<unsigned long long>(part_size)),
                         "THIS CAN NOT BE UNDONE!" })) {
    ReleaseLocation(*loc);
    return;
  }

  ui->Print("\n-- Flashing %s to %s ...\n", Basename(image).c_str(), target.c_str());
  std::string hex;
  uint64_t bytes = 0;
  bool ok = CopyData(image, blk, true, &hex, &bytes);
  ReleaseLocation(*loc);
  if (ok) {
    ui->Print("Done: %llu bytes, sha256 %s\n", static_cast<unsigned long long>(bytes), hex.c_str());
  } else {
    ui->Print("FAILED to flash %s: %s\n", target.c_str(), strerror(errno));
  }
}

void MountPartitionsMenu(Device* device) {
  auto ui = device->GetUI();
  size_t selected = 0;
  while (true) {
    std::vector<std::string> items;
    for (const auto& p : kLogicalPartitions) {
      items.push_back((IsMounted("/mnt/" + p) ? "[mounted] " : "[       ] ") + p + "  ->  /mnt/" + p);
    }
    selected = Menu(device, { "Mount partitions (read-only)", "Select to mount / unmount:" }, items,
                    selected);
    if (selected >= items.size()) return;

    const std::string& name = kLogicalPartitions[selected];
    const std::string mount_point = "/mnt/" + name;
    if (IsMounted(mount_point)) {
      if (umount(mount_point.c_str()) == 0) {
        ui->Print("Unmounted %s.\n", mount_point.c_str());
      } else {
        ui->Print("Failed to unmount %s: %s\n", mount_point.c_str(), strerror(errno));
      }
      continue;
    }

    std::string dev = LogicalPartitionPath(ui, name);
    if (dev.empty()) {
      ui->Print("%s: device not found.\n", name.c_str());
      continue;
    }
    mkdir(mount_point.c_str(), 0755);
    bool mounted = false;
    for (const char* fs : { "erofs", "ext4" }) {
      if (mount(dev.c_str(), mount_point.c_str(), fs, MS_RDONLY, nullptr) == 0) {
        ui->Print("Mounted %s (%s) at %s.\n", name.c_str(), fs, mount_point.c_str());
        mounted = true;
        break;
      }
    }
    if (!mounted) ui->Print("Failed to mount %s: %s\n", dev.c_str(), strerror(errno));
  }
}

void ExportLogs(Device* device) {
  auto ui = device->GetUI();
  auto loc = ChooseLocation(device, "Export logs", "Choose where to save the logs:");
  if (!loc) return;

  std::string dir = loc->path + "/recovery-logs-" + Timestamp();
  if (mkdir(dir.c_str(), 0755) != 0) {
    ui->Print("Failed to create %s: %s\n", dir.c_str(), strerror(errno));
    ReleaseLocation(*loc);
    return;
  }

  std::vector<std::pair<std::string, std::string>> files = {
    { "/tmp/recovery.log", "recovery.log" },
    { "/cache/recovery/last_log", "last_log" },
    { "/cache/recovery/last_kmsg", "last_kmsg" },
    { "/proc/bus/input/devices", "input_devices.txt" },
    { "/proc/self/mounts", "mounts.txt" },
    { "/proc/cmdline", "cmdline.txt" },
  };
  if (std::unique_ptr<DIR, decltype(&closedir)> d(opendir("/sys/fs/pstore"), closedir); d) {
    dirent* de;
    while ((de = readdir(d.get())) != nullptr) {
      if (de->d_type != DT_REG) continue;
      files.emplace_back(std::string("/sys/fs/pstore/") + de->d_name,
                         std::string("pstore-") + de->d_name);
    }
  }

  ui->Print("\n-- Exporting logs to %s\n", dir.c_str());
  for (const auto& [src, name] : files) {
    std::string content;
    if (!android::base::ReadFileToString(src, &content)) continue;
    if (android::base::WriteStringToFile(content, dir + "/" + name)) {
      ui->Print("  %s\n", name.c_str());
    }
  }

  int len = klogctl(KLOG_SIZE_BUFFER, nullptr, 0);
  if (len > 0) {
    std::string kmsg(len, '\0');
    int n = klogctl(KLOG_READ_ALL, kmsg.data(), len);
    if (n > 0) {
      kmsg.resize(n);
      if (android::base::WriteStringToFile(kmsg, dir + "/dmesg.txt")) ui->Print("  dmesg.txt\n");
    }
  }

  std::string props;
  __system_property_foreach(
      [](const prop_info* pi, void* cookie) {
        __system_property_read_callback(
            pi,
            [](void* c, const char* name, const char* value, uint32_t) {
              static_cast<std::string*>(c)->append(StringPrintf("[%s]: [%s]\n", name, value));
            },
            cookie);
      },
      &props);
  if (android::base::WriteStringToFile(props, dir + "/getprop.txt")) ui->Print("  getprop.txt\n");

  ReleaseLocation(*loc);
  ui->Print("Logs saved to %s\n", dir.c_str());
  if (loc->volume_id.empty()) ui->Print("Copy them off the device: adb pull %s\n", dir.c_str());
}

void SwitchActiveSlot(Device* device) {
  auto ui = device->GetUI();
  auto boot = android::hal::BootControlClient::WaitForService();
  if (!boot) {
    ui->Print("Boot control HAL is not available.\n");
    return;
  }
  int32_t num_slots = boot->GetNumSlots();
  if (num_slots < 2) {
    ui->Print("This device has no A/B slots.\n");
    return;
  }
  int32_t current = boot->GetCurrentSlot();
  int32_t active = boot->GetActiveBootSlot();

  std::vector<std::string> items;
  for (int32_t slot = 0; slot < num_slots; ++slot) {
    std::string label = "Slot " + boot->GetSuffix(slot);
    if (slot == current) label += "  (running now)";
    if (slot == active) label += "  (active for next boot)";
    items.push_back(label);
  }
  size_t chosen = Menu(device,
                       { "Switch active slot",
                         "The next boot uses the active slot. Only pick a slot that has a",
                         "complete, matching system (and re-patch KernelSU there if needed)." },
                       items, static_cast<size_t>(std::max(active, 0)));
  if (chosen >= items.size()) return;
  int32_t slot = static_cast<int32_t>(chosen);
  if (slot == active) {
    ui->Print("Slot %s is already active.\n", boot->GetSuffix(slot).c_str());
    return;
  }
  if (!Confirm(device, { "Make slot " + boot->GetSuffix(slot) + " active for the next boot?" })) {
    return;
  }
  auto result = boot->SetActiveBootSlot(slot);
  if (result.IsOk()) {
    ui->Print("Slot %s is now active.\n", boot->GetSuffix(slot).c_str());
  } else {
    ui->Print("Failed to set slot %s active: %s\n", boot->GetSuffix(slot).c_str(),
              result.errMsg.c_str());
  }
}

// ---------------------------------------------------------------------------
// Terminal and /data decryption (liuqin)
// ---------------------------------------------------------------------------
namespace {

// Runs argv with stdout+stderr captured line by line; kills the whole process
// group after |timeout_s| seconds. Returns the exit status (or -1).
int RunCapture(const std::vector<std::string>& argv, int timeout_s,
               const std::function<void(const std::string&)>& on_line,
               const std::string* input = nullptr) {
  int pipefd[2];
  if (pipe2(pipefd, O_CLOEXEC) != 0) return -1;
  int inpipe[2] = { -1, -1 };
  if (input && pipe2(inpipe, O_CLOEXEC) != 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    if (input) {
      close(inpipe[0]);
      close(inpipe[1]);
    }
    return -1;
  }
  if (pid == 0) {
    setpgid(0, 0);
    if (input) {
      dup2(inpipe[0], 0);
    } else {
      int devnull = open("/dev/null", O_RDONLY);
      if (devnull >= 0) dup2(devnull, 0);
    }
    dup2(pipefd[1], 1);
    dup2(pipefd[1], 2);
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    execv(args[0], args.data());
    _exit(127);
  }
  close(pipefd[1]);
  if (input) {
    // Secrets go through a pipe, never through argv (visible in /proc).
    close(inpipe[0]);
    android::base::WriteFully(inpipe[1], input->data(), input->size());
    close(inpipe[1]);
  }
  std::string pending;
  bool timed_out = false;
  const time_t deadline = time(nullptr) + timeout_s;
  while (true) {
    struct pollfd pfd { pipefd[0], POLLIN, 0 };
    int left = static_cast<int>(deadline - time(nullptr));
    if (left <= 0) {
      timed_out = true;
      break;
    }
    int r = poll(&pfd, 1, std::min(left, 1) * 1000);
    if (r < 0 && errno == EINTR) continue;
    if (r == 0) continue;
    char buf[4096];
    ssize_t n = read(pipefd[0], buf, sizeof(buf));
    if (n <= 0) break;
    pending.append(buf, n);
    size_t pos;
    while ((pos = pending.find('\n')) != std::string::npos) {
      on_line(pending.substr(0, pos));
      pending.erase(0, pos + 1);
    }
  }
  if (!pending.empty()) on_line(pending);
  close(pipefd[0]);
  if (timed_out) {
    kill(-pid, SIGKILL);
    on_line("[timed out after " + std::to_string(timeout_s) + "s, killed]");
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string ShellQuote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\''";
    else out += c;
  }
  return out + "'";
}

std::string FbeHelper() {
  return access("/system/bin/fbe_helper", X_OK) == 0 ? "/system/bin/fbe_helper" : "/tmp/fbe_helper";
}

// Runs fbe_helper <args>, echoing its output to the recovery screen/log.
bool RunFbe(RecoveryUI* ui, const std::vector<std::string>& args) {
  std::vector<std::string> argv{ FbeHelper() };
  argv.insert(argv.end(), args.begin(), args.end());
  int rc = RunCapture(argv, 120, [ui](const std::string& l) { ui->Print("  %s\n", l.c_str()); });
  return rc == 0;
}

bool DataMountedRw() {
  std::string mounts;
  android::base::ReadFileToString("/proc/self/mounts", &mounts);
  for (const auto& line : android::base::Split(mounts, "\n")) {
    auto f = android::base::Split(line, " ");
    if (f.size() > 3 && f[1] == "/data") return android::base::StartsWith(f[3], "rw");
  }
  return false;
}

bool DecryptAndMount(RecoveryUI* ui, bool rw) {
  if (IsMounted("/data") && (!rw || DataMountedRw())) {
    ui->Print("/data is already mounted%s.\n", DataMountedRw() ? " read-write" : " read-only");
    return true;
  }
  if (IsMounted("/data") && rw) {
    ui->Print("/data is mounted read-only; unmounting to remount read-write...\n");
    if (!RunFbe(ui, { "umount-data" })) return false;
  }
  // fbe_helper needs the dynamic partitions mapped (system/vendor/odm).
  if (LogicalPartitionPath(ui, "vendor").empty()) {
    ui->Print("Cannot map dynamic partitions.\n");
    return false;
  }
  ui->Print("\n-- Starting TEE (qseecomd + KeyMint)...\n");
  if (!RunFbe(ui, { "start-tee" })) return false;
  ui->Print("-- Mounting /data (%s)...\n", rw ? "read-write" : "read-only");
  if (!RunFbe(ui, rw ? std::vector<std::string>{ "mount-data", "rw" }
                     : std::vector<std::string>{ "mount-data" })) {
    return false;
  }
  ui->Print("-- Unlocking device-encrypted (DE) storage...\n");
  RunFbe(ui, { "unlock-de" });
  ui->Print("Note: internal storage (/sdcard) stays encrypted until the lock-screen\n"
            "credential is entered (not supported yet).\n");
  return true;
}

}  // namespace

void DecryptData(Device* device, bool rw) {
  DecryptAndMount(device->GetUI(), rw);
}

void UnmountData(Device* device) {
  RunFbe(device->GetUI(), { "umount-data" });
}

void WipeDalvikCache(Device* device) {
  auto ui = device->GetUI();
  if (!Confirm(device, { "Wipe Dalvik / ART cache?",
                         "Deletes /data/dalvik-cache/*. Android rebuilds it on the next",
                         "boot (the first boot takes longer). Apps and data are kept." })) {
    return;
  }
  if (!DecryptAndMount(ui, true)) {
    ui->Print("Wipe Dalvik cancelled: /data could not be mounted read-write.\n");
    return;
  }
  ui->Print("-- Wiping Dalvik / ART cache...\n");
  RunFbe(ui, { "wipe-dalvik" });
}

void TerminalMenu(Device* device) {
  auto ui = device->GetUI();
  std::vector<std::string> lines{
    "Recovery terminal (root, /system/bin/sh).",
    "Each command runs to completion (60 s limit); interactive programs are not supported.",
    "Type 'exit' or tap Exit to leave. Swipe or use the volume keys to scroll.",
    "",
  };
  std::string cwd = "/";
  auto add = [&lines](const std::string& l) {
    lines.push_back(l);
    if (lines.size() > 3000) lines.erase(lines.begin(), lines.begin() + 500);
  };
  while (true) {
    auto input = ui->ReadLine(lines, cwd + " # ", false);
    if (!input) return;
    std::string cmd = android::base::Trim(*input);
    add(cwd + " # " + cmd);
    if (cmd.empty()) continue;
    if (cmd == "exit") return;
    if (cmd == "clear") {
      lines.clear();
      continue;
    }
    if (cmd == "cd" || android::base::StartsWith(cmd, "cd ")) {
      std::string target = cmd == "cd" ? "/" : android::base::Trim(cmd.substr(3));
      std::string resolved;
      int rc = RunCapture({ "/system/bin/sh", "-c",
                            "cd " + ShellQuote(cwd) + " && cd " + target + " && pwd" },
                          10, [&](const std::string& l) {
                            if (!l.empty() && l[0] == '/') resolved = l;
                            else add(l);
                          });
      if (rc == 0 && !resolved.empty()) cwd = resolved;
      continue;
    }
    int rc = RunCapture({ "/system/bin/sh", "-c", "cd " + ShellQuote(cwd) + " && " + cmd }, 60,
                        add);
    if (rc != 0) add("[exit " + std::to_string(rc) + "]");
  }
}

void UnlockInternalStorage(Device* device) {
  auto ui = device->GetUI();
  if (!IsMounted("/data")) {
    // Prefer read-write so internal storage is usable for backups/copies; fall
    // back to read-only (e.g. while an OTA checkpoint is pending).
    if (!DecryptAndMount(ui, true)) {
      ui->Print("Read-write mount not possible; trying read-only...\n");
      if (!DecryptAndMount(ui, false)) return;
    }
  }
  if (access((std::string(kInternalStorage) + "/Android").c_str(), F_OK) == 0) {
    ui->Print("Internal storage is already unlocked (%s).\n", kInternalStorage);
    return;
  }
  auto pin = ui->ReadLine(
      { "Unlock internal storage (user 0)",
        "",
        "Enter the lock-screen PIN or password.",
        "It is checked by the TEE (Gatekeeper): a wrong entry counts as a failed",
        "lock-screen attempt, and repeated failures add a waiting time.",
        "Tap Exit to cancel." },
      "PIN: ", true);
  if (!pin || pin->empty()) return;
  std::string input = *pin + "\n";
  std::fill(pin->begin(), pin->end(), '\0');
  ui->Print("\n-- Unlocking internal storage (CE)...\n");
  RunCapture({ FbeHelper(), "unlock-ce", "0" }, 120,
             [ui](const std::string& l) { ui->Print("  %s\n", l.c_str()); }, &input);
  std::fill(input.begin(), input.end(), '\0');
}
