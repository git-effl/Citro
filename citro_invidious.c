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
static int http_get_request(const char *url, char *out_buf, size_t max_len, int *out_status, Result *out_result) {
    httpcContext context;
    Result ret = 0;
    u32 statuscode = 0;
    u32 contentsize = 0;

    if (out_status) *out_status = 0;
    if (out_result) *out_result = 0;

    /* Open HTTP GET context with SSL enabled */
    ret = httpcOpenContext(&context, HTTPC_METHOD_GET, (char *)url, 1);
    if (R_FAILED(ret)) {
        if (out_result) *out_result = ret;
        return -1;
    }

    /* Disable SSL certificate verification so Let's Encrypt and modern CA certs work on 3DS */
    httpcSetSSLOpt(&context, SSLCOPT_DisableVerify);

    /* Enable keep-alive to stabilize connections */
    httpcSetKeepAlive(&context, HTTPC_KEEPALIVE_ENABLED);

    /* Use browser User-Agent and headers to prevent server-side bot drops */
    httpcAddRequestHeaderField(&context, "User-Agent", "Mozilla/5.0 (Nintendo 3DS; U; ; en) Version/1.7617.US");
    httpcAddRequestHeaderField(&context, "Accept", "application/json, text/plain, */*");
    httpcAddRequestHeaderField(&context, "Connection", "Keep-Alive");

    /* Begin request pipeline */
    ret = httpcBeginRequest(&context);
    if (R_FAILED(ret)) {
        if (out_result) *out_result = ret;
        httpcCloseContext(&context);
        return -2;
    }

    /* Verify HTTP response status */
    ret = httpcGetResponseStatusCode(&context, &statuscode);
    if (out_status) *out_status = (int)statuscode;
    if (out_result) *out_result = ret;

    /* Handle HTTP redirects (301, 302, 303, 307, 308) */
    if (statuscode >= 301 && statuscode <= 308) {
        char newUrl[512] = {0};
        Result hRes = httpcGetResponseHeader(&context, "Location", newUrl, sizeof(newUrl));
        httpcCloseContext(&context);
        if (R_SUCCEEDED(hRes) && newUrl[0] != '\0') {
            return http_get_request(newUrl, out_buf, max_len, out_status, out_result);
        }
        return -3;
    }

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
        bytesRead = 0;
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
    out_results->lastHttpStatus = 0;
    out_results->lastResultCode = 0;
    out_results->lastError[0] = '\0';

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

    const char *actualHost = host ? host : DEFAULT_INVIDIOUS_HOST;
    const char *scheme = "https://";
    if (strncmp(actualHost, "http://", 7) == 0 || strncmp(actualHost, "https://", 8) == 0) {
        scheme = "";
    }

    snprintf(url, sizeof(url), "%s%s/api/v1/search?q=%s&type=video",
             scheme, actualHost, encodedQuery);

    /* Allocate temporary string for JSON processing */
    char *jsonPayload = (char *)malloc(HTTP_RECV_BUF_SIZE);
    if (!jsonPayload) {
        out_results->isLoading = false;
        snprintf(out_results->lastError, sizeof(out_results->lastError), "Out of memory");
        return -1;
    }

    int bytes = http_get_request(url, jsonPayload, HTTP_RECV_BUF_SIZE,
                                 &out_results->lastHttpStatus,
                                 &out_results->lastResultCode);

    /* If HTTPS fails with 0xD8A0A03C (TLS/SSL certificate failure), attempt automatic fallback to plain HTTP */
    if (bytes <= 0 && (u32)out_results->lastResultCode == 0xD8A0A03C && strncmp(url, "https://", 8) == 0) {
        char fallbackUrl[512];
        snprintf(fallbackUrl, sizeof(fallbackUrl), "http://%s", url + 8);
        bytes = http_get_request(fallbackUrl, jsonPayload, HTTP_RECV_BUF_SIZE,
                                 &out_results->lastHttpStatus,
                                 &out_results->lastResultCode);
    }

    if (bytes <= 0) {
        free(jsonPayload);
        out_results->isLoading = false;
        if (out_results->lastHttpStatus > 0 && out_results->lastHttpStatus != 200) {
            snprintf(out_results->lastError, sizeof(out_results->lastError),
                     "HTTP %d error", out_results->lastHttpStatus);
        } else if ((u32)out_results->lastResultCode == 0xD8A0A03C) {
            snprintf(out_results->lastError, sizeof(out_results->lastError),
                     "TLS cert err: Check 3DS Date/Time or use HTTP");
        } else if (R_FAILED(out_results->lastResultCode)) {
            snprintf(out_results->lastError, sizeof(out_results->lastError),
                     "Net err: 0x%08lX", (unsigned long)out_results->lastResultCode);
        } else {
            snprintf(out_results->lastError, sizeof(out_results->lastError),
                     "Server unreachable");
        }
        return -2;
    }

    int parsed = invidious_parse_search_json(jsonPayload, out_results);
    free(jsonPayload);

    out_results->isLoading = false;
    if (parsed == 0) {
        snprintf(out_results->lastError, sizeof(out_results->lastError), "No video results found");
    }
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
    const char *actualHost = host ? host : DEFAULT_INVIDIOUS_HOST;
    const char *scheme = "https://";
    if (strncmp(actualHost, "http://", 7) == 0 || strncmp(actualHost, "https://", 8) == 0) {
        scheme = "";
    }

    snprintf(url, sizeof(url), "%s%s/api/v1/videos/%s",
             scheme, actualHost, videoId);

    char *jsonPayload = (char *)malloc(HTTP_RECV_BUF_SIZE);
    if (!jsonPayload) return -1;

    int bytes = http_get_request(url, jsonPayload, HTTP_RECV_BUF_SIZE, NULL, NULL);
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
    bool adapter = false;

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
        status->isAdapterPlugged = adapter;
    } else {
        status->isAdapterPlugged = false;
    }

    return 0;
}

