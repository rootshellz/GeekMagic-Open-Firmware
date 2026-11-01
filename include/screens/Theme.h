// SPDX-License-Identifier: GPL-3.0-or-later
// MatrixGrid palette shared by the native screens (same colours as screens/turzx/panels/ui.py in the host repo).
#ifndef SCREENS_THEME_H
#define SCREENS_THEME_H

#include <cstdint>

namespace Theme {

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

constexpr uint16_t BG = rgb(0, 6, 0);
constexpr uint16_t FRAME = rgb(0, 140, 45);
constexpr uint16_t GREEN = rgb(0, 255, 65);
constexpr uint16_t LABEL = rgb(0, 190, 55);
constexpr uint16_t DIM = rgb(0, 120, 40);
constexpr uint16_t FAINT = rgb(0, 60, 22);
constexpr uint16_t WARN = rgb(255, 190, 0);
constexpr uint16_t CRIT = rgb(255, 50, 50);
constexpr uint16_t INFO = rgb(60, 200, 255);
constexpr uint16_t WHITE = rgb(225, 245, 225);

}  // namespace Theme

#endif  // SCREENS_THEME_H
