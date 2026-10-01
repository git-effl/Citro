/**
 * ============================================================================
 * Citro - Lightweight Invidious YouTube Client for Nintendo 3DS
 * File: citro_invidious.c
 * ----------------------------------------------------------------------------
 * Implementation of keyless Invidious API requests using 3DS httpc / sockets
 * and memory-safe JSON parsing via cJSON.
 * ============================================================================
 */

#include "citro_invidious.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

/* Forward declaration of cJSON primitives */
#include "cJSON.h"

/* Static pre-allocated buffer for HTTP responses to avoid heap fragmentation */
static uint8_t *s_httpBuffer = NULL;
static bool s_httpcInitialized = false;

Result invidious_init(void) {
    /* Allocate static 256KB download buffer once at startup */
    if (!s_httpBuffer) {
        s_httpBuffer = (uint8_t *)memalign(0x1000, HTTP_RECV_BUF_SIZE);
        if (!s_httpBuffer) {
            return -1;
        }
    }

    /* Initialize 3DS HTTPC service (handles HTTPS/TLS on-device) */
    Result res = httpcInit(0);
    if (R_SUCCEEDED(res)) {
        s_httpcInitialized = true;
    }
    return res;
}

void invidious_exit(void) {
    if (s_httpcInitialized) {
        httpcExit();
        s_httpcInitialized = false;
    }
    if (s_httpBuffer) {
        free(s_httpBuffer);
        s_httpBuffer = NULL;
    }
}

/**
 * Perform a keyless HTTPS GET request to Invidious and store body in s_httpBuffer.
 */
static int http_get_request(const char *url, char *out_buf, size_t max_len) {
    httpcContext context;
    Result ret = 0;
    u32 statuscode = 0;
    u32 contentsize = 0;

    /* Open HTTP GET context with SSL enabled (3DS handles TLS verification) */
    ret = httpcOpenContext(&context, HTTPC_METHOD_GET, (char *)url, 1);
    if (R_FAILED(ret)) {
        return -1;
    }

    /* Set common User-Agent header (Invidious instances appreciate non-blank UAs) */
    httpcAddRequestHeaderField(&context, "User-Agent", "Citro-3DS-Client/1.0 (Nintendo 3DS Homebrew)");
    httpcAddRequestHeaderField(&context, "Accept", "application/json");

    /* Begin request pipeline */
    ret = httpcBeginRequest(&context);
    if (R_FAILED(ret)) {
        httpcCloseContext(&context);
        return -2;
    }

    /* Verify HTTP response status (200 OK expected) */
    ret = httpcGetResponseStatusCode(&context, &statuscode);
    if (R_FAILED(ret) || statuscode != 200) {
        httpcCloseContext(&context);
        return -3;
    }

    /* Query content length if provided by server */
    ret = httpcGetDownloadSizeState(&context, NULL, &contentsize);
    if (R_FAILED(ret)) {
        contentsize = 0;
    }

    /* Read response payload into pre-allocated memory */
    u32 bytesRead = 0;
    u32 totalBytes = 0;
    size_t targetCap = (max_len - 1 < HTTP_RECV_BUF_SIZE) ? (max_len - 1) : (HTTP_RECV_BUF_SIZE - 1);

    do {
        ret = httpcDownloadData(&context, s_httpBuffer + totalBytes, targetCap - totalBytes, &bytesRead);
        totalBytes += bytesRead;
    } while (ret == (s32)HTTPC_RESULTCODE_DOWNLOADPENDING && totalBytes < targetCap);

    httpcCloseContext(&context);

    if (totalBytes > 0) {
        s_httpBuffer[totalBytes] = '\0';
        memcpy(out_buf, s_httpBuffer, totalBytes + 1);
        return (int)totalBytes;
    }

    return -4;
}

int invidious_search(const char *host, const char *query, SearchResults *out_results) {
    if (!out_results) return -1;
    out_results->count = 0;
    out_results->isLoading = true;

    /* Build Invidious URL: https://<host>/api/v1/search?q=<query>&type=video */
    char url[512];
    char encodedQuery[128];
    size_t eq_idx = 0;

    /* Simple URL encode for spaces */
    for (size_t i = 0; query[i] != '\0' && eq_idx < sizeof(encodedQuery) - 4; i++) {
        if (query[i] == ' ') {
            encodedQuery[eq_idx++] = '+';
        } else {
            encodedQuery[eq_idx++] = query[i];
        }
    }
    encodedQuery[eq_idx] = '\0';
    strncpy(out_results->query, query, sizeof(out_results->query) - 1);

    snprintf(url, sizeof(url), "https://%s/api/v1/search?q=%s&type=video",
             host ? host : DEFAULT_INVIDIOUS_HOST,
             encodedQuery);

    /* Allocate temporary string for JSON processing */
    char *jsonPayload = (char *)malloc(HTTP_RECV_BUF_SIZE);
    if (!jsonPayload) {
        out_results->isLoading = false;
        return -1;
    }

    int bytes = http_get_request(url, jsonPayload, HTTP_RECV_BUF_SIZE);
    if (bytes <= 0) {
        free(jsonPayload);
        out_results->isLoading = false;
        return -2;
    }

    int parsed = invidious_parse_search_json(jsonPayload, out_results);
    free(jsonPayload);

    out_results->isLoading = false;
    return parsed;
}

int invidious_parse_search_json(const char *json_str, SearchResults *out_results) {
    if (!json_str || !out_results) return -1;

    cJSON *root = cJSON_Parse(json_str);
    if (!root || !cJSON_IsArray(root)) {
        if (root) cJSON_Delete(root);
        return -1;
    }

    int arraySize = cJSON_GetArrayItem(root, 0) ? cJSON_GetArraySize(root) : 0;
    int itemsToExtract = (arraySize < MAX_SEARCH_RESULTS) ? arraySize : MAX_SEARCH_RESULTS;
    int validCount = 0;

    for (int i = 0; i < itemsToExtract; i++) {
        cJSON *item = cJSON_GetArrayItem(root, i);
        if (!item) continue;

        cJSON *type = cJSON_GetObjectItem(item, "type");
        if (type && cJSON_IsString(type) && strcmp(type->valuestring, "video") != 0) {
            continue; /* Skip non-video entries like channels or playlists */
        }

        VideoMetadata *vid = &out_results->items[validCount];
        memset(vid, 0, sizeof(VideoMetadata));

        /* Extract videoId */
        cJSON *vidId = cJSON_GetObjectItem(item, "videoId");
        if (vidId && cJSON_IsString(vidId)) {
            strncpy(vid->videoId, vidId->valuestring, MAX_ID_LEN - 1);
        }

        /* Extract title */
        cJSON *title = cJSON_GetObjectItem(item, "title");
        if (title && cJSON_IsString(title)) {
            strncpy(vid->title, title->valuestring, MAX_TITLE_LEN - 1);
        }

        /* Extract author / channel */
        cJSON *author = cJSON_GetObjectItem(item, "author");
        if (author && cJSON_IsString(author)) {
            strncpy(vid->author, author->valuestring, MAX_AUTHOR_LEN - 1);
        }

        /* Extract duration in seconds */
        cJSON *length = cJSON_GetObjectItem(item, "lengthSeconds");
        if (length && cJSON_IsNumber(length)) {
            vid->lengthSeconds = length->valueint;
        }

        /* Extract view count */
        cJSON *views = cJSON_GetObjectItem(item, "viewCount");
        if (views && cJSON_IsNumber(views)) {
            vid->viewCount = (int64_t)views->valuedouble;
        }

        /* Extract description snippet */
        cJSON *desc = cJSON_GetObjectItem(item, "description");
        if (desc && cJSON_IsString(desc)) {
            strncpy(vid->description, desc->valuestring, MAX_DESC_LEN - 1);
        }

        vid->isValid = true;
        validCount++;
    }

    cJSON_Delete(root);
    out_results->count = validCount;
    return validCount;
}

int invidious_fetch_video_details(const char *host, const char *videoId, VideoMetadata *out_video) {
    if (!videoId || !out_video) return -1;

    char url[512];
    snprintf(url, sizeof(url), "https://%s/api/v1/videos/%s",
             host ? host : DEFAULT_INVIDIOUS_HOST,
             videoId);

    char *jsonPayload = (char *)malloc(HTTP_RECV_BUF_SIZE);
    if (!jsonPayload) return -1;

    int bytes = http_get_request(url, jsonPayload, HTTP_RECV_BUF_SIZE);
    if (bytes <= 0) {
        free(jsonPayload);
        return -2;
    }

    int res = invidious_parse_video_json(jsonPayload, out_video);
    free(jsonPayload);
    return res;
}

int invidious_parse_video_json(const char *json_str, VideoMetadata *out_video) {
    if (!json_str || !out_video) return -1;

    cJSON *root = cJSON_Parse(json_str);
    if (!root) return -1;

    cJSON *likeCount = cJSON_GetObjectItem(root, "likeCount");
    if (likeCount && cJSON_IsNumber(likeCount)) {
        out_video->likeCount = (int32_t)likeCount->valueint;
    }

    cJSON *desc = cJSON_GetObjectItem(root, "description");
    if (desc && cJSON_IsString(desc)) {
        strncpy(out_video->description, desc->valuestring, MAX_DESC_LEN - 1);
    }

    cJSON_Delete(root);
    return 0;
}

Result citro_battery_init(void) {
    /* Initialize PTM:U service for hardware battery monitoring */
    return ptmuInit();
}

void citro_battery_exit(void) {
    ptmuExit();
}

Result citro_battery_update(BatteryStatus *status) {
    if (!status) return -1;

    u8 level = 5;
    u8 charge = 0;
    u8 adapter = 0;

    Result res = PTMU_GetBatteryLevel(&level);
    if (R_SUCCEEDED(res)) {
        status->level = level;
        /* Map 0-5 3DS bars to estimated percentage: 5=100%, 4=80%, 3=60%, 2=40%, 1=20%, 0=5% */
        switch (level) {
            case 5: status->percent = 100; break;
            case 4: status->percent = 80;  break;
            case 3: status->percent = 60;  break;
            case 2: status->percent = 40;  break;
            case 1: status->percent = 20;  break;
            case 0:
            default: status->percent = 5;  break;
        }
    } else {
        /* Fallback if ptmu not available */
        status->level = 5;
        status->percent = 100;
    }

    if (R_SUCCEEDED(PTMU_GetBatteryChargeState(&charge))) {
        status->isCharging = (charge != 0);
    } else {
        status->isCharging = false;
    }

    if (R_SUCCEEDED(PTMU_GetAdapterState(&adapter))) {
        status->isAdapterPlugged = (adapter != 0);
    } else {
        status->isAdapterPlugged = false;
    }

    return 0;
}

