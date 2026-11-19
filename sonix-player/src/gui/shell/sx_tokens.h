#ifndef GUI_SX_TOKENS_H
#define GUI_SX_TOKENS_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// Sonix UI 3.1: Graphite 2 design tokens
//
// Shared component sizes, gesture thresholds and colours. All sizes are
// screen pixels on the 480 px wide panel (R1 480 x 800, R3 Pro II 480 x 720);
// nothing here scales.
//
// The colours that depend on the cover are not here as RGB: they are an OKLCH
// lightness and chroma (sx_lc_t) that sx_palette.c turns into RGB565 for the
// hue of the track playing, once per track. See sx_palette.h.
//
// Header only, no code: include it from anywhere.
// ---------------------------------------------------------------------------

// ---------- The frame ----------
#define SX_STATUSBAR_H 44
#define SX_STATUSBAR_FONT 22
#define SX_HEADER_H 88	 // under the 44 px status bar
#define SX_HEADER_BTN 88 // the back target and the header's buttons
#define SX_TITLE_FONT 32 // semibold; smaller when the available width requires it
#define SX_TITLE_FONT_LONG 30
#define SX_TITLE_LONG_CHARS 12
#define SX_BODY_TOP 132
#define SX_BODY_TOP_HOME 59
#define SX_BODY_PAD_L 16
#define SX_BODY_PAD_R 16 // 32 with the A-Z lane
#define SX_BODY_PAD_R_LANE 32
#define SX_EDGE_BACK_W 72				  // navigation owns this left-edge strip
#define SX_ROW_SWIPE_INSET SX_EDGE_BACK_W // row actions stay outside the navigation strip
#define SX_EDGE_HOME_PX 160	  // a deliberate long pull, then a stationary dwell
#define SX_EDGE_HOME_HOLD_MS 700
#define SX_EDGE_HOME_JITTER_PX 12

// ---------- A list row ----------
#define SX_ROW_H 88
#define SX_ROW_H_BIG 104
#define SX_ROW_H_COMPACT 64 // the queue; with large rows 84
#define SX_ROW_H_COMPACT_BIG 84
#define SX_ROW_THUMB 56 // large rows 68, compact 48
#define SX_ROW_THUMB_BIG 64
#define SX_ROW_THUMB_COMPACT 48
#define SX_ROW_THUMB_R 12
#define SX_ROW_R 14
#define SX_ROW_GAP 2
#define SX_ROW_PAD_L 12
#define SX_ROW_PAD_R 12
#define SX_ROW_INNER_GAP 12
#define SX_ROW_TITLE_FONT 26
#define SX_ROW_SUB_FONT 20
#define SX_ROW_TRAIL_W 72 // the star button or the chevron at the right
#define SX_ROW_NUM_W 32	  // the track number on an album page

// ---------- A settings card ----------
#define SX_CARD_R 20
#define SX_CARD_PAD_V 16
#define SX_CARD_PAD_H 24
#define SX_CARD_MIN_H 56 // content plus vertical padding gives an 88 px card
#define SX_CARD_GAP 12
#define SX_CARD_TITLE_FONT 24
#define SX_CARD_SUB_FONT 22 // the mono subtitle (codec, network): 17
#define SX_CARD_SUB_MONO_FONT 17
#define SX_CARD_VALUE_FONT 22
#define SX_PILL_H 52
#define SX_PILL_PAD_H 22
#define SX_PILL_FONT 22
#define SX_PILL_GAP 10
#define SX_SWITCH_W 68
#define SX_SWITCH_H 36
#define SX_SWITCH_KNOB 28
#define SX_SWATCH 52
#define SX_BAR_H 8 // a card's fill bar (cache used, scan progress)

// ---------- Tiles (home, Music, Streaming, Networks, More) ----------
#define SX_TILE_GAP 12
#define SX_TILE_R 20
#define SX_TILE_H_MAX_HOME 232
#define SX_TILE_H_MAX 204
#define SX_TILE_H_MIN 120 // height = (H - top - 88 - 15 - 2 * 12) / 3, kept in range
#define SX_TILE_FONT 24
#define SX_TILE_H_R1 188
#define SX_TILE_H_R3 164
#define SX_TILE_PAD 20
#define SX_TILE_SUB_H 24
#define SX_TILE_ICON 34

// ---------- The mini player ----------
#define SX_MINI_SLOT_H 128
#define SX_MINI_H 112
#define SX_MINI_COMPACT_SLOT_H 72
#define SX_MINI_COMPACT_H 56
#define SX_MINI_COMPACT_TARGET 64
#define SX_MINI_COMPACT_PLAY 44
#define SX_MINI_R 20
#define SX_MINI_COVER 88
#define SX_MINI_COVER_R 12
#define SX_MINI_PLAY 64
#define SX_MINI_PLAY_TARGET 88
#define SX_MINI_PROGRESS_H 3
#define SX_MINI_PROGRESS_INSET 18
#define SX_MINI_PROGRESS_BOTTOM 4
#define SX_MINI_MARGIN_TOP 8
#define SX_MINI_MARGIN_SIDE 16
#define SX_MINI_MARGIN_BOTTOM 8

// ---------- Now playing ----------
#define SX_NP_PANEL_H 352
#define SX_NP_PANEL_R 24
#define SX_NP_TITLE_FONT 32
#define SX_NP_ARTIST_FONT 22
#define SX_NP_SEEK_TRACK_H 6
#define SX_NP_SEEK_KNOB 20
#define SX_NP_PLAY 96
#define SX_NP_SKIP 76
#define SX_NP_SIDE_BTN 72
#define SX_NP_UPNEXT_H 72

// ---------- Layers ----------
#define SX_SHEET_R 24
#define SX_SHEET_ITEM_H 88
#define SX_SHEET_ITEM_FONT 24
#define SX_SHEET_TITLE_FONT 26
#define SX_SHEET_SUB_FONT 21
#define SX_SHEET_HANDLE_W 44
#define SX_SHEET_HANDLE_H 5
#define SX_SHEET_PAD_H 28
#define SX_SHEET_BTN_H 88
#define SX_TOAST_H 88
#define SX_TOAST_R 18
#define SX_TOAST_MS 6000
#define SX_TOAST_FONT 22
#define SX_TOAST_MARGIN 16 // from the sides
#define SX_VOL_OSD_W 64
#define SX_VOL_OSD_H 300
#define SX_VOL_OSD_MS 1500
#define SX_VOL_OSD_RIGHT 14 // from the right edge
#define SX_AZ_LANE_W 28
#define SX_AZ_BUBBLE 104
#define SX_CC_BTN 64

// ---------- Gestures and time ----------
#define SX_LONGPRESS_MS 450
#define SX_SWIPE_LOCK_PX 10 // past this a row is taken sideways
#define SX_SWIPE_COMMIT_PX 100
#define SX_SWIPE_MAX_PX 160
#define SX_DRAG_LOCK_PX 12 // the player and the sheet, downwards
#define SX_NP_COLLAPSE_PX 110
#define SX_SHEET_CLOSE_PX 80
#define SX_TOAST_DISMISS_PX 30
#define SX_CC_CLOSE_PX 60	// upwards
#define SX_MINI_SWIPE_PX 60 // sideways; upwards it is 40
#define SX_MINI_SWIPE_UP_PX 40
#define SX_EDGE_BACK_PX 70
#define SX_PREV_RESTART_S 3 // previous after 3 s goes to the start of the track
#define SX_ANIM_NAV_MS 180	// cubic-bezier(.2,.8,.2,1), close to lv_anim_path_ease_out
#define SX_ANIM_SHEET_MS 200
#define SX_ANIM_ROW_SNAP_MS 150
#define SX_EQ_FRAME_MS 100 // the equaliser bars, about 10 frames a second

// ---------- The neutral palette (tint from cover off) ----------
#define SX_D_BG 0x11151A
#define SX_D_SURFACE 0x1E252D
#define SX_D_PRESS 0x303B46
#define SX_D_TEXT 0xF1F4F7
#define SX_D_TEXT2 0xAEBBC6
#define SX_L_BG 0xE5E9EC
#define SX_L_SURFACE 0xF7F9FA
#define SX_L_PRESS 0xD4DDE3
#define SX_L_TEXT 0x1A252E
#define SX_L_TEXT2 0x465663
#define SX_D_ACCENT 0x366C8C
#define SX_D_ACCENT_TEXT 0x8FBAD4
#define SX_L_ACCENT 0x295C7A
#define SX_CATEGORY_BG 0xAAB8C2
#define SX_CATEGORY_INK 0x17232B

// ---------- Fixed colours ----------
#define SX_TEXT_ON_DARK 0xF1EFE9
#define SX_TEXT_ON_LIGHT 0x25232A
#define SX_STAR_ON_DARK 0xF6D32D
#define SX_STAR_ON_LIGHT 0xB07800
#define SX_STAR_OFF 0x969696
#define SX_CHAIN_OK 0x8FE3C0
#define SX_CHAIN_WARN 0xF6C548
#define SX_DANGER_BG 0xAE3540
#define SX_DANGER_TEXT 0xFF9B9F
#define SX_L_DANGER_TEXT 0xA12E38
#define SX_D_WARNING_TEXT 0xE0BD79
#define SX_L_WARNING_TEXT 0x75551A
#define SX_TOAST_BG 0xF1EFE9
#define SX_TOAST_TEXT 0x17161A
#define SX_TOAST_ACTION 0x245A8A
#define SX_CC_BG SX_D_SURFACE
#define SX_CC_BTN_OFF SX_D_PRESS
#define SX_VOL_OSD_BG 0x141416
// The 1 px line along the top of the sheet and the mini player: white at 16 %
// in the dark theme, black at 12 % in the light one.
#define SX_HAIRLINE_DARK_OPA 41
#define SX_HAIRLINE_LIGHT_OPA 31

// The accent presets, for whoever does not want it from the cover.
#define SX_ACC_BLUE 0x3584E4
#define SX_ACC_ORANGE 0xFF7800
#define SX_ACC_YELLOW 0xE5A50A // text on it is 0x1D1D1D
#define SX_ACC_GREEN 0x2EC27E
#define SX_ACC_PURPLE 0x9141AC
#define SX_ACC_PINK 0xD56199
#define SX_ACC_YELLOW_INK 0x1D1D1D

// ---------- The palette from the cover: {L, C} at the cover's hue ----------
//
// Macros rather than the handoff's `static const` objects, so that including
// this header costs no data in a file that does not use them.
typedef struct {
	float L, C;
} sx_lc_t;

// Dark theme
#define SX_T_D_BG ((sx_lc_t){0.20f, 0.032f})
#define SX_T_D_SURFACE ((sx_lc_t){0.26f, 0.040f})
#define SX_T_D_PRESS ((sx_lc_t){0.31f, 0.046f})
#define SX_T_D_TEXT2 ((sx_lc_t){0.76f, 0.030f})
#define SX_T_D_ACCENT ((sx_lc_t){0.84f, 0.100f})
#define SX_T_D_ON_ACC ((sx_lc_t){0.20f, 0.040f})
#define SX_T_D_SWIPE_L ((sx_lc_t){0.42f, 0.070f}) // "Add to the end of the queue"
// Light theme
#define SX_T_L_BG ((sx_lc_t){0.955f, 0.015f})
#define SX_T_L_PRESS ((sx_lc_t){0.90f, 0.025f})
#define SX_T_L_TEXT2 ((sx_lc_t){0.45f, 0.030f})
#define SX_T_L_ACCENT ((sx_lc_t){0.50f, 0.140f}) // white text on it
#define SX_T_L_SWIPE_L ((sx_lc_t){0.82f, 0.060f})
// Always dark: the player, the mini player and the album page
#define SX_T_NP_ACCENT ((sx_lc_t){0.84f, 0.100f})
#define SX_T_NP_INK ((sx_lc_t){0.30f, 0.070f}) // the glyph on the white play disc
#define SX_T_MINI_BG ((sx_lc_t){0.30f, 0.060f})
#define SX_T_NP_GRAD_TOP ((sx_lc_t){0.40f, 0.090f}) // the player's backdrop, top
#define SX_T_NP_GRAD_BOT ((sx_lc_t){0.18f, 0.035f}) //                       bottom
#define SX_T_NP_PANEL_T ((sx_lc_t){0.28f, 0.065f})	// the control panel, top
#define SX_T_NP_PANEL_B ((sx_lc_t){0.14f, 0.030f})	//                    bottom
#define SX_T_AL_GRAD_0 ((sx_lc_t){0.52f, 0.100f})	// album page at 0 px (and its status bar)
#define SX_T_AL_GRAD_1 ((sx_lc_t){0.38f, 0.075f})	//            at 380 px
#define SX_T_AL_GRAD_2 ((sx_lc_t){0.22f, 0.035f})	//            at 900 px
#define SX_T_COVER_TONE ((sx_lc_t){0.50f, 0.090f})	// the plaque shown until a thumbnail exists

#endif /* GUI_SX_TOKENS_H */
