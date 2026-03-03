/*
 * stretch_plugin.cpp - Time-stretch DSP plugin for Move Anything
 *
 * Loads WAV files, plays them back in a loop through Bungee time-stretcher
 * for real-time pitch-preserving speed changes. Exposes controls for target
 * BPM, bar length, and saving the stretched result.
 *
 * Plugin API v2 implementation.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdarg>
#include <algorithm>

#include <bungee/Bungee.h>

/* ------------------------------------------------------------------ */
/*  Embedded host/plugin API structs (avoid cross-repo include)       */
/* ------------------------------------------------------------------ */

extern "C" {

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
    int (*get_clock_status)(void);
} host_api_v1_t;

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host);

} // extern "C"

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

static const int    SAMPLE_RATE = 44100;
static const int    MAX_FRAMES  = SAMPLE_RATE * 600;  // 10 minutes

static const int    NUM_BARS_OPTIONS = 7;
static const float  BARS_OPTIONS[NUM_BARS_OPTIONS] = {
    0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f
};
static const char *BARS_LABELS[NUM_BARS_OPTIONS] = {
    "1/4", "1/2", "1", "2", "4", "8", "16"
};

/* Output accumulator capacity (frames). Must be large enough to hold
 * several Bungee output grains. 16384 frames ~= 0.37s at 44.1kHz. */
static const int OUT_BUF_CAPACITY = 16384;

/* ------------------------------------------------------------------ */
/*  Instance state                                                    */
/* ------------------------------------------------------------------ */

struct instance_t {
    char file_path[512];
    char file_name[128];
    char module_dir[512];
    char save_result[256];
    char save_dir[512];

    /* Source audio (stereo interleaved float) */
    float *audio_data;
    int    audio_frames;
    float  duration_secs;

    /* Parameters */
    float  project_bpm;
    int    target_bpm;
    int    bars_index;      // index into BARS_OPTIONS
    int    save_mode;       // 0 = overwrite, 1 = copy
    int    pitch_semitones; // -12 to +12 (0 = no pitch change)

    int    playing;
    float  speed;           // playback speed ratio

    /* Bungee stretcher */
    Bungee::Stretcher<Bungee::Basic> *stretcher;
    Bungee::Request req;
    float *grain_input;     // non-interleaved buffer [max_grain * 2]
    int    max_grain;       // maxInputFrameCount from stretcher

    /* Output accumulator (interleaved stereo float) */
    float *out_buf;
    int    out_count;       // frames currently buffered
};

/* ------------------------------------------------------------------ */
/*  Globals                                                           */
/* ------------------------------------------------------------------ */

static const host_api_v1_t *s_host = nullptr;

/* ------------------------------------------------------------------ */
/*  Utility helpers                                                   */
/* ------------------------------------------------------------------ */

static void host_log(const char *fmt, ...) {
    if (!s_host || !s_host->log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s_host->log(buf);
}

static const char *basename_ptr(const char *path) {
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

static void strip_ext(char *dst, size_t dst_sz, const char *filename) {
    snprintf(dst, dst_sz, "%s", filename);
    char *dot = strrchr(dst, '.');
    if (dot) *dot = '\0';
}

/* ------------------------------------------------------------------ */
/*  Bungee helpers                                                    */
/* ------------------------------------------------------------------ */

/* Feed a grain to the stretcher from interleaved audio_data.
 * Handles out-of-bounds frames via muteHead/muteTail. */
static void feed_grain(instance_t *inst, const Bungee::InputChunk &chunk) {
    int len = chunk.end - chunk.begin;
    int stride = inst->max_grain;

    memset(inst->grain_input, 0, stride * 2 * sizeof(float));

    for (int i = 0; i < len; i++) {
        int src = chunk.begin + i;
        if (src >= 0 && src < inst->audio_frames) {
            inst->grain_input[i]          = inst->audio_data[src * 2 + 0];
            inst->grain_input[stride + i] = inst->audio_data[src * 2 + 1];
        }
    }

    int muteHead = std::max(0, -chunk.begin);
    int muteTail = std::max(0, chunk.end - inst->audio_frames);

    inst->stretcher->analyseGrain(inst->grain_input, stride, muteHead, muteTail);
}

/* Convert semitones to Bungee pitch multiplier. */
static inline double pitch_multiplier(int semitones) {
    return pow(2.0, semitones / 12.0);
}

/* Reset Bungee request for playback start or loop wrap. */
static void reset_stretcher(instance_t *inst, double position) {
    inst->req.position = position;
    inst->req.speed = (double)inst->speed;
    inst->req.pitch = pitch_multiplier(inst->pitch_semitones);
    inst->req.reset = true;
    inst->req.resampleMode = resampleMode_autoOut;
    inst->stretcher->preroll(inst->req);
}

/* ------------------------------------------------------------------ */
/*  Speed calculation                                                 */
/* ------------------------------------------------------------------ */

static void recalc_speed(instance_t *inst) {
    if (inst->audio_frames == 0 || inst->target_bpm <= 0) {
        inst->speed = 1.0f;
        return;
    }
    float target_bars = BARS_OPTIONS[inst->bars_index];
    float target_duration = target_bars * 4.0f * 60.0f / (float)inst->target_bpm;
    if (target_duration <= 0.0f) {
        inst->speed = 1.0f;
        return;
    }
    inst->speed = inst->duration_secs / target_duration;

    /* Update live stretcher speed (takes effect on next grain) */
    inst->req.speed = (double)inst->speed;
}

/* ------------------------------------------------------------------ */
/*  Auto-bars guess: snap source duration to nearest bar option       */
/* ------------------------------------------------------------------ */

static void auto_guess_bars(instance_t *inst) {
    if (inst->duration_secs <= 0.0f || inst->project_bpm <= 0.0f) return;

    float approx = inst->duration_secs * inst->project_bpm / 240.0f;

    int best = 0;
    float best_dist = fabsf(approx - BARS_OPTIONS[0]);
    for (int i = 1; i < NUM_BARS_OPTIONS; i++) {
        float d = fabsf(approx - BARS_OPTIONS[i]);
        if (d < best_dist) {
            best_dist = d;
            best = i;
        }
    }
    inst->bars_index = best;
}

/* ------------------------------------------------------------------ */
/*  WAV loader                                                        */
/* ------------------------------------------------------------------ */

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static bool load_wav(instance_t *inst, const char *path) {
    if (inst->audio_data) {
        free(inst->audio_data);
        inst->audio_data = nullptr;
        inst->audio_frames = 0;
        inst->duration_secs = 0.0f;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        host_log("[stretch] cannot open %s", path);
        return false;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size < 44) {
        host_log("[stretch] file too small: %ld bytes", file_size);
        fclose(fp);
        return false;
    }

    uint8_t *raw = (uint8_t *)malloc(file_size);
    if (!raw) { fclose(fp); return false; }

    size_t read_bytes = fread(raw, 1, file_size, fp);
    fclose(fp);

    if ((long)read_bytes != file_size) { free(raw); return false; }

    if (memcmp(raw, "RIFF", 4) != 0 || memcmp(raw + 8, "WAVE", 4) != 0) {
        host_log("[stretch] not a WAVE file");
        free(raw);
        return false;
    }

    uint16_t audio_format = 0, num_channels = 0, bits_per_sample = 0;
    uint32_t sample_rate = 0, data_size = 0;
    const uint8_t *data_ptr = nullptr;
    bool found_fmt = false, found_data = false;

    size_t pos = 12;
    while (pos + 8 <= (size_t)file_size) {
        const uint8_t *chunk = raw + pos;
        uint32_t chunk_size = read_u32(chunk + 4);

        if (memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16) {
            audio_format    = read_u16(chunk + 8);
            num_channels    = read_u16(chunk + 10);
            sample_rate     = read_u32(chunk + 12);
            bits_per_sample = read_u16(chunk + 22);
            found_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            data_ptr  = chunk + 8;
            data_size = chunk_size;
            found_data = true;
        }

        pos += 8 + chunk_size;
        if (chunk_size & 1) pos++;
        if (found_fmt && found_data) break;
    }

    if (!found_fmt || !found_data) { free(raw); return false; }

    bool is_pcm   = (audio_format == 1);
    bool is_float = (audio_format == 3);

    if ((!is_pcm && !is_float) || num_channels < 1 || num_channels > 2) {
        host_log("[stretch] unsupported format: fmt=%d ch=%d", audio_format, num_channels);
        free(raw);
        return false;
    }
    if (is_pcm && bits_per_sample != 16 && bits_per_sample != 24) { free(raw); return false; }
    if (is_float && bits_per_sample != 32) { free(raw); return false; }

    int bytes_per_sample = bits_per_sample / 8;
    int block_align = bytes_per_sample * num_channels;
    int total_frames = (int)(data_size / block_align);
    if (total_frames > MAX_FRAMES) total_frames = MAX_FRAMES;
    if (total_frames <= 0) { free(raw); return false; }

    float *buf = (float *)calloc(total_frames * 2, sizeof(float));
    if (!buf) { free(raw); return false; }

    const uint8_t *src = data_ptr;
    for (int i = 0; i < total_frames; i++) {
        float samples[2] = {0.0f, 0.0f};
        for (int ch = 0; ch < num_channels; ch++) {
            float val = 0.0f;
            if (is_pcm && bits_per_sample == 16) {
                int16_t s = (int16_t)read_u16(src);
                val = s / 32768.0f;
                src += 2;
            } else if (is_pcm && bits_per_sample == 24) {
                int32_t s = (int32_t)src[0] | ((int32_t)src[1] << 8) | ((int32_t)src[2] << 16);
                if (s & 0x800000) s |= (int32_t)0xFF000000;
                val = s / 8388608.0f;
                src += 3;
            } else if (is_float) {
                uint32_t u = read_u32(src);
                memcpy(&val, &u, sizeof(float));
                src += 4;
            }
            samples[ch] = val;
        }
        buf[i * 2 + 0] = samples[0];
        buf[i * 2 + 1] = (num_channels == 1) ? samples[0] : samples[1];
    }

    free(raw);

    inst->audio_data   = buf;
    inst->audio_frames = total_frames;
    inst->duration_secs = (float)total_frames / (float)sample_rate;

    const char *fname = basename_ptr(path);
    snprintf(inst->file_name, sizeof(inst->file_name), "%s", fname);

    host_log("[stretch] loaded %s: %d frames, %.1fs, %dHz %dch %dbit",
             fname, total_frames, inst->duration_secs,
             sample_rate, num_channels, bits_per_sample);

    return true;
}

/* ------------------------------------------------------------------ */
/*  WAV writer (16-bit stereo PCM)                                    */
/* ------------------------------------------------------------------ */

static void write_u16_le(uint8_t *p, uint16_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}

static void write_u32_le(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

static bool write_wav_stereo(const char *path, const float *data, int frames) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;

    uint32_t data_size = frames * 2 * sizeof(int16_t);
    uint32_t file_size = 36 + data_size;

    uint8_t header[44];
    memcpy(header + 0, "RIFF", 4);
    write_u32_le(header + 4, file_size);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    write_u32_le(header + 16, 16);
    write_u16_le(header + 20, 1);
    write_u16_le(header + 22, 2);
    write_u32_le(header + 24, SAMPLE_RATE);
    write_u32_le(header + 28, SAMPLE_RATE * 2 * 2);
    write_u16_le(header + 32, 4);
    write_u16_le(header + 34, 16);
    memcpy(header + 36, "data", 4);
    write_u32_le(header + 40, data_size);

    fwrite(header, 1, 44, fp);

    for (int i = 0; i < frames * 2; i++) {
        float s = data[i];
        if (s >  1.0f) s =  1.0f;
        if (s < -1.0f) s = -1.0f;
        int16_t val = (int16_t)(s * 32767.0f);
        uint8_t b[2];
        write_u16_le(b, (uint16_t)val);
        fwrite(b, 1, 2, fp);
    }

    fclose(fp);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Offline Bungee stretch for save                                   */
/* ------------------------------------------------------------------ */

static bool stretch_offline(instance_t *inst, float **out_data, int *out_frames) {
    /* Estimate output size */
    double ratio = (inst->speed > 0.01) ? inst->speed : 1.0;
    int estimated = (int)((double)inst->audio_frames / ratio) + SAMPLE_RATE;
    if (estimated > MAX_FRAMES) estimated = MAX_FRAMES;

    float *result = (float *)calloc(estimated * 2, sizeof(float));
    if (!result) return false;

    /* Create a temporary stretcher for offline processing */
    Bungee::Stretcher<Bungee::Basic> stretcher(
        Bungee::SampleRates{SAMPLE_RATE, SAMPLE_RATE}, 2, 0);

    int maxGrain = stretcher.maxInputFrameCount();
    float *grain_buf = (float *)calloc(maxGrain * 2, sizeof(float));
    if (!grain_buf) { free(result); return false; }

    Bungee::Request req{};
    req.position = 0.0;
    req.speed = (double)inst->speed;
    req.pitch = pitch_multiplier(inst->pitch_semitones);
    req.reset = true;
    req.resampleMode = resampleMode_autoOut;

    stretcher.preroll(req);

    int written = 0;
    int max_iters = estimated + 1000;  // safety limit

    while (req.position < (double)inst->audio_frames && written < estimated && max_iters-- > 0) {
        Bungee::InputChunk chunk = stretcher.specifyGrain(req);

        int len = chunk.end - chunk.begin;
        memset(grain_buf, 0, maxGrain * 2 * sizeof(float));

        for (int i = 0; i < len; i++) {
            int src = chunk.begin + i;
            if (src >= 0 && src < inst->audio_frames) {
                grain_buf[i]            = inst->audio_data[src * 2 + 0];
                grain_buf[maxGrain + i] = inst->audio_data[src * 2 + 1];
            }
        }

        int muteHead = std::max(0, -chunk.begin);
        int muteTail = std::max(0, chunk.end - inst->audio_frames);

        stretcher.analyseGrain(grain_buf, maxGrain, muteHead, muteTail);

        Bungee::OutputChunk output{};
        stretcher.synthesiseGrain(output);

        int toCopy = std::min(output.frameCount, estimated - written);
        for (int i = 0; i < toCopy; i++) {
            result[written * 2 + 0] = output.data[i];
            result[written * 2 + 1] = output.data[i + output.channelStride];
            written++;
        }

        stretcher.next(req);
        req.reset = false;
    }

    free(grain_buf);

    *out_data = result;
    *out_frames = written;

    host_log("[stretch] offline stretch: %d -> %d frames (speed=%.3f pitch=%+dst)",
             inst->audio_frames, written, inst->speed, inst->pitch_semitones);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Save stretched audio                                              */
/* ------------------------------------------------------------------ */

static void do_save(instance_t *inst) {
    if (!inst->audio_data || inst->audio_frames == 0) {
        snprintf(inst->save_result, sizeof(inst->save_result), "No audio loaded");
        return;
    }

    /* Perform offline stretch */
    float *stretched = nullptr;
    int stretched_frames = 0;

    if (!stretch_offline(inst, &stretched, &stretched_frames) || !stretched) {
        snprintf(inst->save_result, sizeof(inst->save_result), "Stretch failed");
        return;
    }

    /* Determine output path */
    char out_path[512];

    if (inst->save_mode == 0) {
        snprintf(out_path, sizeof(out_path), "%s", inst->file_path);
    } else {
        char stem[128];
        strip_ext(stem, sizeof(stem), inst->file_name);

        /* Use UI-provided save_dir, or fall back to default */
        const char *dir = inst->save_dir[0]
            ? inst->save_dir
            : "/data/UserData/UserLibrary/Samples/Move Everything/Stretch";
        host_log("[stretch] save_dir='%s' using dir='%s'", inst->save_dir, dir);

        char cmd[256];
        snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
        if (system(cmd) != 0) {
            host_log("[stretch] mkdir failed for %s", dir);
        }

        if (inst->pitch_semitones != 0)
            snprintf(out_path, sizeof(out_path), "%s/%.100s-%dbpm%+dst.wav",
                     dir, stem, inst->target_bpm, inst->pitch_semitones);
        else
            snprintf(out_path, sizeof(out_path), "%s/%.100s-%dbpm.wav",
                     dir, stem, inst->target_bpm);
    }

    host_log("[stretch] saving %d stretched frames to %s", stretched_frames, out_path);

    if (write_wav_stereo(out_path, stretched, stretched_frames)) {
        snprintf(inst->save_result, sizeof(inst->save_result), "ok");
    } else {
        snprintf(inst->save_result, sizeof(inst->save_result), "Error writing file");
    }

    free(stretched);
}

/* ------------------------------------------------------------------ */
/*  Plugin API v2 callbacks                                           */
/* ------------------------------------------------------------------ */

static void *stretch_create(const char *module_dir, const char *json_defaults) {
    instance_t *inst = (instance_t *)calloc(1, sizeof(instance_t));
    if (!inst) return nullptr;

    snprintf(inst->module_dir, sizeof(inst->module_dir), "%s", module_dir);

    inst->project_bpm = 120.0f;
    inst->target_bpm  = 120;
    inst->bars_index  = 2;  // "1" bar
    inst->save_mode   = 0;
    inst->speed       = 1.0f;

    /* Create Bungee stretcher */
    inst->stretcher = new Bungee::Stretcher<Bungee::Basic>(
        Bungee::SampleRates{SAMPLE_RATE, SAMPLE_RATE}, 2, 0);

    inst->max_grain = inst->stretcher->maxInputFrameCount();
    inst->grain_input = (float *)calloc(inst->max_grain * 2, sizeof(float));

    inst->out_buf = (float *)calloc(OUT_BUF_CAPACITY * 2, sizeof(float));
    inst->out_count = 0;

    /* Initialize request */
    inst->req.position = 0.0;
    inst->req.speed = 1.0;
    inst->req.pitch = pitch_multiplier(inst->pitch_semitones);
    inst->req.reset = true;
    inst->req.resampleMode = resampleMode_autoOut;

    host_log("[stretch] instance created (maxGrain=%d)", inst->max_grain);
    return inst;
}

static void stretch_destroy(void *ptr) {
    instance_t *inst = (instance_t *)ptr;
    if (!inst) return;
    if (inst->stretcher) delete inst->stretcher;
    if (inst->grain_input) free(inst->grain_input);
    if (inst->out_buf) free(inst->out_buf);
    if (inst->audio_data) free(inst->audio_data);
    free(inst);
    host_log("[stretch] instance destroyed");
}

static void stretch_on_midi(void *ptr, const uint8_t *msg, int len, int source) {
    (void)ptr; (void)msg; (void)len; (void)source;
}

static void stretch_set_param(void *ptr, const char *key, const char *val) {
    instance_t *inst = (instance_t *)ptr;
    if (!inst || !key || !val) return;

    if (strcmp(key, "file_path") == 0) {
        snprintf(inst->file_path, sizeof(inst->file_path), "%s", val);
        inst->playing = 0;
        inst->out_count = 0;
        inst->save_result[0] = '\0';

        if (load_wav(inst, val)) {
            if (inst->target_bpm <= 0)
                inst->target_bpm = (int)roundf(inst->project_bpm);
            auto_guess_bars(inst);
            recalc_speed(inst);
        }
    }
    else if (strcmp(key, "project_bpm") == 0) {
        inst->project_bpm = (float)atof(val);
        if (inst->project_bpm < 20.0f)  inst->project_bpm = 20.0f;
        if (inst->project_bpm > 300.0f) inst->project_bpm = 300.0f;

        if (inst->audio_frames > 0) {
            auto_guess_bars(inst);
            recalc_speed(inst);
        }
    }
    else if (strcmp(key, "target_bpm") == 0) {
        inst->target_bpm = atoi(val);
        if (inst->target_bpm < 40)  inst->target_bpm = 40;
        if (inst->target_bpm > 300) inst->target_bpm = 300;
        recalc_speed(inst);
    }
    else if (strcmp(key, "target_bars") == 0) {
        int idx = atoi(val);
        if (idx < 0) idx = 0;
        if (idx >= NUM_BARS_OPTIONS) idx = NUM_BARS_OPTIONS - 1;
        inst->bars_index = idx;
        recalc_speed(inst);
    }
    else if (strcmp(key, "pitch_semitones") == 0) {
        int v = atoi(val);
        if (v < -12) v = -12;
        if (v >  12) v =  12;
        inst->pitch_semitones = v;
        inst->req.pitch = pitch_multiplier(v);
    }
    else if (strcmp(key, "save_mode") == 0) {
        inst->save_mode = atoi(val);
        if (inst->save_mode < 0) inst->save_mode = 0;
        if (inst->save_mode > 1) inst->save_mode = 1;
    }
    else if (strcmp(key, "save_dir") == 0) {
        snprintf(inst->save_dir, sizeof(inst->save_dir), "%s", val);
        host_log("[stretch] save_dir set to: '%s'", inst->save_dir);
    }
    else if (strcmp(key, "playing") == 0) {
        int v = atoi(val);
        if (v && !inst->playing && inst->stretcher) {
            /* Starting playback: reset stretcher to beginning */
            inst->out_count = 0;
            reset_stretcher(inst, 0.0);
        }
        inst->playing = v ? 1 : 0;
    }
    else if (strcmp(key, "save_result") == 0) {
        snprintf(inst->save_result, sizeof(inst->save_result), "%s", val);
    }
    else if (strcmp(key, "save") == 0) {
        do_save(inst);
    }
}

static int stretch_get_param(void *ptr, const char *key, char *buf, int buf_len) {
    instance_t *inst = (instance_t *)ptr;
    if (!inst || !key || !buf || buf_len <= 0) return 0;

    if (strcmp(key, "target_bpm") == 0)
        return snprintf(buf, buf_len, "%d", inst->target_bpm);
    if (strcmp(key, "target_bars") == 0)
        return snprintf(buf, buf_len, "%d", inst->bars_index);
    if (strcmp(key, "target_bars_label") == 0)
        return snprintf(buf, buf_len, "%s", BARS_LABELS[inst->bars_index]);
    if (strcmp(key, "pitch_semitones") == 0)
        return snprintf(buf, buf_len, "%d", inst->pitch_semitones);
    if (strcmp(key, "save_mode") == 0)
        return snprintf(buf, buf_len, "%d", inst->save_mode);
    if (strcmp(key, "playing") == 0)
        return snprintf(buf, buf_len, "%d", inst->playing);
    if (strcmp(key, "speed") == 0)
        return snprintf(buf, buf_len, "%.3f", inst->speed);
    if (strcmp(key, "source_duration") == 0)
        return snprintf(buf, buf_len, "%.2f", inst->duration_secs);
    if (strcmp(key, "file_name") == 0)
        return snprintf(buf, buf_len, "%s", inst->file_name);
    if (strcmp(key, "save_result") == 0)
        return snprintf(buf, buf_len, "%s", inst->save_result);
    if (strcmp(key, "play_pos") == 0) {
        if (inst->audio_frames > 0 && inst->stretcher) {
            float frac = (float)(inst->req.position / (double)inst->audio_frames);
            if (frac < 0.0f) frac = 0.0f;
            if (frac > 1.0f) frac = 1.0f;
            return snprintf(buf, buf_len, "%.4f", frac);
        }
        return snprintf(buf, buf_len, "0.0");
    }

    return 0;
}

static int stretch_get_error(void *ptr, char *buf, int buf_len) {
    (void)ptr; (void)buf; (void)buf_len;
    return 0;
}

static void stretch_render_block(void *ptr, int16_t *out_lr, int frames) {
    instance_t *inst = (instance_t *)ptr;

    if (!inst || !inst->playing || !inst->stretcher ||
        !inst->audio_data || inst->audio_frames == 0) {
        memset(out_lr, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    /* Fill output accumulator until we have enough frames */
    int safety = 256;  // max grains per block (prevents infinite loop)
    while (inst->out_count < frames && safety-- > 0) {
        /* Handle loop wrap */
        if (inst->req.position >= (double)inst->audio_frames) {
            double wrapped = fmod(inst->req.position, (double)inst->audio_frames);
            if (wrapped < 0.0) wrapped = 0.0;
            reset_stretcher(inst, wrapped);
        }

        Bungee::InputChunk chunk = inst->stretcher->specifyGrain(inst->req);

        feed_grain(inst, chunk);

        Bungee::OutputChunk output{};
        inst->stretcher->synthesiseGrain(output);

        /* Append output frames to accumulator */
        int space = OUT_BUF_CAPACITY - inst->out_count;
        int toCopy = std::min(output.frameCount, space);
        for (int i = 0; i < toCopy; i++) {
            int idx = (inst->out_count + i) * 2;
            inst->out_buf[idx + 0] = output.data[i];
            inst->out_buf[idx + 1] = output.data[i + output.channelStride];
        }
        inst->out_count += toCopy;

        inst->stretcher->next(inst->req);
        inst->req.reset = false;
    }

    /* Drain `frames` from the accumulator to the output */
    int avail = std::min(inst->out_count, frames);
    for (int i = 0; i < avail; i++) {
        float L = inst->out_buf[i * 2 + 0];
        float R = inst->out_buf[i * 2 + 1];

        if (L >  1.0f) L =  1.0f;
        if (L < -1.0f) L = -1.0f;
        if (R >  1.0f) R =  1.0f;
        if (R < -1.0f) R = -1.0f;

        out_lr[i * 2 + 0] = (int16_t)(L * 32767.0f);
        out_lr[i * 2 + 1] = (int16_t)(R * 32767.0f);
    }

    /* Zero-fill if we ran short (shouldn't happen normally) */
    for (int i = avail; i < frames; i++) {
        out_lr[i * 2 + 0] = 0;
        out_lr[i * 2 + 1] = 0;
    }

    /* Shift remaining frames to front of accumulator */
    int remaining = inst->out_count - avail;
    if (remaining > 0) {
        memmove(inst->out_buf, inst->out_buf + avail * 2,
                remaining * 2 * sizeof(float));
    }
    inst->out_count = remaining;
}

/* ------------------------------------------------------------------ */
/*  Static API table and entry point                                  */
/* ------------------------------------------------------------------ */

static plugin_api_v2_t s_api = {
    /* api_version    */ 2,
    /* create_instance */ stretch_create,
    /* destroy_instance*/ stretch_destroy,
    /* on_midi        */ stretch_on_midi,
    /* set_param      */ stretch_set_param,
    /* get_param      */ stretch_get_param,
    /* get_error      */ stretch_get_error,
    /* render_block   */ stretch_render_block,
};

extern "C" plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    s_host = host;
    host_log("[stretch] plugin init v2 (Bungee %s %s)",
             Bungee::Stretcher<Bungee::Basic>::edition(),
             Bungee::Stretcher<Bungee::Basic>::version());
    return &s_api;
}
