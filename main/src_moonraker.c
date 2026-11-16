#include "src_moonraker.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "moonraker";

#define RX_MAX          (4 * 1024)
#define RETRY_INFO_US   (5 * 1000 * 1000)
#define BACKOFF_MIN_MS  2000
#define BACKOFF_MAX_MS  30000

enum { ID_IDENTIFY = 1, ID_INFO = 2, ID_SUBSCRIBE = 3, ID_METADATA = 4, ID_PROC = 5, ID_SYSTEM = 6,
       ID_HISTORY = 7, ID_QUEUE = 8, ID_RPC = 100 /* + method index */ };

/* Values computed here rather than read from Klipper, worked out the way
 * Mainsail does so the two agree. */
#define DERIVED_PREFIX "tavle."
enum {
    D_SLICER, D_KLIPPER, D_FILE, D_FILAMENT, D_PROGRESS, D_FILENAME, D_LAYER, D_THUMB,
    D_USED_G, D_TOTAL_G,
    D_COUNT
};
static const char *const DERIVED[D_COUNT] = {
    [D_SLICER] = "tavle.slicer_remaining",
    [D_KLIPPER] = "tavle.klipper_remaining",
    [D_FILE] = "tavle.file_remaining",
    [D_FILAMENT] = "tavle.filament_remaining",
    [D_PROGRESS] = "tavle.progress",
    [D_FILENAME] = "tavle.filename",
    [D_LAYER] = "tavle.layer",
    [D_THUMB] = "tavle.thumbnail",
    [D_USED_G] = "tavle.filament_used_g",
    [D_TOTAL_G] = "tavle.filament_total_g",
};
/* Moonraker's own data, beyond Klipper objects. A group is only asked for
 * when a widget reads one of its paths. */
enum { G_META, G_PROC, G_SYSTEM, G_HISTORY, G_QUEUE, G_RPC, G_COUNT };
static const char *const GROUP_PREFIX[G_COUNT] = {
    [G_META] = "moonraker.meta.",     /* the printing file's metadata */
    [G_PROC] = "moonraker.proc.",     /* host stats, pushed every second */
    [G_SYSTEM] = "moonraker.system.", /* host description, once per connect */
    [G_HISTORY] = "moonraker.history.", /* job totals, again after each job */
    [G_QUEUE] = "moonraker.queue.",   /* job queue, again when it changes */
    [G_RPC] = "rpc:",                 /* rpc:<method>:<field>, polled */
};

/* Methods an rpc: path may call: read-only ones without parameters. The
 * layout comes from a URL, so nothing that could move, heat or restart the
 * printer may be reachable from it. */
static const char *const RPC_ALLOWED[] = {
    "printer.info", "server.info", "machine.system_info", "machine.proc_stats",
    "server.history.totals", "server.job_queue.status", "machine.update.status",
    "machine.device_power.devices", "server.sensors.list", "server.announcements.list",
    "server.webcams.list",
};
#define RPC_MAX       6
#define RPC_POLL_DEF  60
#define RPC_POLL_MIN  5

#define THUMB_MIN_W 100 /* smallest thumbnail worth showing in a cell */
#define THUMB_MAX_W 1024 /* widest the PNG decoder takes */

typedef struct {
    bool sent;
    bool is_num;
    double num;
    uint32_t hash; /* of the text */
} last_t;

/* From the file's metadata; zero when unknown. */
typedef struct {
    double estimated_time;
    double filament_total;
    double filament_weight; /* grams for the whole print */
    double start_byte, end_byte;
    double layer_count, first_layer_height, layer_height, object_height;
    char thumb[160]; /* path of the chosen thumbnail, below the gcodes root */
} file_meta_t;

typedef struct {
    int index;
    SemaphoreHandle_t lock; /* held while reporting, so none leaves after stop */
    bool dead;              /* stopped: report nothing */
    TaskHandle_t waiter;    /* task in mr_stop, told when the client is gone */
    volatile bool subscribed; /* status updates flowing; else the retry timer asks again */
    char uri[96];
    char base[80]; /* http://host:port */
    char **paths;
    int npaths;
    esp_websocket_client_handle_t ws;
    esp_timer_handle_t retry;
    char *rx;
    int rx_len;
    bool rx_skip;
    int backoff_ms;
    /* inputs for the derived values */
    bool want[D_COUNT];
    bool want_any;
    double print_duration;
    double filament_used;
    double progress;
    double file_position;
    double current_layer, total_layer; /* -1 = Klipper does not know */
    double z;
    char state[16];
    char filename[128];
    file_meta_t meta;
    last_t last[D_COUNT];
    bool gwant[G_COUNT];
    char rpc[RPC_MAX][40];   /* allowed methods the layout's rpc: paths call */
    int nrpc;
    char rpc_refused[40];    /* first method refused, for the status line */
    esp_timer_handle_t poll; /* calls the rpc methods every poll_s */
    int poll_s;
    bool dropped_big;        /* a reply over RX_MAX was dropped */
} mr_t;

/* Reports go out only while the source runs: stop() may return before the
 * websocket task has ended. */
static void report_number(mr_t *m, const char *path, double v)
{
    xSemaphoreTake(m->lock, portMAX_DELAY);
    if (!m->dead) {
        sources_report_number(m->index, path, v);
    }
    xSemaphoreGive(m->lock);
}

static void report_text(mr_t *m, const char *path, const char *text)
{
    xSemaphoreTake(m->lock, portMAX_DELAY);
    if (!m->dead) {
        sources_report_text(m->index, path, text);
    }
    xSemaphoreGive(m->lock);
}

static void report_state(mr_t *m, src_state_t state, const char *detail)
{
    xSemaphoreTake(m->lock, portMAX_DELAY);
    if (!m->dead) {
        sources_report_state(m->index, state, detail);
    }
    xSemaphoreGive(m->lock);
}

static bool is_derived(const char *path)
{
    return strncmp(path, DERIVED_PREFIX, strlen(DERIVED_PREFIX)) == 0;
}

/* The group a path belongs to, or -1 for a Klipper object path. */
static int group_of(const char *path)
{
    for (int g = 0; g < G_COUNT; g++) {
        if (strncmp(path, GROUP_PREFIX[g], strlen(GROUP_PREFIX[g])) == 0) {
            return g;
        }
    }
    return -1;
}

/* Not a Klipper object: computed here or read from a Moonraker call. */
static bool not_klipper(const char *path)
{
    return is_derived(path) || group_of(path) >= 0;
}

/* Follows a dotted path into JSON: "a.b.0.c" takes list item 0 of b, and a
 * last segment "count" on a list gives its length. *count_out is set then. */
static const cJSON *json_at(const cJSON *v, const char *path, double *count_out, bool *is_count)
{
    char seg[SRC_PATH_MAX];
    *is_count = false;
    while (v && path && *path) {
        size_t n = strcspn(path, ".");
        snprintf(seg, sizeof(seg), "%.*s", (int)n, path);
        const char *next = path[n] ? path + n + 1 : NULL;
        if (cJSON_IsArray(v)) {
            if (!next && strcmp(seg, "count") == 0) {
                *count_out = cJSON_GetArraySize(v);
                *is_count = true;
                return v;
            }
            char *end;
            long i = strtol(seg, &end, 10);
            v = (*end == '\0' && end != seg) ? cJSON_GetArrayItem(v, (int)i) : NULL;
        } else {
            v = cJSON_GetObjectItemCaseSensitive(v, seg);
        }
        path = next;
    }
    return v;
}

/* Reports path, read from root at path + skip, as a number or text. */
static void report_json(mr_t *m, const char *path, size_t skip, const cJSON *root)
{
    double count;
    bool is_count;
    const cJSON *v = json_at(root, path + skip, &count, &is_count);
    if (is_count) {
        report_number(m, path, count);
    } else if (cJSON_IsNumber(v)) {
        report_number(m, path, v->valuedouble);
    } else if (cJSON_IsBool(v)) {
        report_number(m, path, cJSON_IsTrue(v) ? 1 : 0);
    } else if (cJSON_IsString(v)) {
        report_text(m, path, v->valuestring);
    } else {
        report_text(m, path, "--");
    }
}

/* Reports every wanted path of group g from root; root NULL shows "--". */
static void report_group(mr_t *m, int g, const cJSON *root)
{
    if (!m->gwant[g]) {
        return;
    }
    size_t skip = strlen(GROUP_PREFIX[g]);
    for (int i = 0; i < m->npaths; i++) {
        if (group_of(m->paths[i]) == g) {
            report_json(m, m->paths[i], skip, root);
        }
    }
}

static void send_json(mr_t *m, const char *method, int id, cJSON *params)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddStringToObject(req, "method", method);
    if (params) {
        cJSON_AddItemToObject(req, "params", params);
    }
    cJSON_AddNumberToObject(req, "id", id);
    char *s = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (s) {
        esp_websocket_client_send_text(m->ws, s, strlen(s), pdMS_TO_TICKS(2000));
        free(s);
    }
}

/* "heater_fan fan1.speed" -> object "heater_fan fan1", field path "speed" */
static bool split_path(const char *path, char *obj, size_t obj_len, const char **rest)
{
    const char *dot = strchr(path, '.');
    size_t n = dot ? (size_t)(dot - path) : strlen(path);
    if (n == 0 || n >= obj_len) {
        return false;
    }
    memcpy(obj, path, n);
    obj[n] = '\0';
    *rest = dot ? dot + 1 : NULL;
    return true;
}

static void subscribe(mr_t *m)
{
    cJSON *objects = cJSON_CreateObject();
    char obj[SRC_PATH_MAX];
    static const char *const inputs[] = {
        "print_stats.print_duration", "print_stats.filename", "print_stats.state",
        "print_stats.info", "print_stats.filament_used", "virtual_sdcard.progress", "virtual_sdcard.file_position",
        "gcode_move.gcode_position", /* last: only for the layer */
    };
    int ninputs = sizeof(inputs) / sizeof(inputs[0]);
    if (!m->want[D_LAYER]) {
        ninputs--; /* the position changes many times a second */
    }
    int total = m->npaths + (m->want_any ? ninputs : 0);
    for (int i = 0; i < total; i++) {
        const char *path = i < m->npaths ? m->paths[i] : inputs[i - m->npaths];
        const char *rest;
        if (not_klipper(path) || !split_path(path, obj, sizeof(obj), &rest)) {
            continue;
        }
        cJSON *fields = cJSON_GetObjectItemCaseSensitive(objects, obj);
        if (!rest) {
            /* whole object: null asks for every field */
            if (fields) {
                cJSON_DeleteItemFromObjectCaseSensitive(objects, obj);
            }
            cJSON_AddNullToObject(objects, obj);
            continue;
        }
        if (cJSON_IsNull(fields)) {
            continue;
        }
        if (!fields) {
            fields = cJSON_AddArrayToObject(objects, obj);
        }
        char field[SRC_PATH_MAX];
        size_t n = strcspn(rest, ".");
        snprintf(field, sizeof(field), "%.*s", (int)n, rest);
        bool dup = false;
        const cJSON *f;
        cJSON_ArrayForEach(f, fields) {
            dup |= strcmp(f->valuestring, field) == 0;
        }
        if (!dup) {
            cJSON_AddItemToArray(fields, cJSON_CreateString(field));
        }
    }
    cJSON *params = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "objects", objects);
    send_json(m, "printer.objects.subscribe", ID_SUBSCRIBE, params);
}

static void request_metadata(mr_t *m)
{
    memset(&m->meta, 0, sizeof(m->meta));
    if (!m->filename[0]) {
        report_group(m, G_META, NULL);
        return;
    }
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "filename", m->filename);
    send_json(m, "server.files.metadata", ID_METADATA, p);
}

/* "rpc:server.info:klippy_state" -> method "server.info", field after it. */
static bool rpc_split(const char *path, char *method, size_t len, size_t *skip)
{
    const char *start = path + strlen(GROUP_PREFIX[G_RPC]);
    const char *colon = strchr(start, ':');
    if (!colon || colon == start || (size_t)(colon - start) >= len) {
        return false;
    }
    snprintf(method, len, "%.*s", (int)(colon - start), start);
    *skip = colon + 1 - path;
    return true;
}

static bool rpc_allowed(const char *method)
{
    for (size_t i = 0; i < sizeof(RPC_ALLOWED) / sizeof(RPC_ALLOWED[0]); i++) {
        if (strcmp(method, RPC_ALLOWED[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void send_rpcs(mr_t *m)
{
    for (int k = 0; k < m->nrpc; k++) {
        send_json(m, m->rpc[k], ID_RPC + k, NULL);
    }
}

/* A reply to rpc method k: every path calling it reads its field. */
static void on_rpc_reply(mr_t *m, int k, const cJSON *result)
{
    char method[40];
    size_t skip;
    for (int i = 0; i < m->npaths; i++) {
        if (group_of(m->paths[i]) == G_RPC && rpc_split(m->paths[i], method, sizeof(method), &skip) &&
            strcmp(method, m->rpc[k]) == 0) {
            report_json(m, m->paths[i], skip, result);
        }
    }
}

static void on_poll(void *arg)
{
    mr_t *m = arg;
    if (m->subscribed && esp_websocket_client_is_connected(m->ws)) {
        send_rpcs(m);
    }
}

/* First values of the wanted groups; later ones arrive as notifications. */
static void request_groups(mr_t *m)
{
    if (m->gwant[G_PROC]) {
        send_json(m, "machine.proc_stats", ID_PROC, NULL);
    }
    if (m->gwant[G_SYSTEM]) {
        send_json(m, "machine.system_info", ID_SYSTEM, NULL);
    }
    if (m->gwant[G_HISTORY]) {
        send_json(m, "server.history.totals", ID_HISTORY, NULL);
    }
    if (m->gwant[G_QUEUE]) {
        send_json(m, "server.job_queue.status", ID_QUEUE, NULL);
    }
    send_rpcs(m);
    /* paths calling a refused method say so instead of staying "--" */
    char method[40];
    size_t skip;
    for (int i = 0; m->rpc_refused[0] && i < m->npaths; i++) {
        if (group_of(m->paths[i]) == G_RPC && rpc_split(m->paths[i], method, sizeof(method), &skip) &&
            !rpc_allowed(method)) {
            report_text(m, m->paths[i], "not allowed");
        }
    }
}

static double num_or(const cJSON *obj, const char *key, double def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

/* The smallest thumbnail at least THUMB_MIN_W wide, else the largest; none
 * wider than the decoder takes. */
static void pick_thumbnail(mr_t *m, const cJSON *thumbs)
{
    const cJSON *best = NULL, *t;
    double best_w = 0;
    cJSON_ArrayForEach(t, thumbs) {
        double w = num_or(t, "width", 0);
        double h = num_or(t, "height", 0);
        if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(t, "relative_path")) || w <= 0 ||
            w > THUMB_MAX_W || h > THUMB_MAX_W) {
            continue;
        }
        bool better = !best ||
                      (w >= THUMB_MIN_W ? best_w < THUMB_MIN_W || w < best_w : w > best_w);
        if (better) {
            best = t;
            best_w = w;
        }
    }
    m->meta.thumb[0] = '\0';
    if (best) {
        /* relative to the folder the gcode file is in */
        const char *rel = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(best, "relative_path"));
        const char *slash = strrchr(m->filename, '/');
        int dir = slash ? (int)(slash - m->filename) + 1 : 0;
        snprintf(m->meta.thumb, sizeof(m->meta.thumb), "%.*s%s", dir, m->filename, rel);
    }
}

static void on_metadata(mr_t *m, const cJSON *r)
{
    file_meta_t *f = &m->meta;
    f->estimated_time = num_or(r, "estimated_time", 0);
    f->filament_total = num_or(r, "filament_total", 0);
    f->filament_weight = num_or(r, "filament_weight_total", 0);
    f->start_byte = num_or(r, "gcode_start_byte", 0);
    f->end_byte = num_or(r, "gcode_end_byte", 0);
    f->layer_count = num_or(r, "layer_count", 0);
    f->first_layer_height = num_or(r, "first_layer_height", 0);
    f->layer_height = num_or(r, "layer_height", 0);
    f->object_height = num_or(r, "object_height", 0);
    pick_thumbnail(m, cJSON_GetObjectItemCaseSensitive(r, "thumbnails"));
    ESP_LOGI(TAG, "metadata for %s: slicer estimate %.0f s, %.0f layers, thumbnail %s",
             m->filename, f->estimated_time, f->layer_count, f->thumb[0] ? f->thumb : "none");
}

static uint32_t hash_str(const char *s)
{
    uint32_t h = 5381;
    while (*s) {
        h = h * 33 + (uint8_t)*s++;
    }
    return h;
}

/* Sends a derived value if a widget shows it and it changed. */
static void emit(mr_t *m, int d, bool is_num, double num, const char *text)
{
    if (!m->want[d]) {
        return;
    }
    last_t *l = &m->last[d];
    uint32_t h = is_num ? 0 : hash_str(text);
    if (l->sent && l->is_num == is_num && (is_num ? l->num == num : l->hash == h)) {
        return;
    }
    *l = (last_t){.sent = true, .is_num = is_num, .num = num, .hash = h};
    if (is_num) {
        report_number(m, DERIVED[d], num);
    } else {
        report_text(m, DERIVED[d], text);
    }
}

static void emit_num_or_none(mr_t *m, int d, bool valid, double v)
{
    if (valid) {
        emit(m, d, true, v, NULL);
    } else {
        emit(m, d, false, 0, "--");
    }
}

/* Takes what changed from a status object into the inputs. */
static void take_inputs(mr_t *m, const cJSON *status)
{
    const cJSON *ps = cJSON_GetObjectItemCaseSensitive(status, "print_stats");
    const cJSON *vs = cJSON_GetObjectItemCaseSensitive(status, "virtual_sdcard");
    const cJSON *gm = cJSON_GetObjectItemCaseSensitive(status, "gcode_move");
    const cJSON *v;
    m->print_duration = num_or(ps, "print_duration", m->print_duration);
    m->filament_used = num_or(ps, "filament_used", m->filament_used);
    m->progress = num_or(vs, "progress", m->progress);
    m->file_position = num_or(vs, "file_position", m->file_position);
    if ((v = cJSON_GetObjectItemCaseSensitive(ps, "state")) && cJSON_IsString(v)) {
        strlcpy(m->state, v->valuestring, sizeof(m->state));
    }
    const cJSON *info = cJSON_GetObjectItemCaseSensitive(ps, "info");
    if ((v = cJSON_GetObjectItemCaseSensitive(info, "current_layer"))) {
        m->current_layer = cJSON_IsNumber(v) ? v->valuedouble : -1;
    }
    if ((v = cJSON_GetObjectItemCaseSensitive(info, "total_layer"))) {
        m->total_layer = cJSON_IsNumber(v) ? v->valuedouble : -1;
    }
    const cJSON *pos = cJSON_GetObjectItemCaseSensitive(gm, "gcode_position");
    if (cJSON_IsArray(pos) && cJSON_IsNumber(cJSON_GetArrayItem(pos, 2))) {
        m->z = cJSON_GetArrayItem(pos, 2)->valuedouble;
    }
    if ((v = cJSON_GetObjectItemCaseSensitive(ps, "filename")) && cJSON_IsString(v) &&
        strcmp(v->valuestring, m->filename) != 0) {
        strlcpy(m->filename, v->valuestring, sizeof(m->filename));
        request_metadata(m);
    }
}

/* Progress through the gcode between the slicer's start and end markers,
 * as Mainsail shows it; virtual_sdcard.progress also counts the header. */
static double file_progress(const mr_t *m)
{
    const file_meta_t *f = &m->meta;
    if (m->filename[0] && f->start_byte > 0 && f->end_byte > f->start_byte) {
        if (m->file_position <= f->start_byte) {
            return 0;
        }
        if (m->file_position >= f->end_byte) {
            return 1;
        }
        return (m->file_position - f->start_byte) / (f->end_byte - f->start_byte);
    }
    return m->progress;
}

static void layer_text(const mr_t *m, char *buf, size_t len)
{
    const file_meta_t *f = &m->meta;
    double total = m->total_layer;
    if (total < 0) {
        total = f->layer_count;
    }
    if (total <= 0 && f->object_height > 0 && f->layer_height > 0) {
        total = ceil((f->object_height - f->first_layer_height) / f->layer_height + 1);
    }
    double cur = m->current_layer;
    if (cur < 0) {
        cur = 0;
        if (total > 0 && f->layer_height > 0) {
            /* Klipper is not told the layer: work it out from the height */
            cur = ceil((m->z - f->first_layer_height) / f->layer_height + 1);
            cur = cur > total ? total : cur < 0 ? 0 : cur;
        }
    }
    if (total > 0) {
        snprintf(buf, len, "%.0f/%.0f", cur, total);
    } else {
        snprintf(buf, len, "%.0f", cur);
    }
}

static void url_append(char *out, size_t len, const char *s)
{
    size_t n = strlen(out);
    for (; *s && n + 4 < len; s++) {
        unsigned char c = *s;
        if (isalnum(c) || strchr("-._~/", c)) {
            out[n++] = c;
        } else {
            n += snprintf(out + n, len - n, "%%%02X", c);
        }
    }
    out[n] = '\0';
}

/* "1h18m", "15m22s", "2d3h": the print time a slicer puts in file names. */
static bool is_time_word(const char *s)
{
    if (!*s) {
        return false;
    }
    while (*s) {
        if (!isdigit((unsigned char)*s)) {
            return false;
        }
        while (isdigit((unsigned char)*s)) {
            s++;
        }
        if (!*s || !strchr("dhms", *s)) {
            return false;
        }
        s++;
    }
    return true;
}

/* "tophex_PETG_1h18m" -> "tophex": drops the material and time the slicer's
 * default file name template adds. Other names are left as they are. */
static void strip_slicer_suffix(char *name)
{
    char *time = strrchr(name, '_');
    if (!time || !is_time_word(time + 1)) {
        return;
    }
    *time = '\0';
    char *material = strrchr(name, '_');
    if (material && material != name) {
        *material = '\0';
    } else {
        *time = '_'; /* no name left before the material: keep it all */
    }
}

static void update_derived(mr_t *m)
{
    bool printing = strcmp(m->state, "printing") == 0 || strcmp(m->state, "paused") == 0;
    double fp = file_progress(m);

    emit_num_or_none(m, D_SLICER, printing && m->meta.estimated_time > 0,
                     m->meta.estimated_time - m->print_duration);
    /* Mainsail's default estimate is the mean of these two: time printed
     * scaled by file progress, and by filament used out of the total */
    double t = m->print_duration;
    bool file_ok = printing && fp > 0 && t > 0;
    double file_est = file_ok ? t / fp - t : 0;
    bool fil_ok = printing && t > 0 && m->meta.filament_total > 0 && m->filament_used > 0;
    double fil_est = fil_ok ? t / (m->filament_used / m->meta.filament_total) - t : 0;
    emit_num_or_none(m, D_FILE, file_ok, file_est);
    emit_num_or_none(m, D_FILAMENT, fil_ok, fil_est);
    int nest = file_ok + fil_ok;
    emit_num_or_none(m, D_KLIPPER, nest > 0, nest ? (file_est + fil_est) / nest : 0);
    emit(m, D_PROGRESS, true, fp, NULL);

    /* grams: the slicer's weight for the print, scaled by length used */
    bool g_ok = m->filename[0] && m->meta.filament_weight > 0;
    emit_num_or_none(m, D_TOTAL_G, g_ok, m->meta.filament_weight);
    emit_num_or_none(m, D_USED_G, g_ok && m->meta.filament_total > 0,
                     m->meta.filament_weight * m->filament_used / m->meta.filament_total);

    char buf[SRC_TEXT_MAX];
    const char *base = strrchr(m->filename, '/');
    strlcpy(buf, base ? base + 1 : m->filename, sizeof(buf));
    size_t n = strlen(buf);
    if (n > 6 && strcasecmp(buf + n - 6, ".gcode") == 0) {
        buf[n - 6] = '\0';
    }
    strip_slicer_suffix(buf);
    emit(m, D_FILENAME, false, 0, buf[0] ? buf : "--");

    if (printing) {
        layer_text(m, buf, sizeof(buf));
        emit(m, D_LAYER, false, 0, buf);
    } else {
        emit(m, D_LAYER, false, 0, "--");
    }

    buf[0] = '\0';
    if (m->filename[0] && m->meta.thumb[0]) {
        snprintf(buf, sizeof(buf), "%s/server/files/gcodes/", m->base);
        url_append(buf, sizeof(buf), m->meta.thumb);
    }
    emit(m, D_THUMB, false, 0, buf);
}

/* Reports every bound path present in a Moonraker status object. */
static void report_status(mr_t *m, const cJSON *status)
{
    if (m->want_any) {
        take_inputs(m, status);
        update_derived(m);
    }
    char obj[SRC_PATH_MAX];
    for (int i = 0; i < m->npaths; i++) {
        const char *rest;
        if (not_klipper(m->paths[i]) || !split_path(m->paths[i], obj, sizeof(obj), &rest) ||
            !rest) {
            continue;
        }
        /* an update only carries what changed: skip paths it does not hold */
        double count;
        bool is_count;
        const cJSON *v = json_at(cJSON_GetObjectItemCaseSensitive(status, obj), rest, &count,
                                 &is_count);
        if (is_count) {
            report_number(m, m->paths[i], count);
        } else if (cJSON_IsNumber(v)) {
            report_number(m, m->paths[i], v->valuedouble);
        } else if (cJSON_IsBool(v)) {
            report_number(m, m->paths[i], cJSON_IsTrue(v) ? 1 : 0);
        } else if (cJSON_IsString(v)) {
            report_text(m, m->paths[i], v->valuestring);
        }
    }
}

static void handle_message(mr_t *m, const char *json, int len)
{
    /* Host stats come every second whether asked for or not: drop them
     * before parsing when no widget reads them. */
    static const char PROC_NOTE[] = "\"notify_proc_stat_update\"";
    if (!m->gwant[G_PROC] && memmem(json, len < 80 ? len : 80, PROC_NOTE, sizeof(PROC_NOTE) - 1)) {
        return;
    }
    cJSON *msg = cJSON_ParseWithLength(json, len);
    if (!msg) {
        return;
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(msg, "result");
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(msg, "error");
    const char *method = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(msg, "method"));

    if (cJSON_IsNumber(id) && id->valueint == ID_INFO) {
        const char *state = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(result, "klippy_state"));
        if (state && strcmp(state, "ready") == 0) {
            subscribe(m);
        } else {
            char d[64];
            const char *em = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "message"));
            if (em) {
                /* e.g. "Unauthorized" when the device is not in trusted_clients */
                snprintf(d, sizeof(d), "moonraker: %s", em);
            } else {
                snprintf(d, sizeof(d), "klipper %s", state ? state : "not reachable");
            }
            ESP_LOGW(TAG, "source %d: %s", m->index, d);
            report_state(m, SRC_OFFLINE, d);
            esp_timer_stop(m->retry);
            esp_timer_start_once(m->retry, RETRY_INFO_US);
        }
    } else if (cJSON_IsNumber(id) && id->valueint == ID_SUBSCRIBE) {
        if (error) {
            char d[64];
            const char *em = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "message"));
            snprintf(d, sizeof(d), "subscribe failed: %s", em ? em : "error");
            report_state(m, SRC_OFFLINE, d);
            esp_timer_stop(m->retry);
            esp_timer_start_once(m->retry, RETRY_INFO_US);
        } else {
            m->subscribed = true;
            esp_timer_stop(m->retry);
            report_status(m, cJSON_GetObjectItemCaseSensitive(result, "status"));
            request_groups(m);
            char d[64] = "";
            if (m->rpc_refused[0]) {
                snprintf(d, sizeof(d), "rpc method %.32s not allowed", m->rpc_refused);
            }
            report_state(m, SRC_ONLINE, d);
            ESP_LOGI(TAG, "subscribed to %d paths", m->npaths);
        }
    } else if (cJSON_IsNumber(id) && id->valueint == ID_METADATA) {
        on_metadata(m, result);
        update_derived(m);
        report_group(m, G_META, result);
    } else if (cJSON_IsNumber(id) && id->valueint == ID_PROC) {
        report_group(m, G_PROC, result);
    } else if (cJSON_IsNumber(id) && id->valueint == ID_SYSTEM) {
        report_group(m, G_SYSTEM, cJSON_GetObjectItemCaseSensitive(result, "system_info"));
    } else if (cJSON_IsNumber(id) && id->valueint >= ID_RPC && id->valueint < ID_RPC + m->nrpc) {
        on_rpc_reply(m, id->valueint - ID_RPC, result); /* an error reply shows "--" */
    } else if (cJSON_IsNumber(id) && id->valueint == ID_HISTORY) {
        report_group(m, G_HISTORY, cJSON_GetObjectItemCaseSensitive(result, "job_totals"));
    } else if (cJSON_IsNumber(id) && id->valueint == ID_QUEUE) {
        report_group(m, G_QUEUE, result);
    } else if (method && strcmp(method, "notify_history_changed") == 0) {
        if (m->gwant[G_HISTORY]) {
            send_json(m, "server.history.totals", ID_HISTORY, NULL); /* a job was added or finished */
        }
    } else if (method && strcmp(method, "notify_job_queue_changed") == 0) {
        if (m->gwant[G_QUEUE]) {
            send_json(m, "server.job_queue.status", ID_QUEUE, NULL);
        }
    } else if (method && strcmp(method, "notify_proc_stat_update") == 0) {
        report_group(m, G_PROC, cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(msg, "params"), 0));
    } else if (method && strcmp(method, "notify_status_update") == 0) {
        report_status(m, cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(msg, "params"), 0));
    } else if (method && strcmp(method, "notify_klippy_ready") == 0) {
        m->subscribed = false;
        send_json(m, "server.info", ID_INFO, NULL); /* subscriptions end with klippy */
        esp_timer_stop(m->retry);
        esp_timer_start_once(m->retry, RETRY_INFO_US);
    } else if (method && (strcmp(method, "notify_klippy_disconnected") == 0 ||
                          strcmp(method, "notify_klippy_shutdown") == 0)) {
        m->subscribed = false;
        report_state(m, SRC_OFFLINE, "klipper not ready");
        esp_timer_stop(m->retry);
        esp_timer_start_once(m->retry, RETRY_INFO_US);
    }
    cJSON_Delete(msg);
}

/* Until subscribed, asks again every RETRY_INFO_US, also when a reply never
 * came (Moonraker restarting while a request was on its way). */
static void on_retry(void *arg)
{
    mr_t *m = arg;
    if (m->subscribed || !esp_websocket_client_is_connected(m->ws)) {
        return;
    }
    send_json(m, "server.info", ID_INFO, NULL);
    esp_timer_start_once(m->retry, RETRY_INFO_US);
}

static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    mr_t *m = arg;
    const esp_websocket_event_data_t *e = event_data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "connected to %s", m->uri);
        m->backoff_ms = BACKOFF_MIN_MS;
        esp_websocket_client_set_reconnect_timeout(m->ws, m->backoff_ms);
        report_state(m, SRC_STARTING, "connected, waiting for klipper");
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "client_name", "tavle");
        cJSON_AddStringToObject(p, "version", "0.1");
        cJSON_AddStringToObject(p, "type", "other");
        cJSON_AddStringToObject(p, "url", "https://github.com/AalmanSadath/tavle");
        m->subscribed = false;
        send_json(m, "server.connection.identify", ID_IDENTIFY, p);
        send_json(m, "server.info", ID_INFO, NULL);
        esp_timer_stop(m->retry);
        esp_timer_start_once(m->retry, RETRY_INFO_US);
        break;
    }

    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR: {
        char d[64];
        int code = e ? e->error_handle.esp_ws_handshake_status_code : 0;
        if (code == 401 || code == 403) {
            snprintf(d, sizeof(d), "refused (HTTP %d): add tavle to trusted_clients", code);
        } else if (code) {
            snprintf(d, sizeof(d), "refused (HTTP %d)", code);
        } else {
            snprintf(d, sizeof(d), "cannot reach %s", m->uri + 5);
        }
        m->subscribed = false;
        report_state(m, SRC_OFFLINE, d);
        esp_timer_stop(m->retry);
        /* a reply lost with the connection: ask again once back */
        m->filename[0] = '\0';
        memset(&m->meta, 0, sizeof(m->meta));
        memset(m->last, 0, sizeof(m->last));
        /* back off so an unreachable printer is not hammered */
        esp_websocket_client_set_reconnect_timeout(m->ws, m->backoff_ms);
        m->backoff_ms = m->backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : m->backoff_ms * 2;
        break;
    }

    case WEBSOCKET_EVENT_DATA:
        if (e->op_code != 0x1 && e->op_code != 0x0) {
            break; /* pings, pongs, close */
        }
        if (e->payload_offset == 0) {
            m->rx_len = 0;
            m->rx_skip = e->payload_len > RX_MAX;
            if (m->rx_skip) {
                ESP_LOGW(TAG, "message of %d bytes dropped", e->payload_len);
                if (!m->dropped_big && m->subscribed) {
                    m->dropped_big = true; /* most likely an rpc: reply */
                    report_state(m, SRC_ONLINE, "a reply over 4 KB was dropped");
                }
            }
        }
        if (m->rx_skip || e->payload_offset + e->data_len > RX_MAX) {
            break;
        }
        memcpy(m->rx + e->payload_offset, e->data_ptr, e->data_len);
        m->rx_len = e->payload_offset + e->data_len;
        if (m->rx_len == e->payload_len) {
            handle_message(m, m->rx, m->rx_len);
        }
        break;

    default:
        break;
    }
}

static bool host_ok(const char *h)
{
    size_t n = strlen(h);
    if (n == 0 || n > 63) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char)h[i]) && h[i] != '.' && h[i] != '-') {
            return false;
        }
    }
    return true;
}

static void mr_stop(void *ctx);

static void *mr_start(int index, const cJSON *config, const char *const *paths, int npaths)
{
    const char *host = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(config, "host"));
    const cJSON *port_item = cJSON_GetObjectItemCaseSensitive(config, "port");
    int port = cJSON_IsNumber(port_item) ? port_item->valueint : 7125;
    if (!host || !host_ok(host) || port < 1 || port > 65535) {
        sources_report_state(index, SRC_OFFLINE, "needs a valid host and port");
        return NULL;
    }

    mr_t *m = calloc(1, sizeof(*m));
    if (!m) {
        return NULL;
    }
    m->index = index;
    m->lock = xSemaphoreCreateMutex();
    if (!m->lock) {
        free(m);
        return NULL;
    }
    m->backoff_ms = BACKOFF_MIN_MS;
    snprintf(m->uri, sizeof(m->uri), "ws://%s:%d/websocket", host, port);
    snprintf(m->base, sizeof(m->base), "http://%s:%d", host, port);
    m->current_layer = m->total_layer = -1;
    m->rx = malloc(RX_MAX);
    m->paths = calloc(npaths, sizeof(char *));
    if (!m->rx || !m->paths) {
        mr_stop(m);
        return NULL;
    }
    for (int i = 0; i < npaths; i++) {
        m->paths[i] = strdup(paths[i]);
        if (!m->paths[i]) {
            mr_stop(m);
            return NULL;
        }
        m->npaths++;
        for (int d = 0; d < D_COUNT; d++) {
            if (strcmp(paths[i], DERIVED[d]) == 0) {
                m->want[d] = m->want_any = true;
            }
        }
        int g = group_of(paths[i]);
        if (g >= 0) {
            m->gwant[g] = true;
            /* metadata comes with the file name */
            m->want_any |= g == G_META;
        }
        char method[40];
        size_t skip;
        if (g == G_RPC && rpc_split(paths[i], method, sizeof(method), &skip)) {
            bool known = false;
            for (int k = 0; k < m->nrpc; k++) {
                known |= strcmp(m->rpc[k], method) == 0;
            }
            if (known) {
                /* already called */
            } else if (!rpc_allowed(method)) {
                ESP_LOGW(TAG, "source %d: rpc method %s not allowed", index, method);
                if (!m->rpc_refused[0]) {
                    strlcpy(m->rpc_refused, method, sizeof(m->rpc_refused));
                }
            } else if (m->nrpc < RPC_MAX) {
                strlcpy(m->rpc[m->nrpc++], method, sizeof(m->rpc[0]));
            }
        }
    }

    const esp_timer_create_args_t t = {.callback = on_retry, .arg = m, .name = "mr_retry"};
    if (esp_timer_create(&t, &m->retry) != ESP_OK) {
        mr_stop(m);
        return NULL;
    }
    if (m->nrpc) {
        const cJSON *poll = cJSON_GetObjectItemCaseSensitive(config, "poll");
        m->poll_s = cJSON_IsNumber(poll) ? poll->valueint : RPC_POLL_DEF;
        if (m->poll_s < RPC_POLL_MIN) {
            m->poll_s = RPC_POLL_MIN;
        }
        const esp_timer_create_args_t pt = {.callback = on_poll, .arg = m, .name = "mr_poll"};
        if (esp_timer_create(&pt, &m->poll) != ESP_OK ||
            esp_timer_start_periodic(m->poll, m->poll_s * 1000000LL) != ESP_OK) {
            mr_stop(m);
            return NULL;
        }
    }

    const esp_websocket_client_config_t cfg = {
        .uri = m->uri,
        .buffer_size = 1024,
        .task_stack = 5120,
        .task_name = "moonraker",
        .reconnect_timeout_ms = BACKOFF_MIN_MS,
        /* a stopped source's client lives on until a connect in progress
         * gives up; keep that short */
        .network_timeout_ms = 3000,
        .ping_interval_sec = 10,
        .pingpong_timeout_sec = 30,
        .user_agent = "tavle",
    };
    m->ws = esp_websocket_client_init(&cfg);
    if (!m->ws) {
        mr_stop(m);
        return NULL;
    }
    esp_websocket_register_events(m->ws, WEBSOCKET_EVENT_ANY, on_ws_event, m);
    if (esp_websocket_client_start(m->ws) != ESP_OK) {
        mr_stop(m);
        return NULL;
    }
    ESP_LOGI(TAG, "source %d: %s, %d paths", index, m->uri, m->npaths);
    return m;
}

static void destroy(mr_t *m)
{
    /* The websocket task can arm the retry timer, so stop the timer on both
     * sides of destroying the client. */
    if (m->retry) {
        esp_timer_stop(m->retry);
    }
    if (m->ws) {
        esp_websocket_client_destroy(m->ws); /* waits for its task to end */
    }
    if (m->retry) {
        esp_timer_stop(m->retry);
        esp_timer_delete(m->retry);
    }
    if (m->poll) {
        esp_timer_stop(m->poll);
        esp_timer_delete(m->poll);
    }
    for (int i = 0; i < m->npaths; i++) {
        free(m->paths[i]);
    }
    free(m->paths);
    free(m->rx);
    vSemaphoreDelete(m->lock);
    free(m);
}

static void destroy_task(void *arg)
{
    mr_t *m = arg;
    TaskHandle_t waiter = m->waiter;
    destroy(m);
    xTaskNotifyGive(waiter);
    vTaskDelete(NULL);
}

/* A connected client is gone in about 100 ms, but a connect in progress can
 * take network_timeout_ms to give up, and a screen switch must not wait that
 * long. So the client is destroyed on its own task, waited for up to
 * STOP_WAIT_MS so its memory is back before the next source starts, and
 * otherwise left to finish. Nothing is reported after this returns. */
#define STOP_WAIT_MS 500

static void mr_stop(void *ctx)
{
    mr_t *m = ctx;
    const int index = m->index; /* m may be gone once destroy_task runs */
    xSemaphoreTake(m->lock, portMAX_DELAY);
    m->dead = true;
    xSemaphoreGive(m->lock);
    if (m->retry) {
        esp_timer_stop(m->retry);
    }
    if (m->poll) {
        esp_timer_stop(m->poll);
    }
    m->waiter = xTaskGetCurrentTaskHandle();
    xTaskNotifyStateClear(NULL);
    ulTaskNotifyValueClear(NULL, UINT32_MAX);
    if (!m->ws || xTaskCreate(destroy_task, "mr_stop", 4096, m, 3, NULL) != pdPASS) {
        destroy(m);
        return;
    }
    if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(STOP_WAIT_MS))) {
        ESP_LOGI(TAG, "source %d: still connecting, closing in the background", index);
    }
}

const source_driver_t src_moonraker_driver = {
    .type = "moonraker",
    .start = mr_start,
    .stop = mr_stop,
};
