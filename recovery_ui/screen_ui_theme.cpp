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

// liuqin recovery theme: status bar, home tiles, card lists and a log card,
// drawn with minui primitives. Assets (fonts, icons, logo) come from
// /res/images/{font_title,font_small,ui_*}.png; anything missing falls back to
// the stock text menu. Also provides /tmp/.ui_shot -> /tmp/ui_shot.bmp.

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <android-base/file.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <minui/minui.h>

#include "recovery_ui/device.h"
#include "recovery_ui/screen_ui.h"

namespace {

struct Rgb {
  uint8_t r, g, b;
};
constexpr Rgb kBg{ 13, 20, 23 };
constexpr Rgb kSurface{ 22, 32, 36 };
constexpr Rgb kSurfaceSel{ 24, 58, 58 };
constexpr Rgb kAccent{ 57, 197, 187 };
constexpr Rgb kOnAccent{ 4, 52, 44 };
constexpr Rgb kText{ 232, 246, 244 };
constexpr Rgb kText2{ 127, 184, 178 };
constexpr Rgb kMuted{ 93, 122, 119 };
constexpr Rgb kLogBg{ 9, 14, 16 };
constexpr Rgb kLogText{ 184, 199, 197 };

constexpr int kMargin = 64;

struct ItemStyle {
  const char* icon;
  const char* subtitle;
};

// Home tiles and card lists: icon + subtitle by item title.
const std::map<std::string, ItemStyle>& Styles() {
  static const std::map<std::string, ItemStyle> m = {
    { "Install", { "system_update", "Zip from ADB, /tmp or storage" } },
    { "Wipe", { "delete", "Format data, Dalvik cache" } },
    { "Backup", { "settings_backup", "Partitions and images" } },
    { "Data", { "lock_open", "Decrypt and unlock storage" } },
    { "Terminal", { "doc_codes", "Root shell" } },
    { "Tools", { "settings", "Mount, logs, slots" } },
    { "Advanced", { "settings_development", "Fastboot, ADB, tests" } },
    { "Power off", { "lock_power_off", "Shut the tablet down" } },
    { "Files", { "doc_folder", "Browse, copy, rename, delete" } },
    { "Folder actions", { "settings", nullptr } },
    { "New folder", { "doc_folder", "Create a folder here" } },
    { "Paste here", { "restore", nullptr } },
    { "Copy", { "doc_generic", "Then paste it in another folder" } },
    { "Cut (move)", { "swap_horiz", "Then paste it in another folder" } },
    { "Rename", { "article", "Type a new name" } },
    { "Delete", { "delete", "Cannot be undone" } },
    { "Permissions", { "lock", "chmod 644, 755, 600, 770, 777" } },
    { "Info", { "settings", "Size, owner, SELinux label, sha256" } },
    { "View as text", { "article", nullptr } },
    { "Install this zip", { "system_update", "Install like an OTA package" } },
    { "Apply from ADB", { "usb", "adb sideload <package>" } },
    { "Choose from /tmp or internal storage", { "doc_compressed", "Pick a .zip to install" } },
    { "Format data/factory reset", { "delete", "Erases all user data" } },
    { "Format cache partition", { "delete", nullptr } },
    { "Format system partition", { "delete", "Removes the OS from this slot" } },
    { "Wipe Dalvik / ART cache", { "delete", "Rebuilt on the next boot" } },
    { "Backup partitions", { "settings_backup", "boot, vendor_boot, persist, EFS" } },
    { "Flash image to partition", { "restore", "Raw .img into one partition" } },
    { "Unlock internal storage (PIN)", { "lock_open", "Lock-screen PIN, full access" } },
    { "Decrypt /data (read-only)", { "lock", "Metadata and DE keys, no writes" } },
    { "Mount /data read-write", { "storage", "Needed for wipes and copies" } },
    { "Unmount /data", { "lock", nullptr } },
    { "Mount partitions", { "storage", "Read-only system, vendor, odm" } },
    { "Export logs", { "article", "recovery.log, dmesg, props" } },
    { "Switch active slot", { "swap_horiz", "Slot used for the next boot" } },
    { "Enter fastboot", { "restart", nullptr } },
    { "Reboot to bootloader", { "restart", nullptr } },
    { "Reboot to recovery", { "restart", nullptr } },
    { "Mount/unmount system", { "storage", nullptr } },
    { "View recovery logs", { "article", nullptr } },
    { "Enable ADB", { "usb", nullptr } },
    { "Run graphics test", { "settings", nullptr } },
    { "Run locale test", { "settings", nullptr } },
    { "Enter rescue", { "usb", nullptr } },
  };
  return m;
}

ItemStyle StyleFor(const std::string& title) {
  auto it = Styles().find(title);
  if (it != Styles().end()) return it->second;
  if (android::base::EndsWith(title, "/")) return { "doc_folder", nullptr };
  if (android::base::EndsWithIgnoreCase(title, ".zip")) return { "doc_compressed", nullptr };
  if (android::base::EndsWithIgnoreCase(title, ".img") ||
      android::base::EndsWithIgnoreCase(title, ".bin") ||
      android::base::EndsWithIgnoreCase(title, ".map")) {
    return { "doc_generic", nullptr };
  }
  if (android::base::StartsWith(title, "/tmp") || android::base::StartsWith(title, "Internal storage")) {
    return { "storage", nullptr };
  }
  if (android::base::StartsWith(title, "Choose from ")) return { "sd_card", nullptr };
  return { nullptr, nullptr };
}

std::string Trim(const std::string& s) { return android::base::Trim(s); }

std::string ReadSys(const char* path) {
  std::string s;
  android::base::ReadFileToString(path, &s);
  return Trim(s);
}

bool Mounted(const char* mp, bool* rw) {
  std::string mounts;
  android::base::ReadFileToString("/proc/self/mounts", &mounts);
  for (const auto& line : android::base::Split(mounts, "\n")) {
    auto f = android::base::Split(line, " ");
    if (f.size() > 3 && f[1] == mp) {
      if (rw) *rw = android::base::StartsWith(f[3], "rw");
      return true;
    }
  }
  return false;
}

void Color(Rgb c, uint8_t a = 255) { gr_color(c.r, c.g, c.b, a); }

}  // namespace

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------
void ScreenRecoveryUI::theme_fill_round(int x, int y, int w, int h, int r, Rgb3 c) const {
  if (w <= 0 || h <= 0) return;
  r = std::min({ r, w / 2, h / 2 });
  for (int dy = 0; dy < h; ++dy) {
    double cy = -1;
    if (dy < r) cy = r - dy - 0.5;
    else if (dy >= h - r) cy = dy - (h - r) + 0.5;
    if (cy < 0) {
      gr_color(c.r, c.g, c.b, 255);
      gr_fill(x, y + dy, x + w, y + dy + 1);
      continue;
    }
    double inset = r - std::sqrt(std::max(0.0, double(r) * r - cy * cy));
    int full = static_cast<int>(std::ceil(inset));
    double cover = full - inset;  // coverage of the partial edge pixel
    gr_color(c.r, c.g, c.b, 255);
    gr_fill(x + full, y + dy, x + w - full, y + dy + 1);
    if (full > 0 && cover > 0.02) {
      gr_color(c.r, c.g, c.b, static_cast<uint8_t>(cover * 255));
      gr_fill(x + full - 1, y + dy, x + full, y + dy + 1);
      gr_fill(x + w - full, y + dy, x + w - full + 1, y + dy + 1);
    }
  }
}

void ScreenRecoveryUI::theme_card(int x, int y, int w, int h, int r, Rgb3 fill, bool outline) const {
  if (outline) {
    theme_fill_round(x, y, w, h, r, { kAccent.r, kAccent.g, kAccent.b });
    theme_fill_round(x + 5, y + 5, w - 10, h - 10, std::max(0, r - 5), fill);
  } else {
    theme_fill_round(x, y, w, h, r, fill);
  }
}

void ScreenRecoveryUI::theme_text(const GRFont* font, int x, int y, const std::string& s, Rgb3 c,
                                  bool bold, int max_width) const {
  if (!font || s.empty()) return;
  int cw = 0, ch = 0;
  gr_font_size(font, &cw, &ch);
  std::string t = s;
  if (max_width > 0 && cw > 0 && static_cast<int>(t.size()) * cw > max_width) {
    size_t keep = std::max(1, max_width / cw - 1);
    t = t.substr(0, keep) + "~";
  }
  gr_color(c.r, c.g, c.b, 255);
  gr_text(font, x, y, t.c_str(), bold);
}

const GRSurface* ScreenRecoveryUI::theme_icon(const std::string& name, int size) {
  if (name.empty()) return nullptr;
  std::string key = "ui_" + name + "_" + std::to_string(size);
  auto it = theme_icons_.find(key);
  if (it != theme_icons_.end()) return it->second.get();
  GRSurface* surface = nullptr;
  if (res_create_alpha_surface(key.c_str(), &surface) < 0) surface = nullptr;
  theme_icons_[key].reset(surface);
  return surface;
}

void ScreenRecoveryUI::theme_draw_icon(const std::string& name, int size, int x, int y, Rgb3 c) {
  const GRSurface* icon = theme_icon(name, size);
  if (!icon) return;
  gr_color(c.r, c.g, c.b, 255);
  gr_texticon(x, y, icon);
}

// ---------------------------------------------------------------------------
// Init / screenshots
// ---------------------------------------------------------------------------
void ScreenRecoveryUI::theme_init() {
  GRFont* f = nullptr;
  if (gr_init_font("font_title", &f) == 0) font_title_ = f;
  f = nullptr;
  if (gr_init_font("font_small", &f) == 0) font_small_ = f;
  GRSurface* logo = nullptr;
  if (res_create_display_surface("ui_banner", &logo) == 0 ||
      res_create_display_surface("ui_logo", &logo) == 0) {
    theme_logo_.reset(logo);
  }
  GRSurface* chibi = nullptr;
  if (res_create_display_surface("ui_chibi", &chibi) == 0) theme_chibi_.reset(chibi);
  GRSurface* head = nullptr;
  if (res_create_display_surface("ui_head", &head) == 0) theme_head_.reset(head);
  theme_ready_ = font_title_ && font_small_ && gr_menu_font();
  screenshot_thread_ = std::thread(&ScreenRecoveryUI::ScreenshotLoop, this);
  screenshot_thread_.detach();
}

// Debug aid: `touch /tmp/.ui_shot` -> current frame at half size in /tmp/ui_shot.bmp.
void ScreenRecoveryUI::ScreenshotLoop() {
  while (true) {
    usleep(300 * 1000);
    if (access("/tmp/.ui_shot", F_OK) != 0) continue;
    unlink("/tmp/.ui_shot");
    std::lock_guard<std::mutex> lg(updateMutex);
    draw_screen_locked();
    const GRSurface* s = gr_draw_surface();
    if (!s) continue;
    const int w = s->width / 2, h = s->height / 2;
    const int row = (w * 3 + 3) & ~3;
    std::string bmp(54 + row * h, '\0');
    auto put32 = [&bmp](size_t off, uint32_t v) {
      for (int i = 0; i < 4; ++i) bmp[off + i] = static_cast<char>(v >> (8 * i));
    };
    bmp[0] = 'B'; bmp[1] = 'M';
    put32(2, bmp.size()); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h);
    bmp[26] = 1; bmp[28] = 24; put32(34, row * h);
    const bool bgr = gr_pixel_format() == PixelFormat::BGRA ||
                     gr_pixel_format() == PixelFormat::BGRX;
    const uint8_t* data = s->data();
    for (int y = 0; y < h; ++y) {
      const uint8_t* src = data + static_cast<size_t>(y * 2) * s->row_bytes;
      char* dst = &bmp[54 + static_cast<size_t>(h - 1 - y) * row];
      for (int x = 0; x < w; ++x) {
        const uint8_t* p = src + x * 2 * s->pixel_bytes;
        dst[x * 3 + 0] = static_cast<char>(bgr ? p[0] : p[2]);
        dst[x * 3 + 1] = static_cast<char>(p[1]);
        dst[x * 3 + 2] = static_cast<char>(bgr ? p[2] : p[0]);
      }
    }
    android::base::WriteStringToFile(bmp, "/tmp/ui_shot.bmp");
  }
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
int ScreenRecoveryUI::theme_status_bar(int y) {
  const int W = ScreenWidth();
  int cw = 0, ch = 0;
  gr_font_size(font_small_, &cw, &ch);
  std::string slot = android::base::GetProperty("ro.boot.slot_suffix", "");
  if (!slot.empty() && slot[0] == '_') slot = slot.substr(1);
  std::string left = android::base::GetProperty("ro.product.device", "liuqin");
  if (!slot.empty()) left += "  slot " + slot;
  theme_text(font_small_, kMargin, y, left, { kText2.r, kText2.g, kText2.b });

  bool rw = false;
  std::string data;
  if (access("/data/media/0/Android", F_OK) == 0) data = "storage unlocked";
  else if (Mounted("/data", &rw)) data = rw ? "data rw" : "data ro";
  else data = "data locked";
  std::string right = data;
  std::string cap = ReadSys("/sys/class/power_supply/battery/capacity");
  if (!cap.empty() && cap != "0") {
    right += "  " + cap + "%";
    if (ReadSys("/sys/class/power_supply/battery/status") == "Charging") right += "+";
  }
  time_t now = time(nullptr);
  struct tm tm {};
  localtime_r(&now, &tm);
  if (tm.tm_year + 1900 >= 2020) {
    char buf[8];
    strftime(buf, sizeof(buf), "%H:%M", &tm);
    right += "  " + std::string(buf);
  }
  theme_text(font_small_, W - kMargin - static_cast<int>(right.size()) * cw, y, right,
             { kText2.r, kText2.g, kText2.b });
  return y + ch + 24;
}

// Log card anchored to the bottom; returns its top (or the bottom margin).
int ScreenRecoveryUI::theme_log_card(int min_top) {
  const int W = ScreenWidth(), H = ScreenHeight();
  const int bottom = H - 48;
  // Collect the non-empty tail of the text buffer.
  std::vector<std::string> lines;
  int row = text_row_;
  for (size_t n = 0; n < text_rows_; ++n) {
    std::string l = text_[row];
    if (!l.empty() || !lines.empty()) lines.push_back(l);
    --row;
    if (row < 0) row = text_rows_ - 1;
  }
  while (!lines.empty() && Trim(lines.back()).empty()) lines.pop_back();
  if (lines.empty()) return bottom;
  const int pad = 36;
  int top = std::max(min_top, bottom - pad * 2 - static_cast<int>(lines.size()) * char_height_);
  if (bottom - top < pad * 2 + char_height_) return bottom;
  theme_fill_round(kMargin, top, W - 2 * kMargin, bottom - top, 36, { kLogBg.r, kLogBg.g, kLogBg.b });
  int y = bottom - pad - char_height_;
  const int max_w = W - 2 * kMargin - 2 * pad;
  for (const auto& l : lines) {
    if (y < top + pad) break;
    theme_text(gr_sys_font(), kMargin + pad, y, l, { kLogText.r, kLogText.g, kLogText.b }, false,
               max_w);
    y -= char_height_;
  }
  return top;
}

bool ScreenRecoveryUI::draw_themed_menu_locked() {
  theme_hits_.clear();
  theme_active_ = false;
  if (!theme_ready_ || !menu_) return false;
  const TextMenu* menu = menu_->AsTextMenu();
  if (!menu) return false;
  theme_active_ = true;

  const int W = ScreenWidth(), H = ScreenHeight();
  Color(kBg);
  gr_fill(0, 0, W, H);
  int y = theme_status_bar(36);

  int mw = 0, mh = 0, sw = 0, sh = 0, tw = 0, th = 0;
  gr_font_size(gr_menu_font(), &mw, &mh);
  gr_font_size(font_small_, &sw, &sh);
  gr_font_size(font_title_, &tw, &th);
  const int sel = menu->selection();
  const size_t count = menu->ItemsCount();

  if (menu->IsMain()) {
    // Logo, then 2-column tiles; "Reboot system now" becomes the bottom button.
    if (theme_logo_) {
      int lw = gr_get_width(theme_logo_.get()), lh = gr_get_height(theme_logo_.get());
      gr_blit(theme_logo_.get(), 0, 0, lw, lh, (W - lw) / 2, y);
      y += lh + 40;
    }
    const int gap = 36, tile_h = 270, tile_w = (W - 2 * kMargin - gap) / 2;
    int reboot_index = -1;
    std::vector<int> tiles;
    for (size_t i = 0; i < count; ++i) {
      if (Trim(menu->TextItem(i)) == "Reboot system now") reboot_index = static_cast<int>(i);
      else tiles.push_back(static_cast<int>(i));
    }
    for (size_t t = 0; t < tiles.size(); ++t) {
      int i = tiles[t];
      int x = kMargin + static_cast<int>(t % 2) * (tile_w + gap);
      int ty = y + static_cast<int>(t / 2) * (tile_h + gap);
      bool selected = i == sel;
      theme_card(x, ty, tile_w, tile_h, 44,
                 selected ? Rgb3{ kSurfaceSel.r, kSurfaceSel.g, kSurfaceSel.b }
                          : Rgb3{ kSurface.r, kSurface.g, kSurface.b },
                 selected);
      std::string title = Trim(menu->TextItem(i));
      ItemStyle st = StyleFor(title);
      if (st.icon) theme_draw_icon(st.icon, 104, x + 48, ty + 36, { kAccent.r, kAccent.g, kAccent.b });
      theme_text(gr_menu_font(), x + 48, ty + 156, title, { kText.r, kText.g, kText.b }, true,
                 tile_w - 96);
      if (st.subtitle) {
        theme_text(font_small_, x + 48, ty + 156 + mh + 4, st.subtitle,
                   { kText2.r, kText2.g, kText2.b }, false, tile_w - 96);
      }
      theme_hits_.push_back({ x, ty, x + tile_w, ty + tile_h, i });
    }
    y += static_cast<int>((tiles.size() + 1) / 2) * (tile_h + gap);
    if (reboot_index >= 0) {
      const int bh = 160;
      bool selected = reboot_index == sel;
      theme_fill_round(kMargin, y, W - 2 * kMargin, bh, 44,
                       selected ? Rgb3{ kText.r, kText.g, kText.b }
                                : Rgb3{ kAccent.r, kAccent.g, kAccent.b });
      theme_fill_round(kMargin + 5, y + 5, W - 2 * kMargin - 10, bh - 10, 40,
                       { kAccent.r, kAccent.g, kAccent.b });
      std::string label = "Reboot system";
      int label_w = static_cast<int>(label.size()) * mw;
      int total = 64 + 28 + label_w;
      int lx = (W - total) / 2;
      theme_draw_icon("restart", 64, lx, y + (bh - 64) / 2, { kOnAccent.r, kOnAccent.g, kOnAccent.b });
      theme_text(gr_menu_font(), lx + 64 + 28, y + (bh - mh) / 2, label,
                 { kOnAccent.r, kOnAccent.g, kOnAccent.b }, true);
      theme_hits_.push_back({ kMargin, y, W - kMargin, y + bh, reboot_index });
      y += bh;
    }
    // The log takes the bottom when there is output; otherwise a chibi Miku
    // hangs from the reboot button.
    int log_top = theme_log_card(y + 40);
    if (theme_chibi_) {
      int cw2 = gr_get_width(theme_chibi_.get()), ch2 = gr_get_height(theme_chibi_.get());
      if (y + ch2 + 24 <= log_top) gr_blit(theme_chibi_.get(), 0, 0, cw2, ch2, W - kMargin - cw2 - 48, y);
    }
    return true;
  }

  // Sub menu: back button + title, header lines, then a card list.
  const auto& headers = menu->text_headers();
  const int back = 136;
  bool back_sel = sel == -1;
  theme_card(kMargin, y, back, back, 40,
             back_sel ? Rgb3{ kSurfaceSel.r, kSurfaceSel.g, kSurfaceSel.b }
                      : Rgb3{ kSurface.r, kSurface.g, kSurface.b },
             back_sel);
  theme_draw_icon("arrow_back", 72, kMargin + (back - 72) / 2, y + (back - 72) / 2,
                  { kText.r, kText.g, kText.b });
  theme_hits_.push_back({ kMargin, y, kMargin + back, y + back, -1 });
  std::string title = headers.empty() ? "" : Trim(headers[0]);
  int head_w = 0;
  if (theme_head_) {
    head_w = gr_get_width(theme_head_.get());
    int hh = gr_get_height(theme_head_.get());
    gr_blit(theme_head_.get(), 0, 0, head_w, hh, W - kMargin - head_w, y + (back - hh) / 2);
  }
  theme_text(font_title_, kMargin + back + 40, y + (back - th) / 2, title,
             { kText.r, kText.g, kText.b }, true, W - 2 * kMargin - back - 40 - head_w - 24);
  y += back + 28;
  const int text_w = W - 2 * kMargin;
  const size_t cols = std::max(1, text_w / std::max(1, sw));
  for (size_t h = 1; h < headers.size(); ++h) {
    std::string line = headers[h];
    while (!line.empty()) {
      std::string part = line.substr(0, cols);
      if (line.size() > cols) {
        size_t sp = part.find_last_of(' ');
        if (sp != std::string::npos && sp > cols / 2) part = part.substr(0, sp);
      }
      theme_text(font_small_, kMargin, y, Trim(part), { kText2.r, kText2.g, kText2.b });
      y += sh + 6;
      line = Trim(line.substr(part.size()));
    }
  }
  y += 16;

  // Reserve room for the log, then lay out as many cards as fit around the selection.
  const int card_h = 170, gap = 24;
  int log_top = theme_log_card(std::max(y + card_h + gap, H - 700));
  int avail = log_top - 32 - y;
  int fit = std::max(1, (avail + gap) / (card_h + gap));
  int first = std::clamp(theme_scroll_, 0, std::max(0, static_cast<int>(count) - fit));
  if (sel >= 0 && sel < first) first = sel;
  if (sel >= first + fit) first = sel - fit + 1;
  theme_scroll_ = first;
  menu_->SetMenuHeight(fit * MenuItemHeight());
  for (int i = first; i < static_cast<int>(count) && i < first + fit; ++i) {
    int cy = y + (i - first) * (card_h + gap);
    bool selected = i == sel;
    theme_card(kMargin, cy, text_w, card_h, 36,
               selected ? Rgb3{ kSurfaceSel.r, kSurfaceSel.g, kSurfaceSel.b }
                        : Rgb3{ kSurface.r, kSurface.g, kSurface.b },
               selected);
    std::string item = Trim(menu->TextItem(i));
    std::string item_sub;
    if (size_t tab = item.find('\t'); tab != std::string::npos) {
      item_sub = Trim(item.substr(tab + 1));
      item = Trim(item.substr(0, tab));
    }
    ItemStyle st = StyleFor(item);
    if (!item_sub.empty()) {
      st.subtitle = item_sub.c_str();
      if (!st.icon) st.icon = "doc_generic";
    }
    int tx = kMargin + 44;
    if (st.icon) {
      theme_draw_icon(st.icon, 80, tx, cy + (card_h - 80) / 2, { kAccent.r, kAccent.g, kAccent.b });
      tx += 80 + 36;
    }
    int right = kMargin + text_w - 44 - 64;
    theme_draw_icon("chevron_right", 64, right, cy + (card_h - 64) / 2, { kMuted.r, kMuted.g, kMuted.b });
    int max_w = right - 24 - tx;
    if (st.subtitle) {
      int block = mh + 4 + sh;
      int ty = cy + (card_h - block) / 2;
      theme_text(gr_menu_font(), tx, ty, item, { kText.r, kText.g, kText.b }, false, max_w);
      theme_text(font_small_, tx, ty + mh + 4, st.subtitle, { kText2.r, kText2.g, kText2.b }, false,
                 max_w);
    } else {
      theme_text(gr_menu_font(), tx, cy + (card_h - mh) / 2, item, { kText.r, kText.g, kText.b },
                 false, max_w);
    }
    theme_hits_.push_back({ kMargin, cy, kMargin + text_w, cy + card_h, i });
  }
  if (first + fit < static_cast<int>(count)) {
    std::string more = "+" + std::to_string(count - first - fit) + " more (swipe)";
    theme_text(font_small_, kMargin, y + fit * (card_h + gap) - 8, more, { kMuted.r, kMuted.g, kMuted.b });
  }
  return true;
}

int ScreenRecoveryUI::theme_select(const Point& point) {
  for (const auto& h : theme_hits_) {
    if (point.x() >= h.x0 && point.x() < h.x1 && point.y() >= h.y0 && point.y() < h.y1) {
      if (h.item < 0) return Device::kGoBack;
      int s = menu_->Select(h.item);
      update_screen_locked();
      return s;
    }
  }
  return Device::kNoAction;
}
