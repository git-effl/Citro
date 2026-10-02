/**
 * ============================================================================
 * Citro - Lightweight Invidious YouTube Client for Nintendo 3DS
 * File: citro_invidious.h
 * ----------------------------------------------------------------------------
 * Header defining data structures, state machines, and networking/JSON
 * prototypes for querying open public Invidious instances without API keys.
 *
 * Compatible with devkitARM, libctru, and citro2d.
 * ============================================================================
 */

#ifndef CITRO_INVIDIOUS_H
#define CITRO_INVIDIOUS_H

#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum string buffer lengths tailored for 3DS limited memory constraints */
#define MAX_TITLE_LEN        128
#define MAX_ID_LEN           32
#define MAX_AUTHOR_LEN       64
#define MAX_DESC_LEN         1024
#define MAX_SEARCH_RESULTS   10
#define HTTP_RECV_BUF_SIZE   (256 * 1024) /* 256KB pre-allocated response buffer */

/* Default public Invidious instance host (no API key required) */
#define DEFAULT_INVIDIOUS_HOST "invidious.f5.si"
#define DEFAULT_INVIDIOUS_PORT 443

/**
 * AppState: Current UI screen and navigation context.
 */
typedef enum {
    STATE_SEARCH = 0,   /* Search results list & query input */
    STATE_PLAYBACK,     /* Active video playback & touch transport controls */
    STATE_COMMENTS      /* Video description and metadata viewer */
} AppState;

/**
 * VideoMetadata: Invidious video metadata extracted from API responses.
 * Designed with fixed buffer sizes to avoid dynamic heap fragmentation.
 */
typedef struct {
    char        videoId[MAX_ID_LEN];       /* 11-char YouTube ID (e.g. "dQw4w9WgXcQ") */
    char        title[MAX_TITLE_LEN];      /* Sanitized video title */
    char        author[MAX_AUTHOR_LEN];    /* Channel / uploader name */
    int         lengthSeconds;             /* Total duration in seconds */
    int64_t     viewCount;                 /* Lifetime view count */
    int32_t     likeCount;                 /* Public like count */
    char        description[MAX_DESC_LEN]; /* Video description text */
    bool        isValid;                   /* Flag indicating struct contains loaded data */
} VideoMetadata;

/**
 * SearchResults: Collection of videos returned from an Invidious search query.
 */
typedef struct {
    VideoMetadata items[MAX_SEARCH_RESULTS];
    int           count;
    char          query[64];
    bool          isLoading;
    int           lastHttpStatus;
    Result        lastResultCode;
    char          lastError[64];
} SearchResults;

/**
 * PlaybackState: Tracks active playback timeline and OSD banner timers.
 */
typedef struct {
    VideoMetadata currentVideo;
    bool          isPlaying;
    float         currentPositionSec;      /* Elapsed playback position */
    u64           launchTick;              /* svcGetSystemTick() when video launched */
    bool          showTitleBanner;         /* Active for 5 seconds post-launch */
    float         bannerOpacity;           /* Alpha fade (1.0f -> 0.0f) */
} PlaybackState;

/* ============================================================================
 * Function Prototypes
 * ============================================================================ */

/**
 * BatteryStatus: 3DS Hardware battery status queried from PTM:U service.
 */
typedef struct {
    u8   level;             /* 0 to 5 bars (5 = 100% full, 0 = critical low) */
    u8   percent;           /* Estimated percentage (0 - 100%) */
    bool isCharging;        /* True if AC adapter currently charging */
    bool isAdapterPlugged;  /* True if wall charger connected */
} BatteryStatus;

/**
 * Initialize 3DS PTM (Power Management) service for battery querying.
 */
Result citro_battery_init(void);

/**
 * Exit PTM service.
 */
void citro_battery_exit(void);

/**
 * Query current hardware battery level and charging state.
 */
Result citro_battery_update(BatteryStatus *status);

/**
 * Initialize internal network buffers and HTTP client context.
 * Must be called after socInit() or httpcInit().
 *
 * @return 0 on success, non-zero error code otherwise.
 */
Result invidious_init(void);

/**
 * Free internal network buffers and close open HTTP contexts.
 */
void invidious_exit(void);

/**
 * Fetch search results from an Invidious instance using keyless HTTP GET.
 * Example endpoint: GET /api/v1/search?q=3ds&type=video
 *
 * @param host        Instance hostname (e.g. "invidious.flokinet.to")
 * @param query       Search keywords (e.g. "3ds homebrew")
 * @param out_results Destination struct for parsed results (preallocated)
 * @return            0 on success, negative value on network/parse failure.
 */
int invidious_search(const char *host, const char *query, SearchResults *out_results);

/**
 * Parse an Invidious JSON search response using cJSON without leaking memory.
 *
 * @param json_str    Raw JSON string payload
 * @param out_results Destination struct for parsed video items
 * @return            Number of videos parsed, or negative on error.
 */
int invidious_parse_search_json(const char *json_str, SearchResults *out_results);

/**
 * Fetch detailed video info including full description and like count.
 * Example endpoint: GET /api/v1/videos/<videoId>
 *
 * @param host        Instance hostname
 * @param videoId     YouTube 11-char ID
 * @param out_video   Destination struct for parsed details
 * @return            0 on success, negative on error.
 */
int invidious_fetch_video_details(const char *host, const char *videoId, VideoMetadata *out_video);

/**
 * Parse detailed video JSON response into VideoMetadata.
 */
int invidious_parse_video_json(const char *json_str, VideoMetadata *out_video);

#ifdef __cplusplus
}
#endif

#endif /* CITRO_INVIDIOUS_H */
