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

// On-screen keyboard + line editor for ScreenRecoveryUI (used by the recovery
// terminal and for password entry). Touch taps hit the keyboard; physical
// keyboards (USB / the official pogo keyboard) work too. Vertical swipes and
// volume keys scroll the text above the input line.

#include <linux/input.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <minui/minui.h>

#include "recovery_ui/screen_ui.h"

namespace {

enum KbCode : int {
  kKbNone = 0,
  kKbShift = -1,
  kKbSym = -2,
  kKbBksp = -3,
  kKbEnter = -4,
  kKbExit = -5,
  kKbScrollUp = -6,
  kKbScrollDown = -7,
  kKbSpacer = -8,
};

struct KbKey {
  std::string label;
  int code;
  float weight;
};

using KbRow = std::vector<KbKey>;

KbRow Chars(const std::string& s) {
  KbRow row;
  for (char c : s) row.push_back({ std::string(1, c), c, 1.0f });
  return row;
}

KbRow Join(std::initializer_list<KbRow> parts) {
  KbRow row;
  for (const auto& p : parts) row.insert(row.end(), p.begin(), p.end());
  return row;
}

// mode 0 = lower case, 1 = upper case (one shot), 2 = symbols.
std::vector<KbRow> Layout(int mode) {
  const KbKey exit_key{ "Exit", kKbExit, 1.5f };
  const KbKey enter{ "Enter", kKbEnter, 1.5f };
  const KbKey bksp{ "Del", kKbBksp, 1.5f };
  if (mode == 2) {
    return {
      Chars("1234567890"),
      Chars("!@#$%^&*()"),
      Chars("-_=+[]{}\\|"),
      Join({ { { "abc", kKbSym, 1.5f } }, Chars(";:'\"<>?"), { bksp } }),
      Join({ { exit_key, { "abc", kKbSym, 1.5f }, { "Space", ' ', 2.5f } }, Chars("~`,"),
             { enter } }),
    };
  }
  const bool upper = mode == 1;
  auto letters = [upper](std::string s) {
    if (upper) std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return Chars(s);
  };
  return {
    upper ? Chars("!@#$%^&*()") : Chars("1234567890"),
    letters("qwertyuiop"),
    Join({ { { "", kKbSpacer, 0.5f } }, letters("asdfghjkl"), { { "", kKbSpacer, 0.5f } } }),
    Join({ { { upper ? "SHIFT" : "Shift", kKbShift, 1.5f } }, letters("zxcvbnm"), { bksp } }),
    Join({ { exit_key, { "?123", kKbSym, 1.5f }, { "Space", ' ', 2.5f } }, Chars("-./"),
           { enter } }),
  };
}

// Physical keyboard -> character / action.
int KeyToCode(int key, bool shift) {
  static const struct { int key; char lo, hi; } kMap[] = {
    { KEY_1, '1', '!' }, { KEY_2, '2', '@' }, { KEY_3, '3', '#' }, { KEY_4, '4', '$' },
    { KEY_5, '5', '%' }, { KEY_6, '6', '^' }, { KEY_7, '7', '&' }, { KEY_8, '8', '*' },
    { KEY_9, '9', '(' }, { KEY_0, '0', ')' }, { KEY_MINUS, '-', '_' }, { KEY_EQUAL, '=', '+' },
    { KEY_Q, 'q', 'Q' }, { KEY_W, 'w', 'W' }, { KEY_E, 'e', 'E' }, { KEY_R, 'r', 'R' },
    { KEY_T, 't', 'T' }, { KEY_Y, 'y', 'Y' }, { KEY_U, 'u', 'U' }, { KEY_I, 'i', 'I' },
    { KEY_O, 'o', 'O' }, { KEY_P, 'p', 'P' }, { KEY_LEFTBRACE, '[', '{' },
    { KEY_RIGHTBRACE, ']', '}' }, { KEY_A, 'a', 'A' }, { KEY_S, 's', 'S' }, { KEY_D, 'd', 'D' },
    { KEY_F, 'f', 'F' }, { KEY_G, 'g', 'G' }, { KEY_H, 'h', 'H' }, { KEY_J, 'j', 'J' },
    { KEY_K, 'k', 'K' }, { KEY_L, 'l', 'L' }, { KEY_SEMICOLON, ';', ':' },
    { KEY_APOSTROPHE, '\'', '"' }, { KEY_GRAVE, '`', '~' }, { KEY_BACKSLASH, '\\', '|' },
    { KEY_Z, 'z', 'Z' }, { KEY_X, 'x', 'X' }, { KEY_C, 'c', 'C' }, { KEY_V, 'v', 'V' },
    { KEY_B, 'b', 'B' }, { KEY_N, 'n', 'N' }, { KEY_M, 'm', 'M' }, { KEY_COMMA, ',', '<' },
    { KEY_DOT, '.', '>' }, { KEY_SLASH, '/', '?' }, { KEY_SPACE, ' ', ' ' },
  };
  for (const auto& m : kMap) {
    if (m.key == key) return shift ? m.hi : m.lo;
  }
  switch (key) {
    case KEY_ENTER:
    case KEY_KPENTER:
    case KEY_POWER:
      return kKbEnter;
    case KEY_BACKSPACE:
      return kKbBksp;
    case KEY_ESC:
    case KEY_BACK:
      return kKbExit;
    case KEY_VOLUMEUP:
    case KEY_UP:
    case KEY_PAGEUP:
    case KEY_SCROLLUP:
      return kKbScrollUp;
    case KEY_VOLUMEDOWN:
    case KEY_DOWN:
    case KEY_PAGEDOWN:
    case KEY_SCROLLDOWN:
      return kKbScrollDown;
  }
  return kKbNone;
}

std::vector<std::string> Wrap(const std::vector<std::string>& lines, size_t cols) {
  std::vector<std::string> out;
  if (cols == 0) cols = 1;
  for (const auto& l : lines) {
    if (l.empty()) {
      out.emplace_back();
      continue;
    }
    for (size_t i = 0; i < l.size(); i += cols) out.push_back(l.substr(i, cols));
  }
  return out;
}

}  // namespace

void ScreenRecoveryUI::draw_readline_locked() {
  const int w = ScreenWidth();
  const int h = ScreenHeight();
  const int gap = std::max(4, w / 300);

  // --- keyboard geometry (bottom of the screen) ---
  const auto rows = Layout(rl_mode_);
  const int key_h = std::min(w / 10, h / 14);
  const int kb_h = key_h * static_cast<int>(rows.size());
  const int kb_top = h - kb_h - margin_height_ - 28;

  gr_color(13, 20, 23, 255);
  gr_fill(0, 0, w, h);
  gr_color(18, 27, 31, 255);
  gr_fill(0, kb_top - gap, w, h);

  rl_hit_.clear();
  const GRFont* kfont = gr_menu_font();
  int kfw = 0, kfh = 0;
  gr_font_size(kfont, &kfw, &kfh);
  for (size_t r = 0; r < rows.size(); ++r) {
    float total = 0;
    for (const auto& k : rows[r]) total += k.weight;
    float x = 0;
    const int y = kb_top + static_cast<int>(r) * key_h;
    for (const auto& k : rows[r]) {
      const int x0 = 24 + static_cast<int>(x / total * (w - 48));
      x += k.weight;
      const int x1 = 24 + static_cast<int>(x / total * (w - 48));
      if (k.code == kKbSpacer) continue;
      const bool special = k.code < 0 || k.code == ' ';
      const bool active = (k.code == kKbShift && rl_mode_ == 1) || (k.code == kKbSym && rl_mode_ == 2);
      const bool enter = k.code == kKbEnter;
      Rgb3 fill = (active || enter) ? Rgb3{ 57, 197, 187 }
                  : special         ? Rgb3{ 24, 58, 58 }
                                    : Rgb3{ 38, 54, 60 };
      theme_fill_round(x0 + gap, y + gap, x1 - x0 - 2 * gap, key_h - 2 * gap, 24, fill);
      if (active || enter) gr_color(4, 52, 44, 255);
      else if (special) gr_color(127, 184, 178, 255);
      else gr_color(232, 246, 244, 255);
      int tw = gr_measure(kfont, k.label.c_str());
      gr_text(kfont, x0 + (x1 - x0 - tw) / 2, y + (key_h - kfh) / 2, k.label.c_str(), false);
      rl_hit_.push_back({ Point(x0, y), Point(x1, y + key_h), k.code });
    }
  }

  // --- input line, just above the keyboard ---
  const int in_h = kfh + gap * 4;
  const int in_top = kb_top - gap - in_h;
  theme_fill_round(margin_width_ + 24, in_top + gap, w - 2 * margin_width_ - 48,
                   kb_top - 2 * gap - in_top, 28, { 57, 197, 187 });
  theme_fill_round(margin_width_ + 28, in_top + gap + 4, w - 2 * margin_width_ - 56,
                   kb_top - 2 * gap - in_top - 8, 24, { 22, 32, 36 });
  std::string shown = rl_masked_ ? std::string(rl_input_.size(), '*') : rl_input_;
  std::string line = rl_prompt_ + shown + "_";
  const int max_chars = std::max(1, (w - 2 * margin_width_ - 128) / std::max(1, kfw));
  if (static_cast<int>(line.size()) > max_chars) line = line.substr(line.size() - max_chars);
  gr_color(255, 255, 255, 255);
  gr_text(kfont, margin_width_ + 64, in_top + (in_h - kfh) / 2, line.c_str(), true);

  // --- scrollback text (bottom-aligned above the input line) ---
  const GRFont* tfont = gr_sys_font();
  const int cols = std::max(1, (w - 2 * margin_width_ - 96) / std::max(1, char_width_));
  const auto wrapped = Wrap(rl_lines_, cols);
  const int top = margin_height_ + 48;
  const int visible = std::max(0, (in_top - gap - top) / std::max(1, char_height_));
  const int max_scroll = std::max(0, static_cast<int>(wrapped.size()) - visible);
  rl_scroll_ = std::clamp(rl_scroll_, 0, max_scroll);
  const int end = static_cast<int>(wrapped.size()) - rl_scroll_;
  const int begin = std::max(0, end - visible);
  gr_color(184, 199, 197, 255);
  int y = in_top - gap - (end - begin) * char_height_;
  for (int i = begin; i < end; ++i, y += char_height_) {
    gr_text(tfont, margin_width_ + 48, y, wrapped[i].c_str(), false);
  }
  if (rl_scroll_ > 0) {
    gr_color(57, 197, 187, 255);
    std::string more = "-- " + std::to_string(rl_scroll_) + " more lines below (swipe/volume) --";
    gr_text(tfont, margin_width_ + 48, in_top - gap - char_height_, more.c_str(), true);
  }
}

std::optional<std::string> ScreenRecoveryUI::ReadLine(const std::vector<std::string>& lines,
                                                      const std::string& prompt, bool masked) {
  FlushKeys();
  {
    std::lock_guard<std::mutex> lg(updateMutex);
    readline_active_ = true;
    rl_lines_ = lines;
    rl_prompt_ = prompt;
    rl_input_.clear();
    rl_masked_ = masked;
    rl_mode_ = 0;
    rl_scroll_ = 0;
    show_text = true;
    show_text_ever = true;
    update_screen_locked();
  }

  std::optional<std::string> result;
  for (bool done = false; !done;) {
    InputEvent evt = WaitInputEvent();
    int code = kKbNone;
    if (evt.type() == EventType::EXTRA) {
      if (evt.key() == static_cast<int>(KeyError::INTERRUPTED)) break;
      continue;  // timed out: keep waiting
    }
    if (evt.type() == EventType::TOUCH) {
      std::lock_guard<std::mutex> lg(updateMutex);
      for (const auto& hit : rl_hit_) {
        if (evt.pos().x() >= hit.min.x() && evt.pos().x() < hit.max.x() &&
            evt.pos().y() >= hit.min.y() && evt.pos().y() < hit.max.y()) {
          code = hit.code;
          break;
        }
      }
    } else {
      bool shift = IsKeyPressed(KEY_LEFTSHIFT) || IsKeyPressed(KEY_RIGHTSHIFT);
      code = KeyToCode(evt.key(), shift);
    }

    std::lock_guard<std::mutex> lg(updateMutex);
    switch (code) {
      case kKbNone:
        continue;
      case kKbShift:
        rl_mode_ = rl_mode_ == 1 ? 0 : 1;
        break;
      case kKbSym:
        rl_mode_ = rl_mode_ == 2 ? 0 : 2;
        break;
      case kKbBksp:
        if (!rl_input_.empty()) rl_input_.pop_back();
        break;
      case kKbEnter:
        result = rl_input_;
        done = true;
        break;
      case kKbExit:
        done = true;
        break;
      case kKbScrollUp:
        rl_scroll_ += 3;
        break;
      case kKbScrollDown:
        rl_scroll_ = std::max(0, rl_scroll_ - 3);
        break;
      default:
        if (code >= 32 && code < 127) {
          rl_input_.push_back(static_cast<char>(code));
          if (rl_mode_ == 1) rl_mode_ = 0;  // one-shot shift
          rl_scroll_ = 0;
        }
        break;
    }
    if (!done) update_screen_locked();
  }

  std::lock_guard<std::mutex> lg(updateMutex);
  readline_active_ = false;
  std::fill(rl_input_.begin(), rl_input_.end(), '\0');
  rl_input_.clear();
  rl_lines_.clear();
  rl_hit_.clear();
  update_screen_locked();
  return result;
}
