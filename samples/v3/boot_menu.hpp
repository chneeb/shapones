#ifndef BOOT_MENU_HPP
#define BOOT_MENU_HPP

#include "common.hpp"   // FRAME_BUFF_HEIGHT

// show boot menu
bool boot_menu();

// ROM list layout. The font is mono8x16, so ROW_H = 16 is the tightest that
// does not clip glyphs; one row is reserved at the bottom for the counter.
// This used to be a hardcoded 11, which left three rows of screen unused even
// at the old 20 px spacing.
constexpr int ROW_H = 16;
constexpr int items_per_page = (FRAME_BUFF_HEIGHT / ROW_H) - 1;
#endif
