#include "store.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"

static const char *TAG = "store";

/* Two slots in the "storage" partition, each a header followed by the
 * layout JSON. A save goes to the slot with the older sequence number and
 * writes the header last, so a power cut during a save leaves the other
 * slot as the newest valid copy. */
#define SLOT_SIZE  (64 * 1024)
#define SLOTS      2
#define MAGIC      0x6c766174u /* "tavl" */

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t len;
    uint32_t crc;
} slot_hdr_t;

static const esp_partition_t *s_part;

esp_err_t store_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (!s_part || s_part->size < SLOTS * SLOT_SIZE) {
        ESP_LOGE(TAG, "no usable storage partition");
        s_part = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

/* Reads and checks one slot. Returns the body (malloc'd) or NULL. */
static char *read_slot(int slot, slot_hdr_t *hdr)
{
    size_t off = slot * SLOT_SIZE;
    if (esp_partition_read(s_part, off, hdr, sizeof(*hdr)) != ESP_OK ||
        hdr->magic != MAGIC || hdr->len == 0 || hdr->len > STORE_MAX_LAYOUT) {
        return NULL;
    }
    char *buf = malloc(hdr->len + 1);
    if (!buf) {
        return NULL;
    }
    if (esp_partition_read(s_part, off + sizeof(*hdr), buf, hdr->len) != ESP_OK ||
        esp_rom_crc32_le(0, (const uint8_t *)buf, hdr->len) != hdr->crc) {
        ESP_LOGW(TAG, "slot %d fails its check, ignored", slot);
        free(buf);
        return NULL;
    }
    buf[hdr->len] = '\0';
    return buf;
}

static int newest_slot(uint32_t *seq_out)
{
    int best = -1;
    uint32_t best_seq = 0;
    for (int i = 0; i < SLOTS; i++) {
        slot_hdr_t h;
        char *b = read_slot(i, &h);
        if (b) {
            free(b);
            if (best < 0 || h.seq > best_seq) {
                best = i;
                best_seq = h.seq;
            }
        }
    }
    if (seq_out) {
        *seq_out = best_seq;
    }
    return best;
}

char *store_load(size_t *len)
{
    if (!s_part) {
        return NULL;
    }
    int slot = newest_slot(NULL);
    if (slot < 0) {
        return NULL;
    }
    slot_hdr_t h;
    char *buf = read_slot(slot, &h);
    if (buf) {
        *len = h.len;
        ESP_LOGI(TAG, "saved layout: %u bytes from slot %d (seq %u)", (unsigned)h.len, slot,
                 (unsigned)h.seq);
    }
    return buf;
}

esp_err_t store_save(const char *data, size_t len)
{
    if (!s_part) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len == 0 || len > STORE_MAX_LAYOUT) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t seq;
    int newest = newest_slot(&seq);
    int slot = newest < 0 ? 0 : (newest + 1) % SLOTS;
    size_t off = slot * SLOT_SIZE;
    size_t erase = (sizeof(slot_hdr_t) + len + 4095) & ~(size_t)4095;

    slot_hdr_t h = {
        .magic = MAGIC,
        .seq = newest < 0 ? 1 : seq + 1,
        .len = len,
        .crc = esp_rom_crc32_le(0, (const uint8_t *)data, len),
    };

    esp_err_t err = esp_partition_erase_range(s_part, off, erase);
    if (err == ESP_OK) {
        err = esp_partition_write(s_part, off + sizeof(h), data, len);
    }
    if (err == ESP_OK) {
        err = esp_partition_write(s_part, off, &h, sizeof(h)); /* header last */
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "layout saved: %u bytes to slot %d (seq %u)", (unsigned)len, slot,
                 (unsigned)h.seq);
    } else {
        ESP_LOGE(TAG, "saving layout failed: %s", esp_err_to_name(err));
    }
    return err;
}
