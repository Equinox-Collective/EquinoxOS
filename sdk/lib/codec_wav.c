#include <codec_wav.h>
#include <string.h>

#pragma pack(push, 1)
typedef struct {
    char id[4];
    uint32_t size;
    char wave[4];
} WavHeader;

typedef struct {
    char id[4];
    uint32_t size;
} ChunkHeader;

typedef struct {
    uint16_t fmt;
    uint16_t ch;
    uint32_t rate;
    uint32_t brate;
    uint16_t align;
    uint16_t bps;
} FmtChunk;
#pragma pack(pop)

int wav_init(wav_decoder_t *dec, const uint8_t *file_data, size_t file_size) {
    if (!dec || !file_data || file_size < 44) return -1;

    dec->data = file_data;
    dec->size = file_size;
    dec->src_pos = 0.0;

    WavHeader *wh = (WavHeader *)file_data;
    if (strncmp(wh->id, "RIFF", 4) != 0 || strncmp(wh->wave, "WAVE", 4) != 0) {
        return -2; // Не WAV формат
    }

    FmtChunk fmt;
    int has_fmt = 0;
    dec->data_offset = 0;
    dec->data_size = 0;

    uint32_t offset = 12;
    while (offset < file_size - 8) {
        ChunkHeader *ch = (ChunkHeader *)(file_data + offset);
        if (strncmp(ch->id, "fmt ", 4) == 0) {
            if (ch->size >= 16) {
                memcpy(&fmt, file_data + offset + 8, sizeof(FmtChunk));
                dec->channels = fmt.ch;
                dec->sample_rate = fmt.rate;
                dec->bits_per_sample = fmt.bps;
                has_fmt = 1;
            }
        } else if (strncmp(ch->id, "data", 4) == 0) {
            dec->data_offset = offset + 8;
            dec->data_size = ch->size;
            break;
        }
        offset += 8 + ch->size;
    }

    if (!has_fmt || dec->data_offset == 0) {
        return -3; // Не удалось найти нужные чанки
    }

    // Поддерживаем только PCM (несжатый)
    if (fmt.fmt != 1) {
        return -4; // Сжатые форматы не поддерживаются
    }

    return 0;
}

// Вспомогательная функция для чтения одного сырого сэмпла из конкретного канала
// и конвертации его в int16_t на лету.
static int16_t read_src_sample(wav_decoder_t *dec, size_t sample_idx, int channel) {
    size_t bytes_per_sample = dec->bits_per_sample / 8;
    size_t frame_size = dec->channels * bytes_per_sample;
    size_t offset = sample_idx * frame_size + channel * bytes_per_sample;

    // Защита от выхода за границы данных
    if (offset + bytes_per_sample > dec->data_size) {
        return 0; 
    }

    const uint8_t *ptr = dec->data + dec->data_offset + offset;

    if (dec->bits_per_sample == 8) {
        // 8-бит WAV беззнаковый (0..255), переводим в знаковую шкалу int16_t
        return (int16_t)(((int)*ptr - 128) * 256);
    } else if (dec->bits_per_sample == 16) {
        // 16-бит знаковый
        int16_t val;
        memcpy(&val, ptr, 2);
        return val;
    } else if (dec->bits_per_sample == 24) {
        // 24-бит знаковый: читаем 3 байта, расширяем знак до 32-битного int, сдвигаем до 16-битного
        int32_t val = (ptr[0] << 8) | (ptr[1] << 16) | (ptr[2] << 24);
        return (int16_t)(val >> 16);
    }

    return 0;
}

size_t wav_decode(wav_decoder_t *dec, int16_t *out_buffer, size_t out_samples, uint32_t target_rate) {
    if (!dec || !out_buffer || out_samples == 0) return 0;

    size_t bytes_per_sample = dec->bits_per_sample / 8;
    size_t frame_size = dec->channels * bytes_per_sample;
    size_t total_src_samples = dec->data_size / frame_size;

    double rate_ratio = (double)dec->sample_rate / (double)target_rate;
    size_t written = 0;

    for (size_t i = 0; i < out_samples; i++) {
        size_t idx0 = (size_t)dec->src_pos;
        size_t idx1 = idx0 + 1;

        if (idx0 >= total_src_samples) {
            break; // Достигнут конец файла
        }
        if (idx1 >= total_src_samples) {
            idx1 = idx0; // Клэмпим последний сэмпл
        }

        double t = dec->src_pos - (double)idx0; // Коэффициент интерполяции

        int16_t out_l = 0;
        int16_t out_r = 0;

        if (dec->channels == 1) {
            // Моно -> Стерео (дублируем каналы)
            int16_t s0 = read_src_sample(dec, idx0, 0);
            int16_t s1 = read_src_sample(dec, idx1, 0);
            out_l = (int16_t)(s0 + t * (s1 - s0));
            out_r = out_l;
        } else {
            // Стерео -> Стерео
            int16_t s0_l = read_src_sample(dec, idx0, 0);
            int16_t s1_l = read_src_sample(dec, idx1, 0);
            out_l = (int16_t)(s0_l + t * (s1_l - s0_l));

            int16_t s0_r = read_src_sample(dec, idx0, 1);
            int16_t s1_r = read_src_sample(dec, idx1, 1);
            out_r = (int16_t)(s0_r + t * (s1_r - s0_r));
        }

        out_buffer[written * 2]     = out_l;
        out_buffer[written * 2 + 1] = out_r;
        written++;

        dec->src_pos += rate_ratio;
    }

    return written;
}