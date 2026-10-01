/*
  cJSON - Ultralightweight JSON parser in ANSI C
  Project: Citro 3DS Invidious Client
*/
#ifndef cJSON__h
#define cJSON__h

#ifdef __cplusplus
extern "C"
{
#endif

#define cJSON_Invalid (0)
#define cJSON_False  (1 << 0)
#define cJSON_True   (1 << 1)
#define cJSON_NULL   (1 << 2)
#define cJSON_Number (1 << 3)
#define cJSON_String (1 << 4)
#define cJSON_Array  (1 << 5)
#define cJSON_Object (1 << 6)
#define cJSON_Raw    (1 << 7)

typedef struct cJSON
{
    struct cJSON *next;
    struct cJSON *prev;
    struct cJSON *child;
    int type;
    char *valuestring;
    int valueint;
    double valuedouble;
    char *string;
} cJSON;

cJSON *cJSON_Parse(const char *value);
void   cJSON_Delete(cJSON *c);
int    cJSON_GetArraySize(const cJSON *array);
cJSON *cJSON_GetArrayItem(const cJSON *array, int index);
cJSON *cJSON_GetObjectItem(const cJSON * const object, const char * const string);
int    cJSON_IsArray(const cJSON * const item);
int    cJSON_IsObject(const cJSON * const item);
int    cJSON_IsString(const cJSON * const item);
int    cJSON_IsNumber(const cJSON * const item);

#ifdef __cplusplus
}
#endif

#endif
