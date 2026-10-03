/**
 * ============================================================================
 * Citro - Lightweight Invidious YouTube Client for Nintendo 3DS
 * File: main.c
 * ----------------------------------------------------------------------------
 * Features:
 *  - Full devkitPro/libctru and citro2d dual-screen lifecycle
 *  - SOC (Socket Service) memory-aligned initialization for network requests
 *  - Keyless Invidious API search (no developer keys or accounts required)
 *  - Top screen (400x240): Video playback stage with 5-second OSD title banner
 *  - Bottom screen (320x240): Touchscreen transport controls, seek bar, likes,
 *    and description toggle
 *  - Zero dynamic heap allocation in the 60 FPS render loop
 * ============================================================================
 */

#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <inttypes.h>

#include "citro_invidious.h"

/* 3DS Screen Resolutions */
#define SCREEN_TOP_WIDTH       400
#define SCREEN_TOP_HEIGHT      240
#define SCREEN_BOTTOM_WIDTH    320
#define SCREEN_BOTTOM_HEIGHT   240

/* 3DS SOC (Socket Service) buffer: must be page-aligned (0x1000) and >= 0x40000 */
#define SOC_ALIGN              0x1000
#define SOC_BUFFERSIZE         0x100000 /* 1 Megabyte socket buffer */

/* 5 seconds in CPU ticks (3DS CPU is ~268.12 MHz) */
#define OSD_DURATION_TICKS     (5ULL * 268123480ULL)

/* Citric Theme Colors in RGBA8 format for citro2d */
#define COLOR_BG               C2D_Color32(18, 20, 16, 255)    /* Citrus Rind Dark Slate */
#define COLOR_PANEL            C2D_Color32(28, 32, 24, 255)    /* Citric Charcoal */
#define COLOR_PANEL_ALT        C2D_Color32(40, 46, 34, 255)    /* Citric Tint */
#define COLOR_ACCENT           C2D_Color32(249, 115, 22, 255)  /* Citrus Tangerine Orange */
#define COLOR_ACCENT_HOVER     C2D_Color32(251, 146, 60, 255)  /* Citrus Light Orange */
#define COLOR_LIME             C2D_Color32(132, 204, 22, 255)  /* Citric Lime Green */
#define COLOR_LEMON            C2D_Color32(234, 179, 8, 255)   /* Meyer Lemon Yellow */
#define COLOR_TEXT_WHITE       C2D_Color32(248, 250, 245, 255)
#define COLOR_TEXT_MUTED       C2D_Color32(163, 175, 155, 255)
#define COLOR_SEEK_BG          C2D_Color32(50, 56, 46, 255)
#define COLOR_SEEK_FILL        C2D_Color32(249, 115, 22, 255)  /* Citrus Orange Fill */
#define COLOR_BANNER_BG        C2D_Color32(12, 16, 12, 225)
#define COLOR_BUTTON_BLUE      C2D_Color32(132, 204, 22, 255)  /* Citric Lime Action */

/* Pre-allocated static buffers to avoid dynamic allocation during 60FPS render loop */
static u32 *s_socBuffer = NULL;
static C2D_TextBuf s_topTextBuf;
static C2D_TextBuf s_bottomTextBuf;
static C2D_Text s_topTexts[16];
static C2D_Text s_bottomTexts[24];

/* Custom TTF & Material Icon Fonts for 3DS Graphics Pipeline */
static C2D_Font s_fontUbuntuRegular = NULL;
static C2D_Font s_fontUbuntuBold    = NULL;
static C2D_Font s_fontAndikaRegular = NULL;
static C2D_Font s_fontAndikaBold    = NULL;
static C2D_Font s_fontMaterialIcons = NULL;

/* Material Icons Unicode Glyphs (from icons/MaterialIcons-Regular.ttf) */
#define ICON_PLAY               "\xEE\x80\xB7"  /* U+E037: play_arrow */
#define ICON_PAUSE              "\xEE\x80\xB4"  /* U+E034: pause */
#define ICON_SEARCH             "\xEE\xA2\xB6"  /* U+E8B6: search */
#define ICON_THUMB_UP           "\xEE\xA3\x9C"  /* U+E8DC: thumb_up */
#define ICON_FORWARD_10         "\xEE\x81\x96"  /* U+E056: forward_10 */
#define ICON_REPLAY_10          "\xEE\x81\x99"  /* U+E059: replay_10 */
#define ICON_REFRESH            "\xEE\x97\x95"  /* U+E5D5: refresh */
#define ICON_INFO               "\xEE\xA2\x8E"  /* U+E88E: info */
#define ICON_TV                 "\xEE\x8C\xB3"  /* U+E333: tv */
#define ICON_CODE               "\xEE\xA1\xAF"  /* U+E86F: code / terminal */
#define ICON_DNS                "\xEE\xA1\xB5"  /* U+E875: dns */
#define ICON_ARROW_UP           "\xEE\x8C\x96"  /* U+E316: keyboard_arrow_up */
#define ICON_ARROW_DOWN         "\xEE\x8C\x93"  /* U+E313: keyboard_arrow_down */
#define ICON_BATTERY_FULL       "\xEE\x86\xA4"  /* U+E1A4: battery_std */
#define ICON_BATTERY_CHARGING   "\xEE\x86\xA3"  /* U+E1A3: battery_charging_full */

/**
 * Safely parse text with a custom C2D_Font, falling back to 3DS system font if font is NULL.
 */
static inline void parse_text_font(C2D_Text *text, C2D_Font font, C2D_TextBuf buf, const char *str) {
    if (font) {
        C2D_TextFontParse(text, font, buf, str);
    } else {
        C2D_TextParse(text, buf, str);
    }
    C2D_TextOptimize(text);
}

/**
 * Load a font file checking RomFS, local project folder, and SDMC.
 */
static C2D_Font load_3ds_font(const char *subfolder, const char *filename) {
    char path[128];
    C2D_Font font = NULL;

    /* 1. Try RomFS virtual archive */
    snprintf(path, sizeof(path), "romfs:/%s/%s", subfolder, filename);
    font = C2D_FontLoad(path);
    if (font) return font;

    /* 2. Try relative folder in local working directory */
    snprintf(path, sizeof(path), "%s/%s", subfolder, filename);
    font = C2D_FontLoad(path);
    if (font) return font;

    /* 3. Try SDMC application directory (sdmc:/3ds/Citro/...) */
    snprintf(path, sizeof(path), "sdmc:/3ds/Citro/%s/%s", subfolder, filename);
    font = C2D_FontLoad(path);
    if (font) return font;

    return NULL;
}

/* Global Application State */
static AppState       s_appState = STATE_SEARCH;
static SearchResults  s_searchResults;
static PlaybackState  s_playback;
static BatteryStatus  s_battery = { .level = 5, .percent = 100, .isCharging = false, .isAdapterPlugged = false };
static int            s_selectedResultIndex = 0;
static int           s_descScrollOffset = 0;

/* Invidious Server Presets & Configurable Instance Host */
static const char *s_serverPresets[] = {
    "invidious.f5.si",
    "invidious.flokinet.to",
    "inv.nadeko.net",
    "yewtu.be"
};
#define NUM_SERVER_PRESETS (int)(sizeof(s_serverPresets) / sizeof(s_serverPresets[0]))
static int  s_currentServerIndex = 0;
static char s_currentHost[128] = "invidious.f5.si";

/**
 * Cycle to the next preset Invidious instance host.
 */
static void cycle_invidious_server(void) {
    s_currentServerIndex = (s_currentServerIndex + 1) % NUM_SERVER_PRESETS;
    snprintf(s_currentHost, sizeof(s_currentHost), "%s", s_serverPresets[s_currentServerIndex]);
    invidious_search(s_currentHost, s_searchResults.query[0] ? s_searchResults.query : "3ds", &s_searchResults);
}

/**
 * Prompt user for a custom Invidious instance host via 3DS Software Keyboard (swkbd).
 */
static void prompt_custom_server(void) {
    SwkbdState swkbd;
    char inputBuf[128] = {0};

    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
    swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    swkbdSetHintText(&swkbd, "Enter Invidious Host (e.g. yewtu.be)");
    swkbdSetInitialText(&swkbd, s_currentHost);

    SwkbdButton button = swkbdInputText(&swkbd, inputBuf, sizeof(inputBuf));
    if (button != SWKBD_BUTTON_NONE && inputBuf[0] != '\0') {
        char *hostStart = inputBuf;
        if (strncmp(hostStart, "https://", 8) == 0) hostStart += 8;
        else if (strncmp(hostStart, "http://", 7) == 0) hostStart += 7;

        size_t len = strlen(hostStart);
        while (len > 0 && (hostStart[len - 1] == '/' || hostStart[len - 1] == ' ' || hostStart[len - 1] == '\r' || hostStart[len - 1] == '\n')) {
            hostStart[len - 1] = '\0';
            len--;
        }

        if (len > 0) {
            snprintf(s_currentHost, sizeof(s_currentHost), "%s", hostStart);
            s_currentServerIndex = -1;
            invidious_search(s_currentHost, s_searchResults.query[0] ? s_searchResults.query : "3ds", &s_searchResults);
        }
    }
}

/**
 * Helper: Check if touch coordinates fall within a rectangular button area.
 */
static inline bool is_touch_inside(touchPosition touch, int x, int y, int w, int h) {
    return (touch.px >= x && touch.px <= x + w && touch.py >= y && touch.py <= y + h);
}

/**
 * Launch playback for a selected video and arm the 5-second OSD banner timer.
 */
static void launch_video(const VideoMetadata *video) {
    if (!video) return;

    memcpy(&s_playback.currentVideo, video, sizeof(VideoMetadata));
    s_playback.isPlaying = true;
    s_playback.currentPositionSec = 0.0f;

    /* svcGetSystemTick() retrieves high-resolution ARM timer ticks */
    s_playback.launchTick = svcGetSystemTick();
    s_playback.showTitleBanner = true;
    s_playback.bannerOpacity = 1.0f;

    s_appState = STATE_PLAYBACK;

    /* Fetch full description and likes asynchronously or on demand */
    invidious_fetch_video_details(s_currentHost, video->videoId, &s_playback.currentVideo);
}

/**
 * RENDER: Top Screen (400x240)
 * Handles video view and the 5-second active title OSD banner.
 */
static void render_top_screen(void) {
    /* Clear top background */
    C2D_DrawRectSolid(0, 0, 0, SCREEN_TOP_WIDTH, SCREEN_TOP_HEIGHT, COLOR_BG);
    C2D_TextBufClear(s_topTextBuf);

    if (s_appState == STATE_PLAYBACK || s_appState == STATE_COMMENTS) {
        /* Render simulated video playback viewport (400x200 16:9 box) */
        C2D_DrawRectSolid(20, 10, 0, 360, 200, C2D_Color32(12, 12, 16, 255));

        /* Render mock video content/waves or frame indicator */
        C2D_DrawRectSolid(22, 12, 0, 356, 196, C2D_Color32(20, 24, 30, 255));

        /* Playback progress bar at bottom of top screen */
        float progress = 0.0f;
        if (s_playback.currentVideo.lengthSeconds > 0) {
            progress = s_playback.currentPositionSec / (float)s_playback.currentVideo.lengthSeconds;
            if (progress > 1.0f) progress = 1.0f;
        }
        C2D_DrawRectSolid(20, 206, 0, 360, 4, COLOR_SEEK_BG);
        C2D_DrawRectSolid(20, 206, 0, (int)(360.0f * progress), 4, COLOR_SEEK_FILL);

        /* Center Video ID & Channel info */
        char topInfoStr[128];
        snprintf(topInfoStr, sizeof(topInfoStr), "Channel: %s | ID: %s",
                 s_playback.currentVideo.author, s_playback.currentVideo.videoId);
        parse_text_font(&s_topTexts[0], s_fontUbuntuRegular, s_topTextBuf, topInfoStr);
        C2D_DrawText(&s_topTexts[0], C2D_WithColor, 30, 180, 0, 0.45f, 0.45f, COLOR_TEXT_MUTED);

        /* --------------------------------------------------------------------
         * 5-Second Title Banner OSD Requirement:
         * Display video title for 5 seconds after launching the video.
         * -------------------------------------------------------------------- */
        if (s_playback.showTitleBanner) {
            u64 currentTick = svcGetSystemTick();
            u64 elapsedTicks = currentTick - s_playback.launchTick;

            if (elapsedTicks < OSD_DURATION_TICKS) {
                /* Calculate fade out during the last 0.5s */
                u64 fadeStart = OSD_DURATION_TICKS - (268123480ULL / 2);
                float alpha = 1.0f;
                if (elapsedTicks > fadeStart) {
                    alpha = 1.0f - ((float)(elapsedTicks - fadeStart) / (float)(268123480ULL / 2));
                    if (alpha < 0.0f) alpha = 0.0f;
                }
                u8 alphaByte = (u8)(220.0f * alpha);
                u8 textAlpha = (u8)(255.0f * alpha);

                /* Render sleek translucent banner across top of video */
                C2D_DrawRectSolid(20, 10, 0, 360, 48, C2D_Color32(10, 10, 16, alphaByte));
                C2D_DrawRectSolid(20, 56, 0, 360, 2, C2D_Color32(230, 33, 23, alphaByte));

                /* Video Title (rendered with Andika Bold TTF) */
                parse_text_font(&s_topTexts[2], s_fontAndikaBold, s_topTextBuf, s_playback.currentVideo.title);
                C2D_DrawText(&s_topTexts[2], C2D_WithColor, 28, 16, 0, 0.52f, 0.52f,
                             C2D_Color32(255, 255, 255, textAlpha));

                /* Author and duration (rendered with Ubuntu Regular TTF) */
                char bannerSubStr[96];
                int mins = s_playback.currentVideo.lengthSeconds / 60;
                int secs = s_playback.currentVideo.lengthSeconds % 60;
                snprintf(bannerSubStr, sizeof(bannerSubStr), "%s * %02d:%02d",
                         s_playback.currentVideo.author, mins, secs);
                parse_text_font(&s_topTexts[3], s_fontUbuntuRegular, s_topTextBuf, bannerSubStr);
                C2D_DrawText(&s_topTexts[3], C2D_WithColor, 28, 36, 0, 0.42f, 0.42f,
                             C2D_Color32(200, 200, 210, textAlpha));
            } else {
                s_playback.showTitleBanner = false;
            }
        }
    } else {
        /* STATE_SEARCH: Top screen header and instructions */
        C2D_DrawRectSolid(0, 0, 0, SCREEN_TOP_WIDTH, 40, COLOR_PANEL);
        C2D_DrawRectSolid(0, 38, 0, SCREEN_TOP_WIDTH, 2, COLOR_ACCENT);

        /* Main Header Title (rendered with Andika Bold TTF) */
        parse_text_font(&s_topTexts[0], s_fontAndikaBold, s_topTextBuf, "Citro 3DS - Invidious YouTube Client");
        C2D_DrawText(&s_topTexts[0], C2D_WithColor, 20, 10, 0, 0.65f, 0.65f, COLOR_TEXT_WHITE);

        /* Query Info (rendered with Ubuntu Regular TTF) */
        char searchInfo[256];
        snprintf(searchInfo, sizeof(searchInfo), "Query: \"%s\" (%d results from %s)",
                 s_searchResults.query, s_searchResults.count, s_currentHost);
        parse_text_font(&s_topTexts[1], s_fontUbuntuRegular, s_topTextBuf, searchInfo);
        C2D_DrawText(&s_topTexts[1], C2D_WithColor, 20, 50, 0, 0.45f, 0.45f, COLOR_ACCENT);

        /* Instructions list (rendered with Ubuntu Regular TTF) */
        parse_text_font(&s_topTexts[2], s_fontUbuntuRegular, s_topTextBuf, "* Touch bottom screen to select a video or use quick tags");
        C2D_DrawText(&s_topTexts[2], C2D_WithColor, 20, 80, 0, 0.45f, 0.45f, COLOR_TEXT_WHITE);

        parse_text_font(&s_topTexts[3], s_fontUbuntuRegular, s_topTextBuf, "* (X) Cycle Server | (Y) Custom Server | (A) Play Video");
        C2D_DrawText(&s_topTexts[3], C2D_WithColor, 20, 105, 0, 0.45f, 0.45f, COLOR_TEXT_MUTED);

        parse_text_font(&s_topTexts[4], s_fontUbuntuRegular, s_topTextBuf, "* Pure Invidious API • Open Protocol Streaming");
        C2D_DrawText(&s_topTexts[4], C2D_WithColor, 20, 130, 0, 0.45f, 0.45f, COLOR_TEXT_MUTED);

        /* Selected Video Preview Box */
        if (s_searchResults.count > 0 && s_selectedResultIndex < s_searchResults.count) {
            VideoMetadata *preview = &s_searchResults.items[s_selectedResultIndex];
            C2D_DrawRectSolid(20, 160, 0, 360, 68, COLOR_PANEL);
            C2D_DrawRectSolid(20, 160, 0, 3, 68, COLOR_ACCENT);

            /* Preview Title with Andika Bold TTF */
            parse_text_font(&s_topTexts[5], s_fontAndikaBold, s_topTextBuf, preview->title);
            C2D_DrawText(&s_topTexts[5], C2D_WithColor, 30, 166, 0, 0.50f, 0.50f, COLOR_TEXT_WHITE);

            /* Preview Subtitle with Ubuntu Regular TTF */
            char sub[128];
            snprintf(sub, sizeof(sub), "By %s | %d mins | %" PRId64 " views",
                     preview->author, preview->lengthSeconds / 60, preview->viewCount);
            parse_text_font(&s_topTexts[6], s_fontUbuntuRegular, s_topTextBuf, sub);
            C2D_DrawText(&s_topTexts[6], C2D_WithColor, 30, 192, 0, 0.42f, 0.42f, COLOR_TEXT_MUTED);
        }
    }

    /* ------------------------------------------------------------------------
     * Top-Right 3DS Hardware Battery Level Indicator
     * ------------------------------------------------------------------------ */
    /* Draw 3DS battery casing */
    int bx = 350;
    int by = 6;
    C2D_DrawRectSolid(bx, by, 0, 34, 14, C2D_Color32(40, 46, 36, 255));
    C2D_DrawRectSolid(bx + 34, by + 3, 0, 3, 8, C2D_Color32(70, 80, 60, 255));
    C2D_DrawRectSolid(bx + 2, by + 2, 0, 30, 10, C2D_Color32(14, 18, 14, 255));

    /* Select battery color: Lime for healthy/charging, Lemon for mid, Tangerine for low */
    u32 batColor = COLOR_LIME;
    if (s_battery.level <= 1) {
        batColor = COLOR_ACCENT; /* Low battery orange */
    } else if (s_battery.level <= 2) {
        batColor = COLOR_LEMON;  /* Mid battery lemon */
    }

    /* Draw filled battery bars */
    int fillWidth = (s_battery.level * 30) / 5;
    if (fillWidth > 30) fillWidth = 30;
    if (fillWidth > 0) {
        C2D_DrawRectSolid(bx + 2, by + 2, 0, fillWidth, 10, batColor);
    }

    /* Render percentage / charging text (rendered with Ubuntu Regular TTF) */
    char batStr[24];
    snprintf(batStr, sizeof(batStr), "%s%d%%", s_battery.isCharging ? "+" : "", s_battery.percent);
    parse_text_font(&s_topTexts[7], s_fontUbuntuRegular, s_topTextBuf, batStr);
    C2D_DrawText(&s_topTexts[7], C2D_WithColor, bx - 38, by + 1, 0, 0.38f, 0.38f, COLOR_TEXT_MUTED);
}

/**
 * RENDER: Bottom Screen (320x240 Touchscreen)
 * Renders touch controls, search list, seek bar, likes, and description viewer.
 */
static void render_bottom_screen(void) {
    C2D_DrawRectSolid(0, 0, 0, SCREEN_BOTTOM_WIDTH, SCREEN_BOTTOM_HEIGHT, COLOR_BG);
    C2D_TextBufClear(s_bottomTextBuf);

    if (s_appState == STATE_PLAYBACK) {
        /* --------------------------------------------------------------------
         * Playback Touch Controls (Seek bar, Play/Pause, Likes, Description)
         * -------------------------------------------------------------------- */
        /* Top Navigation Header */
        C2D_DrawRectSolid(0, 0, 0, SCREEN_BOTTOM_WIDTH, 34, COLOR_PANEL);

        /* "< Search" Back Button: (10, 5, 80, 24) */
        C2D_DrawRectSolid(8, 5, 0, 75, 24, COLOR_PANEL_ALT);
        parse_text_font(&s_bottomTexts[0], s_fontUbuntuBold, s_bottomTextBuf, "< Search");
        C2D_DrawText(&s_bottomTexts[0], C2D_WithColor, 14, 9, 0, 0.42f, 0.42f, COLOR_TEXT_WHITE);

        /* Likes display with Material Icon thumb_up: (180, 5, 130, 24) */
        char likesStr[64];
        if (s_playback.currentVideo.likeCount > 0) {
            snprintf(likesStr, sizeof(likesStr), "%s %ld Likes",
                     s_fontMaterialIcons ? ICON_THUMB_UP : "[+]",
                     (long)s_playback.currentVideo.likeCount);
        } else {
            snprintf(likesStr, sizeof(likesStr), "%s Like Video",
                     s_fontMaterialIcons ? ICON_THUMB_UP : "[+]");
        }
        C2D_DrawRectSolid(200, 5, 0, 112, 24, COLOR_PANEL_ALT);
        parse_text_font(&s_bottomTexts[1], s_fontUbuntuBold, s_bottomTextBuf, likesStr);
        C2D_DrawText(&s_bottomTexts[1], C2D_WithColor, 208, 9, 0, 0.42f, 0.42f, COLOR_TEXT_WHITE);

        /* Video Title on Touch Screen (Andika Bold TTF) */
        parse_text_font(&s_bottomTexts[2], s_fontAndikaBold, s_bottomTextBuf, s_playback.currentVideo.title);
        C2D_DrawText(&s_bottomTexts[2], C2D_WithColor, 10, 42, 0, 0.46f, 0.46f, COLOR_TEXT_WHITE);

        /* Author and Views (Ubuntu Regular TTF) */
        char metaStr[96];
        snprintf(metaStr, sizeof(metaStr), "%s * %" PRId64 " views",
                 s_playback.currentVideo.author, s_playback.currentVideo.viewCount);
        parse_text_font(&s_bottomTexts[3], s_fontUbuntuRegular, s_bottomTextBuf, metaStr);
        C2D_DrawText(&s_bottomTexts[3], C2D_WithColor, 10, 62, 0, 0.40f, 0.40f, COLOR_TEXT_MUTED);

        /* --------------------------------------------------------------------
         * Interactive Touch Seek Bar: (20, 90, 280, 16)
         * -------------------------------------------------------------------- */
        C2D_DrawRectSolid(20, 92, 0, 280, 12, COLOR_SEEK_BG);
        float progress = 0.0f;
        if (s_playback.currentVideo.lengthSeconds > 0) {
            progress = s_playback.currentPositionSec / (float)s_playback.currentVideo.lengthSeconds;
            if (progress > 1.0f) progress = 1.0f;
        }
        int fillWidth = (int)(280.0f * progress);
        C2D_DrawRectSolid(20, 92, 0, fillWidth, 12, COLOR_SEEK_FILL);
        /* Scrub knob handle */
        C2D_DrawRectSolid(20 + fillWidth - 3, 89, 0, 8, 18, COLOR_TEXT_WHITE);

        /* Time indicators: (Current / Total) */
        char timeStr[64];
        int curM = (int)s_playback.currentPositionSec / 60;
        int curS = (int)s_playback.currentPositionSec % 60;
        int totM = s_playback.currentVideo.lengthSeconds / 60;
        int totS = s_playback.currentVideo.lengthSeconds % 60;
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d / %02d:%02d", curM, curS, totM, totS);
        parse_text_font(&s_bottomTexts[4], s_fontUbuntuRegular, s_bottomTextBuf, timeStr);
        C2D_DrawText(&s_bottomTexts[4], C2D_WithColor, 20, 112, 0, 0.40f, 0.40f, COLOR_TEXT_MUTED);

        /* --------------------------------------------------------------------
         * Touch Transport Controls (with Material Icons TTF)
         * -------------------------------------------------------------------- */
        /* Rewind 10s: (20, 140, 60, 42) */
        C2D_DrawRectSolid(20, 140, 0, 60, 42, COLOR_PANEL);
        const char *rewindLabel = s_fontMaterialIcons ? ICON_REPLAY_10 " 10s" : "-10s";
        parse_text_font(&s_bottomTexts[5], s_fontUbuntuBold, s_bottomTextBuf, rewindLabel);
        C2D_DrawText(&s_bottomTexts[5], C2D_WithColor, 26, 152, 0, 0.42f, 0.42f, COLOR_TEXT_WHITE);

        /* Big Play/Pause Button: (95, 135, 130, 52) */
        C2D_DrawRectSolid(95, 135, 0, 130, 52, s_playback.isPlaying ? COLOR_ACCENT : COLOR_BUTTON_BLUE);
        char btnLabel[64];
        if (s_fontMaterialIcons) {
            snprintf(btnLabel, sizeof(btnLabel), "%s %s",
                     s_playback.isPlaying ? ICON_PAUSE : ICON_PLAY,
                     s_playback.isPlaying ? "PAUSE" : "PLAY");
        } else {
            snprintf(btnLabel, sizeof(btnLabel), "%s",
                     s_playback.isPlaying ? "PAUSE [||]" : "PLAY [>]");
        }
        parse_text_font(&s_bottomTexts[6], s_fontUbuntuBold, s_bottomTextBuf, btnLabel);
        C2D_DrawText(&s_bottomTexts[6], C2D_WithColor, 116, 150, 0, 0.52f, 0.52f, COLOR_TEXT_WHITE);

        /* Fast Forward 10s: (240, 140, 60, 42) */
        C2D_DrawRectSolid(240, 140, 0, 60, 42, COLOR_PANEL);
        const char *ffLabel = s_fontMaterialIcons ? ICON_FORWARD_10 " 10s" : "+10s";
        parse_text_font(&s_bottomTexts[7], s_fontUbuntuBold, s_bottomTextBuf, ffLabel);
        C2D_DrawText(&s_bottomTexts[7], C2D_WithColor, 246, 152, 0, 0.42f, 0.42f, COLOR_TEXT_WHITE);

        /* Description Toggle Button: (20, 196, 280, 36) */
        C2D_DrawRectSolid(20, 196, 0, 280, 36, COLOR_PANEL_ALT);
        parse_text_font(&s_bottomTexts[8], s_fontUbuntuBold, s_bottomTextBuf, "=== View Video Description ===");
        C2D_DrawText(&s_bottomTexts[8], C2D_WithColor, 55, 206, 0, 0.45f, 0.45f, COLOR_TEXT_WHITE);

    } else if (s_appState == STATE_COMMENTS) {
        /* Description and Details Fullscreen View */
        C2D_DrawRectSolid(0, 0, 0, SCREEN_BOTTOM_WIDTH, 34, COLOR_PANEL);

        /* "< Back to Player" Button: (10, 5, 120, 24) */
        C2D_DrawRectSolid(8, 5, 0, 115, 24, COLOR_PANEL_ALT);
        parse_text_font(&s_bottomTexts[0], s_fontUbuntuBold, s_bottomTextBuf, "< Back to Player");
        C2D_DrawText(&s_bottomTexts[0], C2D_WithColor, 14, 9, 0, 0.42f, 0.42f, COLOR_TEXT_WHITE);

        /* Description title (Andika Bold TTF) */
        parse_text_font(&s_bottomTexts[1], s_fontAndikaBold, s_bottomTextBuf, "Description");
        C2D_DrawText(&s_bottomTexts[1], C2D_WithColor, 140, 9, 0, 0.48f, 0.48f, COLOR_ACCENT);

        /* Render multi-line description snippet (Ubuntu Regular TTF) */
        C2D_DrawRectSolid(10, 42, 0, 300, 150, COLOR_PANEL);
        const char *desc = s_playback.currentVideo.description;
        if (!desc || strlen(desc) == 0) {
            desc = "No description provided for this video.";
        }
        parse_text_font(&s_bottomTexts[2], s_fontUbuntuRegular, s_bottomTextBuf, desc);
        C2D_DrawText(&s_bottomTexts[2], C2D_WithColor, 18, 48 - s_descScrollOffset, 0, 0.40f, 0.40f, COLOR_TEXT_WHITE);

        /* Scroll Up / Down Touch Buttons */
        C2D_DrawRectSolid(10, 200, 0, 145, 32, COLOR_PANEL_ALT);
        const char *scrollUpLabel = s_fontMaterialIcons ? ICON_ARROW_UP " Scroll Up" : "[^] Scroll Up";
        parse_text_font(&s_bottomTexts[3], s_fontUbuntuBold, s_bottomTextBuf, scrollUpLabel);
        C2D_DrawText(&s_bottomTexts[3], C2D_WithColor, 35, 208, 0, 0.42f, 0.42f, COLOR_TEXT_WHITE);

        C2D_DrawRectSolid(165, 200, 0, 145, 32, COLOR_PANEL_ALT);
        const char *scrollDownLabel = s_fontMaterialIcons ? ICON_ARROW_DOWN " Scroll Down" : "[v] Scroll Down";
        parse_text_font(&s_bottomTexts[4], s_fontUbuntuBold, s_bottomTextBuf, scrollDownLabel);
        C2D_DrawText(&s_bottomTexts[4], C2D_WithColor, 185, 208, 0, 0.42f, 0.42f, COLOR_TEXT_WHITE);

    } else {
        /* STATE_SEARCH: Invidious Search Results List */
        C2D_DrawRectSolid(0, 0, 0, SCREEN_BOTTOM_WIDTH, 36, COLOR_PANEL);

        /* Quick Search Tag 1: "3DS" (8, 6, 70, 24) */
        C2D_DrawRectSolid(8, 6, 0, 70, 24, COLOR_ACCENT);
        parse_text_font(&s_bottomTexts[0], s_fontAndikaRegular, s_bottomTextBuf, "\"3ds\"");
        C2D_DrawText(&s_bottomTexts[0], C2D_WithColor, 26, 10, 0, 0.42f, 0.42f, COLOR_TEXT_WHITE);

        /* Quick Search Tag 2: "Homebrew" (84, 6, 95, 24) */
        C2D_DrawRectSolid(84, 6, 0, 95, 24, COLOR_PANEL_ALT);
        parse_text_font(&s_bottomTexts[1], s_fontAndikaRegular, s_bottomTextBuf, "\"homebrew\"");
        C2D_DrawText(&s_bottomTexts[1], C2D_WithColor, 94, 10, 0, 0.42f, 0.42f, COLOR_TEXT_WHITE);

        /* Quick Search Tag 3: "Chiptune" (185, 6, 85, 24) */
        C2D_DrawRectSolid(185, 6, 0, 85, 24, COLOR_PANEL_ALT);
        parse_text_font(&s_bottomTexts[2], s_fontAndikaRegular, s_bottomTextBuf, "\"chiptune\"");
        C2D_DrawText(&s_bottomTexts[2], C2D_WithColor, 195, 10, 0, 0.42f, 0.42f, COLOR_TEXT_WHITE);

        /* Refresh Button: (275, 6, 38, 24) */
        C2D_DrawRectSolid(275, 6, 0, 38, 24, COLOR_BUTTON_BLUE);
        const char *refreshLabel = s_fontMaterialIcons ? ICON_REFRESH : "[R]";
        parse_text_font(&s_bottomTexts[3], s_fontMaterialIcons ? s_fontMaterialIcons : s_fontUbuntuBold, s_bottomTextBuf, refreshLabel);
        C2D_DrawText(&s_bottomTexts[3], C2D_WithColor, 285, 10, 0, 0.42f, 0.42f, COLOR_TEXT_WHITE);

        if (s_searchResults.count == 0) {
            /* Draw helpful status panel when 0 results or connecting */
            C2D_DrawRectSolid(10, 44, 0, 300, 160, COLOR_PANEL);
            C2D_DrawRectSolid(10, 44, 0, 4, 160, COLOR_ACCENT);

            if (s_searchResults.isLoading) {
                parse_text_font(&s_bottomTexts[4], s_fontAndikaBold, s_bottomTextBuf, "Connecting to Invidious...");
                C2D_DrawText(&s_bottomTexts[4], C2D_WithColor, 20, 56, 0, 0.44f, 0.44f, COLOR_ACCENT);

                parse_text_font(&s_bottomTexts[5], s_fontUbuntuRegular, s_bottomTextBuf, "Fetching video results, please wait...");
                C2D_DrawText(&s_bottomTexts[5], C2D_WithColor, 20, 80, 0, 0.38f, 0.38f, COLOR_TEXT_MUTED);
            } else {
                parse_text_font(&s_bottomTexts[4], s_fontAndikaBold, s_bottomTextBuf, "Invidious Status:");
                C2D_DrawText(&s_bottomTexts[4], C2D_WithColor, 20, 56, 0, 0.44f, 0.44f, COLOR_ACCENT);

                char statusDetail[128];
                if (s_searchResults.lastError[0] != '\0') {
                    snprintf(statusDetail, sizeof(statusDetail), "%s", s_searchResults.lastError);
                } else if (s_searchResults.lastHttpStatus > 0) {
                    snprintf(statusDetail, sizeof(statusDetail), "Server returned HTTP %d", s_searchResults.lastHttpStatus);
                } else {
                    snprintf(statusDetail, sizeof(statusDetail), "0 videos found. Tap [R] to retry.");
                }
                parse_text_font(&s_bottomTexts[5], s_fontUbuntuRegular, s_bottomTextBuf, statusDetail);
                C2D_DrawText(&s_bottomTexts[5], C2D_WithColor, 20, 80, 0, 0.38f, 0.38f, COLOR_TEXT_WHITE);

                parse_text_font(&s_bottomTexts[6], s_fontUbuntuRegular, s_bottomTextBuf, "* Press (X) to cycle server instance");
                C2D_DrawText(&s_bottomTexts[6], C2D_WithColor, 20, 110, 0, 0.38f, 0.38f, COLOR_TEXT_MUTED);

                parse_text_font(&s_bottomTexts[7], s_fontUbuntuRegular, s_bottomTextBuf, "* Press (Y) to type custom server");
                C2D_DrawText(&s_bottomTexts[7], C2D_WithColor, 20, 134, 0, 0.38f, 0.38f, COLOR_TEXT_MUTED);

                parse_text_font(&s_bottomTexts[8], s_fontUbuntuRegular, s_bottomTextBuf, "* Touch tags [\"3ds\", \"homebrew\"] or [R]");
                C2D_DrawText(&s_bottomTexts[8], C2D_WithColor, 20, 158, 0, 0.38f, 0.38f, COLOR_TEXT_MUTED);
            }
        } else {
            /* Render up to 4 search result item rows */
            int startY = 38;
            int rowHeight = 40;
            for (int i = 0; i < 4 && i < s_searchResults.count; i++) {
                VideoMetadata *vid = &s_searchResults.items[i];
                u32 rowColor = (i == s_selectedResultIndex) ? COLOR_PANEL_ALT : COLOR_PANEL;
                int y = startY + (i * (rowHeight + 3));

                C2D_DrawRectSolid(6, y, 0, 308, rowHeight, rowColor);

                if (i == s_selectedResultIndex) {
                    C2D_DrawRectSolid(6, y, 0, 4, rowHeight, COLOR_ACCENT);
                }

                /* Video Title (Andika Bold TTF) */
                parse_text_font(&s_bottomTexts[4 + (i * 2)], s_fontAndikaBold, s_bottomTextBuf, vid->title);
                C2D_DrawText(&s_bottomTexts[4 + (i * 2)], C2D_WithColor, 16, y + 2, 0, 0.40f, 0.40f, COLOR_TEXT_WHITE);

                /* Subtitle: Author and length (Ubuntu Regular TTF) */
                char rowSub[96];
                snprintf(rowSub, sizeof(rowSub), "%s * %d:%02d * Touch to Play",
                         vid->author, vid->lengthSeconds / 60, vid->lengthSeconds % 60);
                parse_text_font(&s_bottomTexts[5 + (i * 2)], s_fontUbuntuRegular, s_bottomTextBuf, rowSub);
                C2D_DrawText(&s_bottomTexts[5 + (i * 2)], C2D_WithColor, 16, y + 20, 0, 0.34f, 0.34f, COLOR_TEXT_MUTED);
            }
        }

        /* Server Selection Bar at bottom (6, 212, 308, 24) */
        C2D_DrawRectSolid(6, 212, 0, 308, 24, COLOR_PANEL);
        C2D_DrawRectSolid(6, 212, 0, 3, 24, COLOR_ACCENT);

        char serverLabel[192];
        snprintf(serverLabel, sizeof(serverLabel), "%sServer: %s [Tap/X/Y]",
                 s_fontMaterialIcons ? ICON_DNS " " : "", s_currentHost);
        parse_text_font(&s_bottomTexts[16], s_fontUbuntuRegular, s_bottomTextBuf, serverLabel);
        C2D_DrawText(&s_bottomTexts[16], C2D_WithColor, 14, 216, 0, 0.36f, 0.36f, COLOR_TEXT_WHITE);
    }
}

/**
 * Handle Touch Screen Inputs and Navigation
 */
static void handle_touch_input(touchPosition touch) {
    if (s_appState == STATE_PLAYBACK) {
        /* Back to Search: (8, 5, 75, 24) */
        if (is_touch_inside(touch, 8, 5, 75, 24)) {
            s_appState = STATE_SEARCH;
            return;
        }

        /* Likes Button: (200, 5, 112, 24) */
        if (is_touch_inside(touch, 200, 5, 112, 24)) {
            s_playback.currentVideo.likeCount++;
            return;
        }

        /* Touch Seek Bar: (20, 85, 280, 26) */
        if (is_touch_inside(touch, 20, 85, 280, 26)) {
            float frac = (float)(touch.px - 20) / 280.0f;
            if (frac < 0.0f) frac = 0.0f;
            if (frac > 1.0f) frac = 1.0f;
            s_playback.currentPositionSec = frac * (float)s_playback.currentVideo.lengthSeconds;
            return;
        }

        /* Rewind 10s: (20, 140, 60, 42) */
        if (is_touch_inside(touch, 20, 140, 60, 42)) {
            s_playback.currentPositionSec -= 10.0f;
            if (s_playback.currentPositionSec < 0.0f) s_playback.currentPositionSec = 0.0f;
            return;
        }

        /* Play/Pause Button: (95, 135, 130, 52) */
        if (is_touch_inside(touch, 95, 135, 130, 52)) {
            s_playback.isPlaying = !s_playback.isPlaying;
            return;
        }

        /* Fast Forward 10s: (240, 140, 60, 42) */
        if (is_touch_inside(touch, 240, 140, 60, 42)) {
            s_playback.currentPositionSec += 10.0f;
            if (s_playback.currentPositionSec > (float)s_playback.currentVideo.lengthSeconds) {
                s_playback.currentPositionSec = (float)s_playback.currentVideo.lengthSeconds;
            }
            return;
        }

        /* View Description Button: (20, 196, 280, 36) */
        if (is_touch_inside(touch, 20, 196, 280, 36)) {
            s_appState = STATE_COMMENTS;
            s_descScrollOffset = 0;
            return;
        }

    } else if (s_appState == STATE_COMMENTS) {
        /* Back to Player: (8, 5, 115, 24) */
        if (is_touch_inside(touch, 8, 5, 115, 24)) {
            s_appState = STATE_PLAYBACK;
            return;
        }

        /* Scroll Up: (10, 200, 145, 32) */
        if (is_touch_inside(touch, 10, 200, 145, 32)) {
            s_descScrollOffset -= 24;
            if (s_descScrollOffset < 0) s_descScrollOffset = 0;
            return;
        }

        /* Scroll Down: (165, 200, 145, 32) */
        if (is_touch_inside(touch, 165, 200, 145, 32)) {
            s_descScrollOffset += 24;
            return;
        }

    } else {
        /* STATE_SEARCH: Quick tags */
        if (is_touch_inside(touch, 8, 6, 70, 24)) {
            invidious_search(s_currentHost, "3ds", &s_searchResults);
            return;
        }
        if (is_touch_inside(touch, 84, 6, 95, 24)) {
            invidious_search(s_currentHost, "homebrew", &s_searchResults);
            return;
        }
        if (is_touch_inside(touch, 185, 6, 85, 24)) {
            invidious_search(s_currentHost, "chiptune", &s_searchResults);
            return;
        }
        if (is_touch_inside(touch, 275, 6, 38, 24)) {
            invidious_search(s_currentHost, s_searchResults.query, &s_searchResults);
            return;
        }

        /* Select video item from touch list */
        int startY = 38;
        int rowHeight = 40;
        for (int i = 0; i < 4 && i < s_searchResults.count; i++) {
            int y = startY + (i * (rowHeight + 3));
            if (is_touch_inside(touch, 6, y, 308, rowHeight)) {
                s_selectedResultIndex = i;
                launch_video(&s_searchResults.items[i]);
                return;
            }
        }

        /* Touch on Server bar (6, 212, 308, 24): prompt custom server or cycle */
        if (is_touch_inside(touch, 6, 212, 308, 24)) {
            prompt_custom_server();
            return;
        }
    }
}

/**
 * Main Application Entry Point
 */
int main(int argc, char **argv) {
    /* ------------------------------------------------------------------------
     * 1. 3DS Hardware and Graphics Initialization
     * ------------------------------------------------------------------------ */
    /* Initialize default framebuffer and GPU graphics pipeline */
    gfxInitDefault();

    /* Initialize Citro3D command buffer (underlying PICA200 driver) */
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

    /* Initialize Citro2D 2D graphics engine (quad batching & text rendering) */
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    /* Create hardware render targets for Top (400x240) and Bottom (320x240) screens */
    C3D_RenderTarget *topTarget = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget *bottomTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    /* Pre-allocate separate citro2d text buffers for Top and Bottom screens */
    s_topTextBuf = C2D_TextBufNew(4096);
    s_bottomTextBuf = C2D_TextBufNew(4096);

    /* Initialize 3DS RomFS virtual archive to load custom TTF fonts and icons */
    bool romfsMounted = R_SUCCEEDED(romfsInit());

    /* Load TTF fonts (Ubuntu & Andika) and Material Icons */
    s_fontUbuntuRegular = load_3ds_font("fonts", "Ubuntu-R.ttf");
    s_fontUbuntuBold    = load_3ds_font("fonts", "Ubuntu-B.ttf");
    s_fontAndikaRegular = load_3ds_font("fonts", "Andika-Regular.ttf");
    s_fontAndikaBold    = load_3ds_font("fonts", "Andika-Bold.ttf");
    s_fontMaterialIcons = load_3ds_font("icons", "MaterialIcons-Regular.ttf");

    /* ------------------------------------------------------------------------
     * 2. 3DS SOC (Socket Service) and Network Initialization
     * ------------------------------------------------------------------------ */
    /* SOC buffer MUST be aligned to 0x1000 boundary (3DS MMU page size) */
    s_socBuffer = (u32 *)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (!s_socBuffer) {
        /* Failed to allocate socket buffer */
        goto cleanup_gfx;
    }

    /* socInit registers memory buffer with the 3DS OS kernel socket service */
    Result socRes = socInit(s_socBuffer, SOC_BUFFERSIZE);
    if (R_FAILED(socRes)) {
        /* SOC failed (e.g. WiFi switched off); continue gracefully */
    }

    /* Initialize Invidious HTTP client context */
    invidious_init();

    /* Initialize 3DS PTM battery monitoring service */
    citro_battery_init();
    citro_battery_update(&s_battery);
    u32 frameCounter = 0;

    /* ------------------------------------------------------------------------
     * 3. Initial Keyless Invidious API Query
     * ------------------------------------------------------------------------ */
    memset(&s_searchResults, 0, sizeof(SearchResults));
    memset(&s_playback, 0, sizeof(PlaybackState));

    /* Query public Invidious instance without API keys */
    invidious_search(s_currentHost, "3ds", &s_searchResults);

    /* ------------------------------------------------------------------------
     * 4. Main Event & Render Loop (Locked to 60 FPS VSync)
     * ------------------------------------------------------------------------ */
    while (aptMainLoop()) {
        /* Scan input hardware buttons and touchscreen state */
        hidScanInput();
        u32 kDown = hidKeysDown();

        /* Exit application on START button press */
        if (kDown & KEY_START) {
            break;
        }

        /* Server Switching via hardware buttons */
        if (kDown & KEY_X) {
            cycle_invidious_server();
        }
        if (kDown & KEY_Y) {
            prompt_custom_server();
        }

        /* Handle physical D-pad navigation */
        if (kDown & KEY_UP) {
            if (s_selectedResultIndex > 0) s_selectedResultIndex--;
        }
        if (kDown & KEY_DOWN) {
            if (s_selectedResultIndex < s_searchResults.count - 1) s_selectedResultIndex++;
        }
        if (kDown & KEY_A) {
            if (s_appState == STATE_SEARCH && s_searchResults.count > 0) {
                launch_video(&s_searchResults.items[s_selectedResultIndex]);
            } else if (s_appState == STATE_PLAYBACK) {
                s_playback.isPlaying = !s_playback.isPlaying;
            }
        }
        if (kDown & KEY_B) {
            if (s_appState == STATE_COMMENTS) {
                s_appState = STATE_PLAYBACK;
            } else if (s_appState == STATE_PLAYBACK) {
                s_appState = STATE_SEARCH;
            }
        }

        /* Read Touchscreen coordinates */
        if (kDown & KEY_TOUCH) {
            touchPosition touch;
            hidTouchRead(&touch);
            handle_touch_input(touch);
        }

        /* Advance simulated playback timer at 60 FPS (~0.0166s per frame) */
        if (s_playback.isPlaying && s_playback.currentVideo.lengthSeconds > 0) {
            s_playback.currentPositionSec += (1.0f / 60.0f);
            if (s_playback.currentPositionSec >= (float)s_playback.currentVideo.lengthSeconds) {
                s_playback.currentPositionSec = (float)s_playback.currentVideo.lengthSeconds;
                s_playback.isPlaying = false;
            }
        }

        /* Update battery status every 60 frames (~1 sec) */
        if (++frameCounter % 60 == 0) {
            citro_battery_update(&s_battery);
        }

        /* --------------------------------------------------------------------
         * Frame Rendering Begin
         * -------------------------------------------------------------------- */
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

        /* 1. Render Top Screen */
        C2D_SceneBegin(topTarget);
        C2D_TargetClear(topTarget, COLOR_BG);
        render_top_screen();

        /* 2. Render Bottom Screen */
        C2D_SceneBegin(bottomTarget);
        C2D_TargetClear(bottomTarget, COLOR_BG);
        render_bottom_screen();

        /* Swap framebuffers and present */
        C3D_FrameEnd(0);
    }

    /* ------------------------------------------------------------------------
     * 5. Clean Teardown & Resource Release
     * ------------------------------------------------------------------------ */
    citro_battery_exit();
    invidious_exit();

    /* Free custom TTF and Material Icon fonts */
    if (s_fontUbuntuRegular) C2D_FontFree(s_fontUbuntuRegular);
    if (s_fontUbuntuBold)    C2D_FontFree(s_fontUbuntuBold);
    if (s_fontAndikaRegular) C2D_FontFree(s_fontAndikaRegular);
    if (s_fontAndikaBold)    C2D_FontFree(s_fontAndikaBold);
    if (s_fontMaterialIcons) C2D_FontFree(s_fontMaterialIcons);

    if (romfsMounted) {
        romfsExit();
    }

    if (s_socBuffer) {
        socExit();
        free(s_socBuffer);
        s_socBuffer = NULL;
    }

    C2D_TextBufDelete(s_topTextBuf);
    C2D_TextBufDelete(s_bottomTextBuf);

cleanup_gfx:
    C2D_Fini();
    C3D_Fini();
    gfxExit();

    return 0;
}
