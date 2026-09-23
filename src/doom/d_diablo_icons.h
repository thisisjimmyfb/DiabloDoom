// DiabloDoom item icons - public interface.
#ifndef D_DIABLO_ICONS_H
#define D_DIABLO_ICONS_H

// 32x32 palette-indexed icon data, one per entry.
extern const unsigned char *d_item_icons[];
extern const int d_num_item_icons;
extern const int d_icon_size;

// Icon indices (must match order in d_diablo_icons.c).
#define D_ICON_SWORD_SHORT 0
#define D_ICON_SWORD_LONG 1
#define D_ICON_SWORD_RUNE 2
#define D_ICON_AXE_WAR 3
#define D_ICON_AXE_DOOM 4
#define D_ICON_AXE_GODLY 5
#define D_ICON_ARMOR_LEATHER 6
#define D_ICON_ARMOR_CHAIN 7
#define D_ICON_ARMOR_PLATE 8
#define D_ICON_SHIELD_BUCKLER 9
#define D_ICON_SHIELD_BONE 10
#define D_ICON_HELM_CAP 11
#define D_ICON_HELM_CREST 12
#define D_ICON_RING 13
#define D_ICON_AMULET 14
#define D_ICON_BELT 15
#define D_ICON_POTION_RED 16
#define D_ICON_POTION_BLUE 17
#define D_ICON_POTION_GREEN 18

#endif
