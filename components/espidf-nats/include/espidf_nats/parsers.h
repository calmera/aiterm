/**
 * @file parsers.h
 * @brief Typed response parsers for JetStream and Object Store
 *
 * Helper functions to parse raw NATS message payloads into typed structs.
 * Uses cJSON (included with ESP-IDF) for robust JSON parsing.
 *
 * @note Numeric fields are parsed via cJSON's double representation.
 *       Values above 2^53 (e.g., very large sequence numbers or nanosecond
 *       timestamps) may lose integer precision. This is acceptable for
 *       ESP32 use cases where such values are rare.
 */

#ifndef ESPIDF_NATS_PARSERS_H
#define ESPIDF_NATS_PARSERS_H

#include <cJSON.h>
#include <string.h>
#include <stdlib.h>
#include <esp_log.h>
#include "types.h"

namespace NATSParsers {

    static const char* TAG = "nats_parsers";

    /**
     * Parse a JetStream publish ACK response
     *
     * Expected JSON: {"stream":"name","seq":123,"duplicate":false}
     * Error JSON: {"error":{"code":400,"description":"..."}}
     *
     * @param data Raw message data (JSON string)
     * @param size Size of data
     * @param stream Output: stream name (caller must free)
     * @param seq Output: sequence number
     * @param duplicate Output: whether this was a duplicate (deduplication)
     * @param error_desc Output: error description if failed (caller must free)
     * @return true if ACK parsed successfully, false if error response or parse failure
     */
    inline bool parse_pub_ack(const char* data, int size,
                              const char** stream, uint64_t* seq,
                              bool* duplicate, const char** error_desc) {
        if (data == NULL || size <= 0) return false;

        // Ensure null-terminated for cJSON
        char* json_str = (char*)malloc(size + 1);
        if (json_str == NULL) return false;
        memcpy(json_str, data, size);
        json_str[size] = '\0';

        cJSON* root = cJSON_Parse(json_str);
        free(json_str);
        if (root == NULL) {
            ESP_LOGW(TAG, "Failed to parse pub ACK JSON");
            return false;
        }

        // Check for error response
        cJSON* error_obj = cJSON_GetObjectItemCaseSensitive(root, "error");
        if (error_obj != NULL) {
            if (error_desc != NULL) {
                cJSON* desc = cJSON_GetObjectItemCaseSensitive(error_obj, "description");
                *error_desc = cJSON_IsString(desc) ? strdup(desc->valuestring) : strdup("Unknown error");
            }
            cJSON_Delete(root);
            return false;
        }

        // Parse ACK fields
        if (stream != NULL) {
            cJSON* s = cJSON_GetObjectItemCaseSensitive(root, "stream");
            *stream = cJSON_IsString(s) ? strdup(s->valuestring) : NULL;
        }
        if (seq != NULL) {
            cJSON* s = cJSON_GetObjectItemCaseSensitive(root, "seq");
            *seq = cJSON_IsNumber(s) ? (uint64_t)s->valuedouble : 0;
        }
        if (duplicate != NULL) {
            cJSON* d = cJSON_GetObjectItemCaseSensitive(root, "duplicate");
            *duplicate = cJSON_IsTrue(d);
        }

        cJSON_Delete(root);
        return true;
    }

    /**
     * Parse a JetStream account info response
     *
     * @param data Raw message data (JSON string)
     * @param size Size of data
     * @param info Output struct
     * @return true on success
     */
    inline bool parse_account_info(const char* data, int size,
                                   jetstream_account_info_t* info) {
        if (data == NULL || size <= 0 || info == NULL) return false;
        memset(info, 0, sizeof(jetstream_account_info_t));

        char* json_str = (char*)malloc(size + 1);
        if (json_str == NULL) return false;
        memcpy(json_str, data, size);
        json_str[size] = '\0';

        cJSON* root = cJSON_Parse(json_str);
        free(json_str);
        if (root == NULL) return false;

        // Check for error
        if (cJSON_GetObjectItemCaseSensitive(root, "error") != NULL) {
            cJSON_Delete(root);
            return false;
        }

        cJSON* item;
        item = cJSON_GetObjectItemCaseSensitive(root, "memory");
        if (cJSON_IsNumber(item)) info->memory = (int64_t)item->valuedouble;

        item = cJSON_GetObjectItemCaseSensitive(root, "storage");
        if (cJSON_IsNumber(item)) info->storage = (int64_t)item->valuedouble;

        item = cJSON_GetObjectItemCaseSensitive(root, "streams");
        if (cJSON_IsNumber(item)) info->streams = (int64_t)item->valuedouble;

        item = cJSON_GetObjectItemCaseSensitive(root, "consumers");
        if (cJSON_IsNumber(item)) info->consumers = (int64_t)item->valuedouble;

        // Parse limits
        cJSON* limits = cJSON_GetObjectItemCaseSensitive(root, "limits");
        if (limits != NULL) {
            item = cJSON_GetObjectItemCaseSensitive(limits, "max_memory");
            if (cJSON_IsNumber(item)) info->memory_max = (int64_t)item->valuedouble;

            item = cJSON_GetObjectItemCaseSensitive(limits, "max_storage");
            if (cJSON_IsNumber(item)) info->storage_max = (int64_t)item->valuedouble;

            item = cJSON_GetObjectItemCaseSensitive(limits, "max_streams");
            if (cJSON_IsNumber(item)) info->streams_max = (int64_t)item->valuedouble;

            item = cJSON_GetObjectItemCaseSensitive(limits, "max_consumers");
            if (cJSON_IsNumber(item)) info->consumers_max = (int64_t)item->valuedouble;
        }

        cJSON_Delete(root);
        return true;
    }

    /**
     * Parse object store metadata
     *
     * @note Caller must free() the string fields (name, description, digest)
     *
     * @param data Raw message data (JSON string)
     * @param size Size of data
     * @param meta Output struct
     * @return true on success
     */
    inline bool parse_object_meta(const char* data, int size,
                                  object_meta_t* meta) {
        if (data == NULL || size <= 0 || meta == NULL) return false;
        memset(meta, 0, sizeof(object_meta_t));

        char* json_str = (char*)malloc(size + 1);
        if (json_str == NULL) return false;
        memcpy(json_str, data, size);
        json_str[size] = '\0';

        cJSON* root = cJSON_Parse(json_str);
        free(json_str);
        if (root == NULL) return false;

        // Check for error response
        cJSON* error_obj = cJSON_GetObjectItemCaseSensitive(root, "error");
        if (error_obj != NULL) {
            cJSON_Delete(root);
            return false;
        }

        cJSON* item;
        item = cJSON_GetObjectItemCaseSensitive(root, "name");
        if (cJSON_IsString(item)) meta->name = strdup(item->valuestring);

        item = cJSON_GetObjectItemCaseSensitive(root, "description");
        if (cJSON_IsString(item)) meta->description = strdup(item->valuestring);

        item = cJSON_GetObjectItemCaseSensitive(root, "size");
        if (cJSON_IsNumber(item)) meta->size = (size_t)item->valuedouble;

        item = cJSON_GetObjectItemCaseSensitive(root, "chunks");
        if (cJSON_IsNumber(item)) meta->chunks = (uint64_t)item->valuedouble;

        item = cJSON_GetObjectItemCaseSensitive(root, "digest");
        if (cJSON_IsString(item)) meta->digest = strdup(item->valuestring);

        item = cJSON_GetObjectItemCaseSensitive(root, "mtime");
        if (cJSON_IsNumber(item)) meta->mtime = (uint64_t)item->valuedouble;

        cJSON_Delete(root);
        return true;
    }

    /**
     * Check if a JetStream API response is an error
     *
     * @param data Raw message data
     * @param size Size of data
     * @param error_code Output: error code (0 if not an error)
     * @param error_desc Output: error description string (caller must free)
     * @return true if the response IS an error
     */
    inline bool is_error_response(const char* data, int size,
                                  int* error_code, const char** error_desc) {
        if (error_code != NULL) *error_code = 0;
        if (error_desc != NULL) *error_desc = NULL;
        if (data == NULL || size <= 0) return false;

        char* json_str = (char*)malloc(size + 1);
        if (json_str == NULL) return false;
        memcpy(json_str, data, size);
        json_str[size] = '\0';

        cJSON* root = cJSON_Parse(json_str);
        free(json_str);
        if (root == NULL) return false;

        cJSON* error_obj = cJSON_GetObjectItemCaseSensitive(root, "error");
        if (error_obj == NULL) {
            cJSON_Delete(root);
            return false;
        }

        if (error_code != NULL) {
            cJSON* code = cJSON_GetObjectItemCaseSensitive(error_obj, "code");
            *error_code = cJSON_IsNumber(code) ? (int)code->valuedouble : 0;
        }
        if (error_desc != NULL) {
            cJSON* desc = cJSON_GetObjectItemCaseSensitive(error_obj, "description");
            *error_desc = cJSON_IsString(desc) ? strdup(desc->valuestring) : strdup("Unknown error");
        }

        cJSON_Delete(root);
        return true;
    }

} // namespace NATSParsers

#endif // ESPIDF_NATS_PARSERS_H
