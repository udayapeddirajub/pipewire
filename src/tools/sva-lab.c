/* Client-side LAB (look-ahead buffer) processing.
 *
 * The phase2 plugin drains the LAB synchronously to a raw PCM file under
 * /tmp as sva_lab_dump_<unixtime>.pcm, completing the write before it flips
 * detection status to "detected". On DETECTED we locate the newest dump and
 * wrap it in a canonical WAV header using the platform's fixed SVA capture
 * format (16 kHz / mono / 16-bit) so the captured audio is playable.
 */
#include "config.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "sva-voiceui.h"

#define SVA_LAB_DUMP_DIR    "/tmp"
#define SVA_LAB_DUMP_PREFIX "sva_lab_dump_"
#define SVA_LAB_DUMP_SUFFIX ".pcm"

#define SVA_LAB_DEFAULT_RATE     16000u
#define SVA_LAB_DEFAULT_CHANNELS 1u
#define SVA_LAB_DEFAULT_BITS     16u

bool sva_lab_find_latest_dump(char *out, size_t out_sz)
{
    DIR *dir;
    struct dirent *ent;
    time_t best_mtime = 0;
    char best[1024];
    bool found = false;

    if (out == NULL || out_sz == 0)
        return false;

    dir = opendir(SVA_LAB_DUMP_DIR);
    if (dir == NULL) {
        fprintf(stderr, "LAB: can't open %s: %s\n",
                SVA_LAB_DUMP_DIR, strerror(errno));
        return false;
    }

    while ((ent = readdir(dir)) != NULL) {
        char path[1024];
        struct stat st;
        size_t len = strlen(ent->d_name);
        size_t plen = strlen(SVA_LAB_DUMP_PREFIX);
        size_t slen = strlen(SVA_LAB_DUMP_SUFFIX);

        if (len < plen + slen)
            continue;
        if (strncmp(ent->d_name, SVA_LAB_DUMP_PREFIX, plen) != 0)
            continue;
        if (strcmp(ent->d_name + len - slen, SVA_LAB_DUMP_SUFFIX) != 0)
            continue;

        snprintf(path, sizeof(path), "%s/%s", SVA_LAB_DUMP_DIR, ent->d_name);
        if (stat(path, &st) != 0)
            continue;

        if (!found || st.st_mtime >= best_mtime) {
            best_mtime = st.st_mtime;
            snprintf(best, sizeof(best), "%s", path);
            found = true;
        }
    }
    closedir(dir);

    if (!found)
        return false;

    snprintf(out, out_sz, "%s", best);
    return true;
}

static void write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

bool sva_lab_pcm_to_wav(const char *pcm_path, const char *wav_path)
{
    FILE *in = NULL, *out = NULL;
    uint8_t hdr[44];
    uint8_t buf[8192];
    size_t n;
    long pcm_size;
    uint32_t data_bytes, byte_rate;
    uint16_t block_align;
    const uint32_t rate = SVA_LAB_DEFAULT_RATE;
    const uint16_t channels = SVA_LAB_DEFAULT_CHANNELS;
    const uint16_t bits = SVA_LAB_DEFAULT_BITS;
    bool ok = false;

    in = fopen(pcm_path, "rb");
    if (in == NULL) {
        fprintf(stderr, "LAB: can't open %s: %s\n", pcm_path, strerror(errno));
        return false;
    }

    if (fseek(in, 0, SEEK_END) != 0 || (pcm_size = ftell(in)) < 0) {
        fprintf(stderr, "LAB: can't size %s: %s\n", pcm_path, strerror(errno));
        goto done;
    }
    rewind(in);
    data_bytes = (uint32_t)pcm_size;

    block_align = (uint16_t)(channels * (bits / 8));
    byte_rate = rate * block_align;

    memcpy(hdr + 0, "RIFF", 4);
    write_le32(hdr + 4, 36 + data_bytes);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    write_le32(hdr + 16, 16);
    write_le16(hdr + 20, 1);
    write_le16(hdr + 22, channels);
    write_le32(hdr + 24, rate);
    write_le32(hdr + 28, byte_rate);
    write_le16(hdr + 32, block_align);
    write_le16(hdr + 34, bits);
    memcpy(hdr + 36, "data", 4);
    write_le32(hdr + 40, data_bytes);

    out = fopen(wav_path, "wb");
    if (out == NULL) {
        fprintf(stderr, "LAB: can't create %s: %s\n", wav_path, strerror(errno));
        goto done;
    }

    if (fwrite(hdr, 1, sizeof(hdr), out) != sizeof(hdr)) {
        fprintf(stderr, "LAB: short header write to %s\n", wav_path);
        goto done;
    }

    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fprintf(stderr, "LAB: short data write to %s\n", wav_path);
            goto done;
        }
    }

    ok = true;

done:
    if (in)
        fclose(in);
    if (out)
        fclose(out);
    return ok;
}

void sva_lab_on_detected(struct data *data)
{
    char pcm[1024];
    char wav[1024];
    size_t len;

    (void)data;

    if (!sva_lab_find_latest_dump(pcm, sizeof(pcm))) {
        printf("[lab] no LAB dump found under %s "
               "(capture may be disabled; set sva.lab.enabled true)\n",
               SVA_LAB_DUMP_DIR);
        return;
    }

    printf("[lab] captured keyword audio: %s\n", pcm);

    snprintf(wav, sizeof(wav), "%s", pcm);
    len = strlen(wav);
    if (len > 4 && strcmp(wav + len - 4, SVA_LAB_DUMP_SUFFIX) == 0)
        snprintf(wav + len - 4, sizeof(wav) - (len - 4), ".wav");
    else
        snprintf(wav + len, sizeof(wav) - len, ".wav");

    if (sva_lab_pcm_to_wav(pcm, wav)) {
        printf("[lab] wrapped to playable WAV: %s "
               "(%u Hz, %u ch, %u-bit)\n", wav,
               SVA_LAB_DEFAULT_RATE,
               SVA_LAB_DEFAULT_CHANNELS,
               SVA_LAB_DEFAULT_BITS);
    }
}
