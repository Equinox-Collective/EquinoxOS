#ifndef _EQUOS_CODEC_WAV_H
#define _EQUOS_CODEC_WAV_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    const uint8_t *data;      // Указатель на весь файл в памяти
    size_t size;              // Размер файла
    
    // Параметры формата (заполняются при инициализации)
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint32_t data_offset;     // Смещение до чанка "data"
    uint32_t data_size;       // Размер аудио-данных в байтах
    
    // Состояние воспроизведения
    double src_pos;           // Текущая дробная позиция во входных сэмплах (для ресемплинга)
} wav_decoder_t;

// Инициализирует декодер. Находит fmt и data, проверяет валидность.
// Возвращает 0 при успехе, отрицательное число при ошибке.
int wav_init(wav_decoder_t *dec, const uint8_t *file_data, size_t file_size);

// Декодирует и ресемплирует аудио во внутренний буфер в формате 16-bit Stereo PCM.
// out_samples - сколько СТЕРЕО-кадров (семплов) мы хотим получить (1 кадр = Left + Right = 4 байта).
// out_buffer - буфер, куда запишется результат (размер должен быть минимум out_samples * 2 * sizeof(int16_t)).
// target_rate - частота, которую ожидает звуковая карта (например, 44100 или 48000).
// Возвращает количество реально записанных СТЕРЕО-кадров. Возвращает 0 в конце файла.
size_t wav_decode(wav_decoder_t *dec, int16_t *out_buffer, size_t out_samples, uint32_t target_rate);

#endif /* _EQUOS_CODEC_WAV_H */