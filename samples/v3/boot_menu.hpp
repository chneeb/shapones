#ifndef BOOT_MENU_HPP
#define BOOT_MENU_HPP

#include "common.hpp"   // FRAME_BUFF_HEIGHT

// show boot menu
bool boot_menu();

// ROM list layout. The font is mono8x16; ROW_H is the line pitch, so 20 keeps
// the original 4 px gap between rows. items_per_page used to be hardcoded to
// 11 with this very formula commented out beside it, which left three rows of
// screen unused. One row is reserved at the bottom for the counter.
// (ROW_H = 16 would pack 17 per page with no gap, at the cost of looking
// cramped.)
constexpr int ROW_H = 20;
constexpr int items_per_page = (FRAME_BUFF_HEIGHT / ROW_H) - 1;
#endif
