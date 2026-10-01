/**
 * ============================================================================
 * Citro - Lightweight Invidious YouTube Client for Nintendo 3DS
 * File: main.c
 * ----------------------------------------------------------------------------
 * SPDX-License-Identifier: GPL-3.0-or-later OR Apache-2.0 OR CC-BY-SA-4.0
 * Licensed under GPL-3.0, Apache-2.0, and CC-BY-SA-4.0.
 *
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

/* Colors in RGBA8 format for citro2d */
#define COLOR_BG               C2D_Color32(24, 24, 28, 255)
#define COLOR_PANEL            C2D_Color32(36, 36, 44, 255)
#define COLOR_PANEL_ALT        C2D_Color32(48, 48, 58, 255)
#define COLOR_ACCENT           C2D_Color32(230, 33, 23, 255)   /* YouTube Red */
#define COLOR_ACCENT_HOVER     C2D_Color32(255, 60, 50, 255)
#define COLOR_TEXT_WHITE       C2D_Color32(240, 240, 240, 255)
#define COLOR_TEXT_MUTED       C2D_Color32(160, 160, 170, 255)
#define COLOR_SEEK_BG          C2D_Color32(60, 60, 70, 255)
#define COLOR_SEEK_FILL        C2D_Color32(230, 33, 23, 255)
#define COLOR_BANNER_BG        C2D_Color32(10, 10, 14, 220)
#define COLOR_BUTTON_BLUE      C2D_Color32(33, 150, 243, 255)

/* Pre-allocated static buffers to avoid dynamic allocation during 60FPS render loop */
static u32 *s_socBuffer = NULL;
static C2D_TextBuf s_staticTextBuf;
static C2D_Text s_textObjects[32];

/* Global Application State */
static AppState      s_appState = STATE_SEARCH;
static SearchResults s_searchResults;
static PlaybackState s_playback;
static int           s_selectedResultIndex = 0;
static int           s_descScrollOffset = 0;

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
    invidious_fetch_video_details(DEFAULT_INVIDIOUS_HOST, video->videoId, &s_playback.currentVideo);
}

/**
 * RENDER: Top Screen (400x240)
 * Handles video view and the 5-second active title OSD banner.
 */
static void render_top_screen(void) {
    /* Clear top background */
    C2D_DrawRectSolid(0, 0, 0, SCREEN_TOP_WIDTH, SCREEN_TOP_HEIGHT, COLOR_BG);

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
        C2D_TextBufClear(s_staticTextBuf);
        char topInfoStr[128];
        snprintf(topInfoStr, sizeof(topInfoStr), "Channel: %s | ID: %s",
                 s_playback.currentVideo.author, s_playback.currentVideo.videoId);
        C2D_TextParse(&s_textObjects[0], s_staticTextBuf, topInfoStr);
        C2D_TextOptimize(&s_textObjects[0]);
        C2D_DrawText(&s_textObjects[0], C2D_WithColor, 30, 180, 0, 0.45f, 0.45f, COLOR_TEXT_MUTED);

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

                /* Video Title */
                C2D_TextParse(&s_textObjects[2], s_staticTextBuf, s_playback.currentVideo.title);
                C2D_TextOptimize(&s_textObjects[2]);
                C2D_DrawText(&s_textObjects[2], C2D_WithColor, 28, 16, 0, 0.52f, 0.52f,
                             C2D_Color32(255, 255, 255, textAlpha));

                /* Author and duration */
                char bannerSubStr[96];
                int mins = s_playback.currentVideo.lengthSeconds / 60;
                int secs = s_playback.currentVideo.lengthSeconds % 60;
                snprintf(bannerSubStr, sizeof(bannerSubStr), "%s * %02d:%02d",
                         s_playback.currentVideo.author, mins, secs);
                C2D_TextParse(&s_textObjects[3], s_staticTextBuf, bannerSubStr);
                C2D_TextOptimize(&s_textObjects[3]);
                C2D_DrawText(&s_textObjects[3], C2D_WithColor, 28, 36, 0, 0.42f, 0.42f,
                             C2D_Color32(200, 200, 210, textAlpha));
            } else {
                s_playback.showTitleBanner = false;
            }
        }
    } else {
        /* STATE_SEARCH: Top screen header and instructions */
        C2D_DrawRectSolid(0, 0, 0, SCREEN_TOP_WIDTH, 40, COLOR_PANEL);
        C2D_DrawRectSolid(0, 38, 0, SCREEN_TOP_WIDTH, 2, COLOR_ACCENT);

        C2D_TextBufClear(s_staticTextBuf);
        C2D_TextParse(&s_textObjects[0], s_staticTextBuf, "Citro 3DS - Invidious YouTube Client");
        C2D_TextOptimize(&s_textObjects[0]);
        C2D_DrawText(&s_textObjects[0], C2D_WithColor, 20, 10, 0, 0.65f, 0.65f, COLOR_TEXT_WHITE);

        /* Query Info */
        char searchInfo[128];
        snprintf(searchInfo, sizeof(searchInfo), "Query: \"%s\" (%d results from %s)",
                 s_searchResults.query, s_searchResults.count, DEFAULT_INVIDIOUS_HOST);
        C2D_TextParse(&s_textObjects[1], s_staticTextBuf, searchInfo);
        C2D_TextOptimize(&s_textObjects[1]);
        C2D_DrawText(&s_textObjects[1], C2D_WithColor, 20, 50, 0, 0.45f, 0.45f, COLOR_ACCENT);

        /* Instructions list */
        C2D_TextParse(&s_textObjects[2], s_staticTextBuf, "* Touch bottom screen to select a video or use quick tags");
        C2D_DrawText(&s_textObjects[2], C2D_WithColor, 20, 80, 0, 0.45f, 0.45f, COLOR_TEXT_WHITE);

        C2D_TextParse(&s_textObjects[3], s_staticTextBuf, "* Press (A) to play selected video, (START) to exit");
        C2D_DrawText(&s_textObjects[3], C2D_WithColor, 20, 105, 0, 0.45f, 0.45f, COLOR_TEXT_MUTED);

        C2D_TextParse(&s_textObjects[4], s_staticTextBuf, "* No API keys or Google accounts needed (Pure Invidious API)");
        C2D_DrawText(&s_textObjects[4], C2D_WithColor, 20, 130, 0, 0.45f, 0.45f, COLOR_TEXT_MUTED);

        /* Selected Video Preview Box */
        if (s_searchResults.count > 0 && s_selectedResultIndex < s_searchResults.count) {
            VideoMetadata *preview = &s_searchResults.items[s_selectedResultIndex];
            C2D_DrawRectSolid(20, 160, 0, 360, 68, COLOR_PANEL);
            C2D_DrawRectSolid(20, 160, 0, 3, 68, COLOR_ACCENT);

            C2D_TextParse(&s_textObjects[5], s_staticTextBuf, preview->title);
            C2D_DrawText(&s_textObjects[5], C2D_WithColor, 30, 166, 0, 0.50f, 0.50f, COLOR_TEXT_WHITE);

            char sub[128];
            snprintf(sub, sizeof(sub), "By %s | %d mins | %" PRId64 " views",
                     preview->author, preview->lengthSeconds / 60, preview->viewCount);
            C2D_TextParse(&s_textObjects[6], s_staticTextBuf, sub);
            C2D_DrawText(&s_textObjects[6], C2D_WithColor, 30, 192, 0, 0.42f, 0.42f, COLOR_TEXT_MUTED);
        }
    }
}

/**
 * RENDER: Bottom Screen (320x240 Touchscreen)
 * Renders touch controls, search list, seek bar, likes, and description viewer.
 */
static void render_bottom_screen(void) {
    C2D_DrawRectSolid(0, 0, 0, SCREEN_BOTTOM_WIDTH, SCREEN_BOTTOM_HEIGHT, COLOR_BG);
    C2D_TextBufClear(s_staticTextBuf);

    if (s_appState == STATE_PLAYBACK) {
        /* --------------------------------------------------------------------
         * Playback Touch Controls (Seek bar, Play/Pause, Likes, Description)
         * -------------------------------------------------------------------- */
        /* Top Navigation Header */
        C2D_DrawRectSolid(0, 0, 0, SCREEN_BOTTOM_WIDTH, 34, COLOR_PANEL);

        /* "< Search" Back Button: (10, 5, 80, 24) */
        C2D_DrawRectSolid(8, 5, 0, 75, 24, COLOR_PANEL_ALT);
        C2D_TextParse(&s_textObjects[0], s_staticTextBuf, "< Search");
        C2D_DrawText(&s_textObjects[0], C2D_WithColor, 14, 9, 0, 0.42f, 0.42f, COLOR_TEXT_WHITE);

        /* Likes display with thumb icon: (180, 5, 130, 24) */
        char likesStr[32];
        if (s_playback.currentVideo.likeCount > 0) {
            snprintf(likesStr, sizeof(likesStr), "[+] %d Likes", s_playback.currentVideo.likeCount);
        } else {
            snprintf(likesStr, sizeof(likesStr), "[+] Like Video");
        }
        C2D_DrawRectSolid(200, 5, 0, 112, 24, COLOR_PANEL_ALT);
        C2D_TextParse(&s_textObjects[1], s_staticTextBuf, likesStr);
        C2D_DrawText(&s_textObjects[1], C2D_WithColor, 208, 9, 0, 0.42f, 0.42f, COLOR_TEXT_WHITE);

        /* Video Title on Touch Screen */
        C2D_TextParse(&s_textObjects[2], s_staticTextBuf, s_playback.currentVideo.title);
        C2D_DrawText(&s_textObjects[2], C2D_WithColor, 10, 42, 0, 0.46f, 0.46f, COLOR_TEXT_WHITE);

        /* Author and Views */
        char metaStr[96];
        snprintf(metaStr, sizeof(metaStr), "%s * %" PRId64 " views",
                 s_playback.currentVideo.author, s_playback.currentVideo.viewCount);
        C2D_TextParse(&s_textObjects[3], s_staticTextBuf, metaStr);
        C2D_DrawText(&s_textObjects[3], C2D_WithColor, 10, 62, 0, 0.40f, 0.40f, COLOR_TEXT_MUTED);

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
        C2D_TextParse(&s_textObjects[4], s_staticTextBuf, timeStr);
        C2D_DrawText(&s_textObjects[4], C2D_WithColor, 20, 112, 0, 0.40f, 0.40f, COLOR_TEXT_MUTED);

        /* --------------------------------------------------------------------
         * Touch Transport Controls
         * -------------------------------------------------------------------- */
        /* Rewind 10s: (20, 140, 60, 42) */
        C2D_DrawRectSolid(20, 140, 0, 60, 42, COLOR_PANEL);
        C2D_TextParse(&s_textObjects[5], s_staticTextBuf, "-10s");
        C2D_DrawText(&s_textObjects[5], C2D_WithColor, 35, 152, 0, 0.45f, 0.45f, COLOR_TEXT_WHITE);

        /* Big Play/Pause Button: (95, 135, 130, 52) */
        C2D_DrawRectSolid(95, 135, 0, 130, 52, s_playback.isPlaying ? COLOR_ACCENT : COLOR_BUTTON_BLUE);
        const char *btnLabel = s_playback.isPlaying ? "PAUSE [||]" : "PLAY [>]";
        C2D_TextParse(&s_textObjects[6], s_staticTextBuf, btnLabel);
        C2D_DrawText(&s_textObjects[6], C2D_WithColor, 122, 150, 0, 0.55f, 0.55f, COLOR_TEXT_WHITE);

        /* Fast Forward 10s: (240, 140, 60, 42) */
        C2D_DrawRectSolid(240, 140, 0, 60, 42, COLOR_PANEL);
        C2D_TextParse(&s_textObjects[7], s_staticTextBuf, "+10s");
        C2D_DrawText(&s_textObjects[7], C2D_WithColor, 255, 152, 0, 0.45f, 0.45f, COLOR_TEXT_WHITE);

        /* Description Toggle Button: (20, 196, 280, 36) */
        C2D_DrawRectSolid(20, 196, 0, 280, 36, COLOR_PANEL_ALT);
        C2D_TextParse(&s_textObjects[8], s_staticTextBuf, "=== View Video Description ===");
        C2D_DrawText(&s_textObjects[8], C2D_WithColor, 55, 206, 0, 0.45f, 0.45f, COLOR_TEXT_WHITE);

    } else if (s_appState == STATE_COMMENTS) {
        /* Description and Details Fullscreen View */
        C2D_DrawRectSolid(0, 0, 0, SCREEN_BOTTOM_WIDTH, 34, COLOR_PANEL);

        /* "< Back to Player" Button: (10, 5, 120, 24) */
        C2D_DrawRectSolid(8, 5, 0, 115, 24, COLOR_PANEL_ALT);
        C2D_TextParse(&s_textObjects[0], s_staticTextBuf, "< Back to Player");
        C2D_DrawText(&s_textObjects[0], C2D_WithColor, 14, 9, 0, 0.42f, 0.42f, COLOR_TEXT_WHITE);

        /* Description title */
        C2D_TextParse(&s_textObjects[1], s_staticTextBuf, "Description");
        C2D_DrawText(&s_textObjects[1], C2D_WithColor, 140, 9, 0, 0.48f, 0.48f, COLOR_ACCENT);

        /* Render multi-line description snippet */
        C2D_DrawRectSolid(10, 42, 0, 300, 150, COLOR_PANEL);
        const char *desc = s_playback.currentVideo.description;
        if (!desc || strlen(desc) == 0) {
            desc = "No description provided for this video.";
        }
        C2D_TextParse(&s_textObjects[2], s_staticTextBuf, desc);
        C2D_DrawText(&s_textObjects[2], C2D_WithColor, 18, 48 - s_descScrollOffset, 0, 0.40f, 0.40f, COLOR_TEXT_WHITE);

        /* Scroll Up / Down Touch Buttons */
        C2D_DrawRectSolid(10, 200, 0, 145, 32, COLOR_PANEL_ALT);
        C2D_TextParse(&s_textObjects[3], s_staticTextBuf, "[^] Scroll Up");
        C2D_DrawText(&s_textObjects[3], C2D_WithColor, 40, 208, 0, 0.42f, 0.42f, COLOR_TEXT_WHITE);

        C2D_DrawRectSolid(165, 200, 0, 145, 32, COLOR_PANEL_ALT);
        C2D_TextParse(&s_textObjects[4], s_staticTextBuf, "[v] Scroll Down");
        C2D_DrawText(&s_textObjects[4], C2D_WithColor, 195, 208, 0, 0.42f, 0.42f, COLOR_TEXT_WHITE);

    } else {
        /* STATE_SEARCH: Invidious Search Results List */
        C2D_DrawRectSolid(0, 0, 0, SCREEN_BOTTOM_WIDTH, 36, COLOR_PANEL);

        /* Quick Search Tag 1: "3DS" (8, 6, 70, 24) */
        C2D_DrawRectSolid(8, 6, 0, 70, 24, COLOR_ACCENT);
        C2D_TextParse(&s_textObjects[0], s_staticTextBuf, "\"3ds\"");
        C2D_DrawText(&s_textObjects[0], C2D_WithColor, 26, 10, 0, 0.42f, 0.42f, COLOR_TEXT_WHITE);

        /* Quick Search Tag 2: "Homebrew" (84, 6, 95, 24) */
        C2D_DrawRectSolid(84, 6, 0, 95, 24, COLOR_PANEL_ALT);
        C2D_TextParse(&s_textObjects[1], s_staticTextBuf, "\"homebrew\"");
        C2D_DrawText(&s_textObjects[1], C2D_WithColor, 94, 10, 0, 0.42f, 0.42f, COLOR_TEXT_WHITE);

        /* Quick Search Tag 3: "Chiptune" (185, 6, 85, 24) */
        C2D_DrawRectSolid(185, 6, 0, 85, 24, COLOR_PANEL_ALT);
        C2D_TextParse(&s_textObjects[2], s_staticTextBuf, "\"chiptune\"");
        C2D_DrawText(&s_textObjects[2], C2D_WithColor, 195, 10, 0, 0.42f, 0.42f, COLOR_TEXT_WHITE);

        /* Refresh Button: (275, 6, 38, 24) */
        C2D_DrawRectSolid(275, 6, 0, 38, 24, COLOR_BUTTON_BLUE);
        C2D_TextParse(&s_textObjects[3], s_staticTextBuf, "[R]");
        C2D_DrawText(&s_textObjects[3], C2D_WithColor, 285, 10, 0, 0.42f, 0.42f, COLOR_TEXT_WHITE);

        /* Render up to 4 search result item rows */
        int startY = 42;
        int rowHeight = 46;
        for (int i = 0; i < 4 && i < s_searchResults.count; i++) {
            VideoMetadata *vid = &s_searchResults.items[i];
            u32 rowColor = (i == s_selectedResultIndex) ? COLOR_PANEL_ALT : COLOR_PANEL;
            int y = startY + (i * (rowHeight + 3));

            C2D_DrawRectSolid(6, y, 0, 308, rowHeight, rowColor);

            if (i == s_selectedResultIndex) {
                C2D_DrawRectSolid(6, y, 0, 4, rowHeight, COLOR_ACCENT);
            }

            /* Video Title */
            C2D_TextParse(&s_textObjects[4 + (i * 2)], s_staticTextBuf, vid->title);
            C2D_DrawText(&s_textObjects[4 + (i * 2)], C2D_WithColor, 16, y + 4, 0, 0.42f, 0.42f, COLOR_TEXT_WHITE);

            /* Subtitle: Author and length */
            char rowSub[96];
            snprintf(rowSub, sizeof(rowSub), "%s * %d:%02d * Touch to Play",
                     vid->author, vid->lengthSeconds / 60, vid->lengthSeconds % 60);
            C2D_TextParse(&s_textObjects[5 + (i * 2)], s_staticTextBuf, rowSub);
            C2D_DrawText(&s_textObjects[5 + (i * 2)], C2D_WithColor, 16, y + 24, 0, 0.36f, 0.36f, COLOR_TEXT_MUTED);
        }
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
            invidious_search(DEFAULT_INVIDIOUS_HOST, "3ds", &s_searchResults);
            return;
        }
        if (is_touch_inside(touch, 84, 6, 95, 24)) {
            invidious_search(DEFAULT_INVIDIOUS_HOST, "homebrew", &s_searchResults);
            return;
        }
        if (is_touch_inside(touch, 185, 6, 85, 24)) {
            invidious_search(DEFAULT_INVIDIOUS_HOST, "chiptune", &s_searchResults);
            return;
        }
        if (is_touch_inside(touch, 275, 6, 38, 24)) {
            invidious_search(DEFAULT_INVIDIOUS_HOST, s_searchResults.query, &s_searchResults);
            return;
        }

        /* Select video item from touch list */
        int startY = 42;
        int rowHeight = 46;
        for (int i = 0; i < 4 && i < s_searchResults.count; i++) {
            int y = startY + (i * (rowHeight + 3));
            if (is_touch_inside(touch, 6, y, 308, rowHeight)) {
                s_selectedResultIndex = i;
                launch_video(&s_searchResults.items[i]);
                return;
            }
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

    /* Pre-allocate citro2d text buffer once to guarantee zero allocations in render loop */
    s_staticTextBuf = C2D_TextBufNew(4096);

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

    /* ------------------------------------------------------------------------
     * 3. Initial Keyless Invidious API Query
     * ------------------------------------------------------------------------ */
    memset(&s_searchResults, 0, sizeof(SearchResults));
    memset(&s_playback, 0, sizeof(PlaybackState));

    /* Query public Invidious instance without API keys */
    invidious_search(DEFAULT_INVIDIOUS_HOST, "3ds", &s_searchResults);

    /* ------------------------------------------------------------------------
     * 4. Main Event & Render Loop (Locked to 60 FPS VSync)
     * ------------------------------------------------------------------------ */
    while (aptMainLoop()) {
        /* Scan input hardware buttons and touchscreen state */
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();

        /* Exit application on START button press */
        if (kDown & KEY_START) {
            break;
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

        /* --------------------------------------------------------------------
         * Frame Rendering Begin
         * -------------------------------------------------------------------- */
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

        /* 1. Render Top Screen */
        C2D_SceneBegin(topTarget);
        render_top_screen();

        /* 2. Render Bottom Screen */
        C2D_SceneBegin(bottomTarget);
        render_bottom_screen();

        /* Swap framebuffers and present */
        C3D_FrameEnd();
    }

    /* ------------------------------------------------------------------------
     * 5. Clean Teardown & Resource Release
     * ------------------------------------------------------------------------ */
    invidious_exit();

    if (s_socBuffer) {
        socExit();
        free(s_socBuffer);
        s_socBuffer = NULL;
    }

    C2D_TextBufDelete(s_staticTextBuf);

cleanup_gfx:
    C2D_Fini();
    C3D_Fini();
    gfxExit();

    return 0;
}
