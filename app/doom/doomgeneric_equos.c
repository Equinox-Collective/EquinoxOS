#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomdef.h"      // Для типа boolean
#include "i_sound.h"      // Для sound_module_t
#include "s_sound.h"
#include "../../sdk/include/equos.h"
#include <stdio.h>
#include "w_wad.h"
#include "z_zone.h"

// --- Заглушки для линкера (ресемплинг) ---
int use_libsamplerate = 0;
float libsamplerate_scale = 1.0f;
extern int snd_sfxdevice;
extern int snd_musicdevice;
// --- Системные функции порта ---

// Кэш центральных координат — считаем один раз через SYS_GET_VESA_INFO.
// Делаем static, чтобы лишний syscall на каждый кадр не звать.
static int dg_center_x = 0;
static int dg_center_y = 0;
static int dg_center_ready = 0;

static void dg_compute_center(void) {
    // RAX = phys_fb, RBX = screen_width, RCX = screen_height, RDX = pitch.
    // _syscall возвращает только RAX, поэтому используем inline asm,
    // как делает sdk при необходимости. Проще: вызвать syscall и
    // забрать RBX/RCX через сохранённые регистры.
    uint64_t w = 0, h = 0;
    __asm__ volatile (
        "movq $32, %%rax\n\t"
        "int $0x80\n\t"
        "movq %%rbx, %0\n\t"
        "movq %%rcx, %1\n\t"
        : "=r"(w), "=r"(h)
        :
        : "rax", "rbx", "rcx", "rdx", "memory"
    );
    int sw = (int)w, sh = (int)h;
    if (sw <= 0 || sh <= 0) { sw = 1024; sh = 768; }  // fallback
    dg_center_x = (sw - DOOMGENERIC_RESX) / 2;
    dg_center_y = (sh - DOOMGENERIC_RESY) / 2;
    if (dg_center_x < 0) dg_center_x = 0;
    if (dg_center_y < 0) dg_center_y = 0;
    dg_center_ready = 1;
}

void DG_Init() {
    // Чистим буфер клавиатуры при старте
    for(int i = 0; i < 16; i++) _syscall(9, 0, 0, 0, 0, 0);
    dg_compute_center();
}

void DG_DrawFrame() {
    // Рисуем буфер Дума на экран по центру
    if (!dg_center_ready) dg_compute_center();
    _syscall(SYS_DRAW_BUFFER, dg_center_x, dg_center_y,
             DOOMGENERIC_RESX, DOOMGENERIC_RESY, (uintptr_t)DG_ScreenBuffer);
}

void DG_SleepMs(uint32_t ms) {
    sys_sleep(ms);
}

uint32_t DG_GetTicksMs() {
    // Получаем тики таймера (умножаем на 10, так как в ядре 100Гц)
    return (uint32_t)_syscall(6, 0, 0, 0, 0, 0);
}

// --- Звуковая система ---

void DG_InitSound() {
    printf("Equos: Sound initialized\n");
}

void DG_SubmitSamples(short* samples, int frameCount) {
    _syscall(20, (uintptr_t)samples, (uintptr_t)frameCount * 4, 0, 0, 0);
}

void DG_SetVolumes(int musicVol, int sfxVol) {
    // Пока не реализовано
}

// Функции-обертки для модулей Chocolate Doom
static boolean I_Equos_InitSound(boolean use_sfx_prefix) {
    DG_InitSound();
    return true;
}

#define MAX_CHANNELS 8
typedef struct {
    uint8_t* data;
    uint32_t length;
    uint32_t pos;
    int vol;
    int sep;
    int in_use;
} sfx_channel_t;

static sfx_channel_t channels[MAX_CHANNELS];

#define TARGET_FREQ 44100
#define DOOM_FREQ 11025
#define MIX_BUFFER_SIZE 1260 // 44100 / 35 fps (идеально для Doom)

static void I_Equos_UpdateSound(void) {
    static int16_t mixbuffer[MIX_BUFFER_SIZE * 2];
    memset(mixbuffer, 0, MIX_BUFFER_SIZE * 4);

    uint32_t step = (DOOM_FREQ << 16) / TARGET_FREQ;

    for(int i = 0; i < MAX_CHANNELS; i++) {
        if(!channels[i].in_use) continue;
        sfx_channel_t* ch = &channels[i];
        
        int32_t left_v = (254 - ch->sep) * ch->vol / 127;
        int32_t right_v = ch->sep * ch->vol / 127;

        for(int j = 0; j < MIX_BUFFER_SIZE; j++) {
            uint32_t src_idx = (ch->pos >> 16);
            if(src_idx >= ch->length) { ch->in_use = 0; break; }

            // Делим звук на 2 (сдвиг >> 9 вместо >> 8), чтобы не было "перегруза"
            int32_t sample = ((int32_t)ch->data[src_idx] - 128) << 8;
            int32_t l = (int32_t)mixbuffer[j*2] + ((sample * left_v) >> 9);
            int32_t r = (int32_t)mixbuffer[j*2+1] + ((sample * right_v) >> 9);
            
            // Клиппинг
            if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; else if (r < -32768) r = -32768;

            mixbuffer[j*2] = (int16_t)l;
            mixbuffer[j*2+1] = (int16_t)r;
            ch->pos += step;
        }
    }
    // Шлем РЕАЛЬНЫЙ размер (frames * 4 байта)
    DG_SubmitSamples(mixbuffer, MIX_BUFFER_SIZE);
}

static void I_Equos_SubmitSamples(void) {}
static int I_Equos_GetSfxLumpNum(sfxinfo_t* sfx) {
    char name[9];
    sprintf(name, "DS%s", sfx->name); // Звуки в вадах начинаются на DS...
    return W_GetNumForName(name);
}

static void I_Equos_UpdateSoundParams(int handle, int vol, int sep) {
    if(handle >= 0 && handle < MAX_CHANNELS) {
        channels[handle].vol = vol;
        channels[handle].sep = sep;
    }
}

static int I_Equos_StartSound(sfxinfo_t* sfx, int channel, int vol, int sep) {
    if(!sfx || channel < 0 || channel >= MAX_CHANNELS) return 0;
    
    // Получаем реальный номер люмпа
    int lump = I_Equos_GetSfxLumpNum(sfx);
    if (lump < 0) return 0;

    uint8_t* ptr = (uint8_t*)W_CacheLumpNum(lump, PU_STATIC);
    if (!ptr) return 0;

    // Выстрелим в консоль, чтобы знать, что звук пошел
    // printf("Playing sound: %s\n", sfx->name);

    uint16_t magic = ptr[0] | (ptr[1] << 8);
    if(magic != 3) return 0;
    
    uint32_t len = ptr[4] | (ptr[5] << 8) | (ptr[6] << 16) | (ptr[7] << 24);
    
    channels[channel].data = ptr + 8;
    channels[channel].length = len;
    channels[channel].pos = 0;
    channels[channel].vol = vol;
    channels[channel].sep = sep;
    channels[channel].in_use = 1;
    return channel;
}

static void I_Equos_StopSound(int handle) {
    if(handle >= 0 && handle < MAX_CHANNELS) {
        channels[handle].in_use = 0;
    }
}

static boolean I_Equos_SoundIsPlaying(int handle) {
    if(handle >= 0 && handle < MAX_CHANNELS) {
        return channels[handle].in_use;
    }
    return false;
}

static void I_Equos_CacheSounds(sfxinfo_t* sounds, int num_sounds) {}

static snddevice_t equos_devices[] = { SNDDEVICE_SB };

// Модуль звуковых эффектов
sound_module_t DG_sound_module = {
    equos_devices,
    1,
    I_Equos_InitSound,
    DG_InitSound,
    I_Equos_GetSfxLumpNum,      // Было NULL -> теперь функция
    I_Equos_UpdateSound,
    I_Equos_UpdateSoundParams,  // Было NULL -> теперь функция
    I_Equos_StartSound,         // Было NULL -> теперь функция
    I_Equos_StopSound,          // Было NULL -> теперь функция
    I_Equos_SoundIsPlaying,     // Было NULL -> теперь функция
    I_Equos_CacheSounds,        // Было NULL -> теперь функция
};

// Модуль музыки (заглушка)
static boolean I_Equos_InitMusic(void) { return true; }
static void I_Equos_ShutdownMusic(void) {}
static void I_Equos_SetMusicVolume(int vol) {}
static void I_Equos_PauseMusic(void) {}
static void I_Equos_ResumeMusic(void) {}
static void I_Equos_PlaySong(void* handle, boolean looping) {}
static void I_Equos_StopSong(void) {}
static void* I_Equos_RegisterSong(void* data, int len) { return (void*)1; }
static void I_Equos_UnRegisterSong(void* handle) {}
static boolean I_Equos_MusicIsPlaying(void) { return false; }
static void I_Equos_Poll(void) {}


music_module_t DG_music_module = {
    equos_devices,
    1,
    I_Equos_InitMusic,
    I_Equos_ShutdownMusic,
    I_Equos_SetMusicVolume,
    I_Equos_PauseMusic,
    I_Equos_ResumeMusic,
    I_Equos_RegisterSong,      // Было NULL
    I_Equos_UnRegisterSong,    // Было NULL
    I_Equos_PlaySong,
    I_Equos_StopSong,
    I_Equos_MusicIsPlaying,    // Было NULL
    I_Equos_Poll,               // Было NULL
};
// --- Ввод (Клавиатура) ---

int DG_GetKey(int* pressed, unsigned char* key) {
    uint8_t scancode = (uint8_t)_syscall(SYS_GET_SCANCODE, 0, 0, 0, 0, 0);
    if (scancode == 0) return 0;

    static int is_extended = 0;
    if (scancode == 0xE0) {
        is_extended = 1;
        return 0;
    }

    *pressed = !(scancode & 0x80);
    uint8_t clean = scancode & 0x7F;

    if (is_extended) {
        is_extended = 0;
        switch(clean) {
            case 0x48: *key = KEY_UPARROW; break;
            case 0x50: *key = KEY_DOWNARROW; break;
            case 0x4B: *key = KEY_LEFTARROW; break;
            case 0x4D: *key = KEY_RIGHTARROW; break;
            case 0x1D: *key = KEY_FIRE; break; 
            default: return 0;
        }
        return 1;
    }

    switch(clean) {
        case 0x1D: *key = KEY_FIRE; break;
        case 0x39: *key = KEY_USE; break;
        case 0x01: *key = KEY_ESCAPE; break;
        case 0x1C: *key = KEY_ENTER; break;
        case 0x0E: *key = KEY_BACKSPACE; break;

        // WASD маршрутизируется как в современных шутерах:
        //   W/S — вперёд/назад (UP/DOWN), A/D — стрейф (а не поворот,
        //   как делает LEFTARROW/RIGHTARROW). В classic Doom стрейф —
        //   отдельные клавиши KEY_STRAFE_L/R (по умолчанию ',' и '.').
        case 0x11: *key = KEY_UPARROW;    break; // W
        case 0x1F: *key = KEY_DOWNARROW;  break; // S
        case 0x1E: *key = KEY_STRAFE_L;   break; // A
        case 0x20: *key = KEY_STRAFE_R;   break; // D

        default: {
            static const char map[] = {
                0, 0, '1','2','3','4','5','6','7','8','9','0','-','=', 0,
                9, 'q','w','e','r','t','y','u','i','o','p','[',']', 13, 0,
                0, 'a','s','d','f','g','h','j','k','l',';','\'','`', 0, '\\',
                'z','x','c','v','b','n','m',',','.','/', 0, '*', 0, ' '
            };
            if (clean < sizeof(map) && map[clean] != 0) {
                *key = map[clean];
                return 1;
            }
            return 0;
        }
    }
    return 1;
}

// --- Точка входа ---

int main(int argc, char **argv) {
    printf("Equos: Starting Doom engine with sound...\n");
    
    // Передаем управление doomgeneric
    snd_sfxdevice = 3;   // SNDDEVICE_SB
    snd_musicdevice = 3;
    doomgeneric_Create(argc, argv);
    
    // Основной цикл
    while(1) {
        doomgeneric_Tick();
    }

    return 0;
}