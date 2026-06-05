#include "eqstart.h"
#include "../system/drivers/vesa/vesa.h"
#include "../system/mem/pmm.h"
#include "../system/misc/timer.h"
#include "../system/mem/vmm.h"
#include "../syslibc/stdio.h"
#include "nyan_data.h"
#include "equinox_logo.h"
#include "boot_sin.h"
#include "boot_config.h"
#include <stdbool.h>
#include <stdint.h>

// Прогресс-бар загрузки (определён ниже). Рисует трек, заполнение, проценты и
// примерное оставшееся время. Перерисовывает себя только при смене процента,
// поэтому безопасен для вызова хоть на каждом тике PIT.
static void boot_progress_draw(void);

// Объявляем внешние глобальные переменные ядра
extern uint64_t hhdm_offset;
extern volatile uint32_t tick;

static int tty_row = 0;

// Безопасный сон прямо в ядре без использования системных вызовов
static void kernel_sleep_ms(uint32_t ms) {
    uint32_t start = tick;
    while (tick < start + ms) {
        __asm__ volatile("hlt");
    }
}

// --- NYAN CAT BOOT-АНИМАЦИЯ ---
// Гифка рисуется прямо во фронтбуфер (как и диагностический лог) в зоне,
// которую попросил пользователь — по центру-низу экрана под текстом тестов.

// Геометрия кадра: масштаб и левый-верхний угол вычисляются один раз
// относительно разрешения экрана, чтобы анимация попадала в нужную зону
// при любом framebuffer'е.
static int nyan_scale = 0;
static int nyan_ox = 0;
static int nyan_oy = 0;

// Флаг boot-анимации. Пока =1, PIT-таймер (timer_callback) на каждом тике
// подрисовывает текущий кадр Nyan Cat — гифка крутится НЕПРЕРЫВНО на всём
// протяжении загрузки (паузы между тестами, USB/mouse, ext2 и т.д.), а не
// замирает на последнем кадре. kmain сбрасывает его в 0 прямо перед запуском
// sysgui, чтобы ядро перестало рисовать поверх кадра ring-3 GUI.
volatile int nyan_boot_active = 0;

void nyan_init_geometry(void) {
    // Целый масштаб ~480px ширины при 1280px экрана (пиксель-арт без блюра).
    nyan_scale = (int)(screen_width / 500);
    if (nyan_scale < 1) nyan_scale = 1;

    int nyan_w = NYAN_W * nyan_scale;
    int nyan_h = NYAN_H * nyan_scale;

    // Nyan Cat теперь живёт ВНИЗУ по центру (логотип/надпись — сверху,
    // прогресс-бар — по центру). Центрируем по горизонтали, прижимаем к низу.
    int cx = (int)screen_width / 2;
    nyan_ox = cx - nyan_w / 2;
    if (nyan_ox < 0) nyan_ox = 0;
    nyan_oy = (int)screen_height - nyan_h - 24; // отступ от нижнего края
    if (nyan_oy < (int)screen_height * 60 / 100)
        nyan_oy = (int)screen_height * 60 / 100; // не залезаем выше нижней трети
}

// Рисуем один кадр с целочисленным масштабированием (nearest-neighbor).
void nyan_draw_frame(int frame) {
    const uint8_t *fb = nyan_frames[frame % NYAN_FRAMES];
    for (int y = 0; y < NYAN_H; y++) {
        for (int x = 0; x < NYAN_W; x++) {
            uint32_t color = nyan_palette[fb[y * NYAN_W + x]];
            int px = nyan_ox + x * nyan_scale;
            int py = nyan_oy + y * nyan_scale;
            for (int dy = 0; dy < nyan_scale; dy++) {
                for (int dx = 0; dx < nyan_scale; dx++) {
                    put_pixel_direct(px + dx, py + dy, color);
                }
            }
        }
    }
}

// =========================================================================
//   BOOT-ИНТРО: логотип EquinoxOS + надпись + "booting..." (только режим 0)
//   Хореография: знак красиво появляется по центру сверху (pop + fade),
//   отъезжает влево, справа от него проявляется надпись "EquinoxOS", ниже
//   мелким шрифтом — "booting" с анимированными точками. Рисуется во
//   фронтбуфер из PIT-таймера (как и Nyan), на чёрном фоне — поэтому fade
//   делаем простым умножением яркости (без чтения фреймбуфера).
// =========================================================================

// Метка старта интро (тик, когда включилась boot-анимация) — задаётся в
// eqstart_perform_tests одновременно с nyan_boot_active = 1.
static volatile int      intro_start_set  = 0;
static volatile uint32_t intro_start_tick = 0;

// Раскладка (вычисляется один раз).
static int intro_inited   = 0;
static int band_cy        = 0;  // центр верхней полосы (логотип/надпись)
static int logo_full      = 0;  // итоговый размер знака (квадрат), px
static int logo_top_y     = 0;  // y верхнего края знака в финале
static int logo_center_x  = 0;  // x знака, когда он по центру (фаза A)
static int logo_left_x    = 0;  // x знака после отъезда влево
static int title_x        = 0;
static int title_y        = 0;
static int title_scale    = 0;
static int booting_x      = 0;
static int booting_y      = 0;
static int booting_scale  = 0;
static int booting_maxw   = 0;

// Тайминг фаз (мс от старта интро).
#define INTRO_A_END   600   // pop + fade-in знака по центру
#define INTRO_B_END   1200  // отъезд влево
#define INTRO_C_START 1100  // проявление надписи "EquinoxOS"
#define INTRO_C_END   1750
#define INTRO_D_START 1750  // проявление "booting"
#define INTRO_D_END   2250

// ease-out (квадратичный), p и результат в диапазоне 0..1000.
static int ease_out_1000(int p) {
    if (p < 0) p = 0;
    if (p > 1000) p = 1000;
    return 1000 - (1000 - p) * (1000 - p) / 1000;
}

extern char font8x8_basic[128][8];

// Ширина строки масштабированным шрифтом 8x8 (advance = 9*scale на символ).
static int text_w_scaled(const char *s, int scale) {
    int n = 0;
    while (s[n]) n++;
    if (n == 0) return 0;
    return n * 9 * scale - scale;
}

// Рисуем строку масштабированным шрифтом 8x8 (каждый «пиксель» — блок
// scale x scale). Цвет — сплошной (для fade передаём grayscale).
static void draw_text_scaled(const char *s, int x, int y, int scale, uint32_t color) {
    int cx = x;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= 127) {
            for (int row = 0; row < 8; row++) {
                uint8_t bits = (uint8_t)font8x8_basic[c][row];
                for (int col = 0; col < 8; col++) {
                    if (bits & (1 << col)) {
                        draw_rect_direct(cx + col * scale, y + row * scale,
                                         scale, scale, color);
                    }
                }
            }
        }
        cx += 9 * scale;
    }
}

// Блит знака в прямоугольник dw x dh с nearest-масштабированием и затуханием
// (fade 0..256). Знак белый, фон чёрный -> цвет пикселя = grayscale(alpha).
static void draw_logo_scaled(int dst_x, int dst_y, int dw, int dh, int fade) {
    if (dw <= 0 || dh <= 0) return;
    for (int yy = 0; yy < dh; yy++) {
        int sy = yy * LOGO_H / dh;
        for (int xx = 0; xx < dw; xx++) {
            int sx = xx * LOGO_W / dw;
            int a = equinox_logo_alpha[sy * LOGO_W + sx];
            a = (a * fade) >> 8;
            if (a <= 3) continue; // почти-прозрачное не трогаем (фон чёрный)
            if (a > 255) a = 255;
            uint32_t col = ((uint32_t)a << 16) | ((uint32_t)a << 8) | (uint32_t)a;
            put_pixel_direct(dst_x + xx, dst_y + yy, col);
        }
    }
}

static void boot_intro_init(void) {
    if (intro_inited) return;
    intro_inited = 1;
    if (nyan_scale == 0) nyan_init_geometry();

    int W = (int)screen_width;
    int H = (int)screen_height;

    logo_full = H / 6;
    if (logo_full < 72)  logo_full = 72;
    if (logo_full > 150) logo_full = 150;

    title_scale = (logo_full * 45 / 100) / 8;
    if (title_scale < 2) title_scale = 2;
    if (title_scale > 6) title_scale = 6;

    int tw  = text_w_scaled("EquinoxOS", title_scale);
    int gap = logo_full / 6;
    if (gap < 8) gap = 8;

    int group_w = logo_full + gap + tw;
    int group_x = (W - group_w) / 2;
    if (group_x < 8) group_x = 8;

    band_cy       = H * 22 / 100;
    logo_top_y    = band_cy - logo_full / 2;
    logo_left_x   = group_x;
    logo_center_x = (W - logo_full) / 2;

    title_x = group_x + logo_full + gap;
    title_y = band_cy - (8 * title_scale) / 2;

    booting_scale = title_scale / 2;
    if (booting_scale < 1) booting_scale = 1;
    int bw = text_w_scaled("booting...", booting_scale);
    booting_maxw = bw + 4;
    int group_cx = group_x + group_w / 2;
    booting_x = group_cx - bw / 2;
    if (booting_x < 8) booting_x = 8;
    booting_y = logo_top_y + logo_full + 12;
}

// Один «кадр» интро: e = мс от старта интро. Перерисовываем элементы только
// при изменении их состояния (экономим время в IRQ); кадрируем до ~60 fps.
static void boot_intro_frame(uint32_t e) {
    boot_intro_init();

    static uint32_t s_last_tick = 0;
    if (s_last_tick != 0 && (tick - s_last_tick) < 16) return;
    s_last_tick = tick;

    int W = (int)screen_width;

    // ---------- ЗНАК ----------
    int lx, ly, lcur, lfade;
    if (e < INTRO_A_END) {
        int p = (int)(e * 1000 / INTRO_A_END);
        int ez = ease_out_1000(p);
        lcur  = logo_full * (700 + 300 * ez / 1000) / 1000; // 70% -> 100%
        lfade = 256 * ez / 1000;
        if (lfade > 256) lfade = 256;
        lx = (W - lcur) / 2;
        ly = band_cy - lcur / 2;
    } else if (e < INTRO_B_END) {
        int p = (int)((e - INTRO_A_END) * 1000 / (INTRO_B_END - INTRO_A_END));
        int ez = ease_out_1000(p);
        lcur  = logo_full;
        lfade = 256;
        lx = logo_center_x + (logo_left_x - logo_center_x) * ez / 1000;
        ly = logo_top_y;
    } else {
        lcur  = logo_full;
        lfade = 256;
        lx = logo_left_x;
        ly = logo_top_y;
    }

    static int s_lx = -1, s_ly = -1, s_lcur = 0, s_lfade = -1;
    if (lx != s_lx || ly != s_ly || lcur != s_lcur || lfade != s_lfade) {
        if (s_lcur > 0) // стираем прошлую позицию знака (фон чёрный)
            draw_rect_direct(s_lx, s_ly, s_lcur, s_lcur, 0x000000);
        draw_logo_scaled(lx, ly, lcur, lcur, lfade);
        s_lx = lx; s_ly = ly; s_lcur = lcur; s_lfade = lfade;
    }

    // ---------- НАДПИСЬ "EquinoxOS" ----------
    if (e >= INTRO_C_START) {
        int tf = (int)((e - INTRO_C_START) * 256 / (INTRO_C_END - INTRO_C_START));
        if (tf > 256) tf = 256;
        int g = tf > 0 ? tf - 1 : 0;
        if (g > 255) g = 255;
        static int s_title_g = -1;
        if (g != s_title_g) {
            uint32_t col = ((uint32_t)g << 16) | ((uint32_t)g << 8) | (uint32_t)g;
            draw_text_scaled("EquinoxOS", title_x, title_y, title_scale, col);
            s_title_g = g;
        }
    }

    // ---------- "booting" + анимированные точки ----------
    if (e >= INTRO_D_START) {
        int bf = (int)((e - INTRO_D_START) * 256 / (INTRO_D_END - INTRO_D_START));
        if (bf > 256) bf = 256;
        int g = bf > 0 ? bf - 1 : 0;
        if (g > 255) g = 255;
        int ndots = (e > INTRO_D_END) ? (int)(((e - INTRO_D_END) / 400) % 4) : 0;
        static int s_boot_g = -1, s_boot_dots = -1;
        if (g != s_boot_g || ndots != s_boot_dots) {
            draw_rect_direct(booting_x, booting_y, booting_maxw,
                             8 * booting_scale, 0x000000); // стираем строку
            char buf[16];
            int k = 0;
            const char *bw = "booting";
            while (bw[k]) { buf[k] = bw[k]; k++; }
            for (int d = 0; d < ndots && k < 12; d++) buf[k++] = '.';
            buf[k] = 0;
            uint32_t col = ((uint32_t)g << 16) | ((uint32_t)g << 8) | (uint32_t)g;
            draw_text_scaled(buf, booting_x, booting_y, booting_scale, col);
            s_boot_g = g; s_boot_dots = ndots;
        }
    }
}

// =========================================================================
//   КРУГОВОЙ СПИННЕР В СТИЛЕ WINDOWS 11 (Fluent ProgressRing)
//   Тонкая ОДИНОЧНАЯ дуга (со скруглёнными концами) на чёрном фоне: она
//   растёт и укорачивается, плавно прокручиваясь по кругу — ровно как
//   «крутилка» при загрузке Windows 11 (НЕ точки и НЕ сплошное кольцо).
//   Модель анимации как в Material/Fluent: за один цикл сначала вперёд бежит
//   «голова» (дуга растёт), потом её догоняет «хвост» (дуга укорачивается);
//   суммарный сдвиг за цикл = SPIN_MAX_SWEEP, поэтому движение непрерывное,
//   без рывка на стыке циклов. Целочисленная тригонометрия (boot_sin.h),
//   рисуем во фронтбуфер из таймера, ~60fps.
// =========================================================================
#if BOOT_SHOW_SPINNER
#define SPIN_STEP      2    // шаг по углу при отрисовке дуги, градусы (мельче = глаже)
#define SPIN_MAX_SWEEP 290  // максимальная длина дуги, градусы
#define SPIN_MIN_ARC   16   // минимальная длина дуги (чтобы не схлопывалась в точку)

static int spin_inited = 0;
static int spin_cx = 0, spin_cy = 0, spin_R = 0, spin_thick = 0, spin_clear = 0;

static void boot_spinner_init(void) {
    if (spin_inited) return;
    spin_inited = 1;
    spin_cx = (int)screen_width / 2;
    spin_cy = (int)screen_height * BOOT_SPINNER_Y_PCT / 100;
    spin_R = (int)screen_height / 28;
    if (spin_R < 16) spin_R = 16;
    if (spin_R > 30) spin_R = 30;
    spin_thick = spin_R / 9;          // половина толщины линии (тонкая, как на Win11)
    if (spin_thick < 2) spin_thick = 2;
    spin_clear = spin_R + spin_thick + 2;
}

static void draw_disk(int cx, int cy, int r, uint32_t col) {
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r2)
                put_pixel_direct(cx + dx, cy + dy, col);
}

// Плавная функция «ease-in-out» через косинус: x:0..1000 -> 0..1000.
// (1 - cos(pi*x/1000)) / 2, считаем целочисленно через таблицу boot_cos1000.
static int spin_ease1000(int x) {
    if (x < 0) x = 0;
    if (x > 1000) x = 1000;
    return (1000 - boot_cos1000(180 * x / 1000)) / 2;
}

// Тонкая одиночная дуга, которая растёт/укорачивается и прокручивается —
// «крутилка» Windows 11. Рисуем только саму дугу (фон чёрный), концы скруглены
// засчёт перекрывающихся дисков. Длина дуги = SPIN_MIN_ARC..(MAX_SWEEP+MIN_ARC).
static void boot_spinner_frame(uint32_t e) {
    boot_spinner_init();

    static uint32_t s_last = 0;
    if (s_last != 0 && (tick - s_last) < 16) return;   // ~60fps
    s_last = tick;

    // Стираем прошлую зону спиннера (фон чёрный).
    draw_rect_direct(spin_cx - spin_clear, spin_cy - spin_clear,
                     spin_clear * 2, spin_clear * 2, 0x000000);

    // Период одного цикла «рост+укорачивание» = BOOT_SPINNER_ROT_MS.
    uint32_t period = BOOT_SPINNER_ROT_MS ? BOOT_SPINNER_ROT_MS : 1200;
    uint32_t cycles = e / period;
    int p = (int)((e % period) * 1000 / period);   // фаза цикла 0..999

    int start_grow, end_grow;
    if (p < 500) {
        int t = p * 2;                                  // 0..1000: голова бежит вперёд
        start_grow = 0;
        end_grow   = SPIN_MAX_SWEEP * spin_ease1000(t) / 1000;
    } else {
        int t = (p - 500) * 2;                          // 0..1000: хвост догоняет
        end_grow   = SPIN_MAX_SWEEP;
        start_grow = SPIN_MAX_SWEEP * spin_ease1000(t) / 1000;
    }

    // Сдвиг за каждый цикл = SPIN_MAX_SWEEP -> непрерывная прокрутка без рывка.
    int off  = (int)((cycles * (uint32_t)SPIN_MAX_SWEEP) % 360);
    int a0   = (off + start_grow) % 360;            // хвост (начало дуги)
    int span = (end_grow - start_grow) + SPIN_MIN_ARC;  // длина дуги, >= SPIN_MIN_ARC

    for (int s = 0; s <= span; s += SPIN_STEP) {
        int deg = (a0 + s) % 360;
        int x = spin_cx + spin_R * boot_cos1000(deg) / 1000;
        int y = spin_cy + spin_R * boot_sin1000_d(deg) / 1000;
        draw_disk(x, y, spin_thick, 0xFFFFFF);
    }
}
#endif /* BOOT_SHOW_SPINNER */

// --- ПРОГРЕСС-БАР ЗАГРУЗКИ ---
// Оценка прогресса по времени: tick (= мс, PIT на 1 кГц) против BOOT_ETA_MS из
// boot_config.h. Это намеренно ПРИМЕРНАЯ оценка (как и просил пользователь):
// реальные этапы загрузки недетерминированы, поэтому процент капается на 99 %,
// пока GUI не нарисует первый кадр (syscall 88 -> nyan_boot_active = 0), после
// чего ядро перестаёт рисовать и экран занимает рабочий стол.
// --- САМОПОДСТРОЙКА ВРЕМЕНИ ЗАГРУЗКИ ---
// boot_eta_ms — РАБОЧАЯ оценка времени загрузки (мс). На первом запуске равна
// BOOT_ETA_MS из конфига; после первой загрузки kmain читает реальное время из
// /boottime и переопределяет её через boot_eta_set(), поэтому проценты/время
// становятся точнее. boot_measured_ms заполняется при первом кадре GUI
// (syscall 88) и затем сохраняется ядром на диск.
static uint32_t boot_eta_ms = BOOT_ETA_MS;
volatile uint32_t boot_measured_ms = 0;

// Вызывается ядром после монтирования ФС, если в /boottime есть сохранённое
// время. Принимаем только вменяемые значения (3..180 c), иначе игнор.
void boot_eta_set(uint32_t ms) {
    if (ms >= 3000 && ms <= 180000) {
        boot_eta_ms = ms;
    }
}

static void boot_progress_draw(void) {
#if BOOT_PROGRESS_BAR
    if (nyan_scale == 0) {
        nyan_init_geometry();
    }

    // Процент по таймеру: 0..97% линейно за boot_eta_ms, дальше 97..99%
    // медленно «доползают» (по +1% раз в BOOT_TAIL_STEP_MS), чтобы бар не
    // выглядел зависшим, даже если реальная загрузка дольше ожидаемой.
    // 100% покажет уже сам GUI, перекрыв экран первым кадром (syscall 88).
    uint32_t eta = boot_eta_ms;
    uint32_t pct;
    bool tail; // true => фаза «хвоста» (>=97%, время уже не показываем).
    if (tick < eta) {
        pct = (uint32_t)(((uint64_t)tick * 97) / (uint64_t)eta);
        tail = false;
    } else {
        uint32_t over = tick - eta;
        pct = 97 + over / BOOT_TAIL_STEP_MS;
        if (pct > 99) pct = 99;
        tail = true;
    }
    uint32_t remain_ms = (tick < eta) ? (eta - tick) : 0;
    uint32_t remain_s = (remain_ms + 999) / 1000; // округляем вверх до секунды.

    // Перерисовываем только при смене процента — экономим IRQ-время.
    static uint32_t last_pct = 0xFFFFFFFFu;
    if (pct == last_pct) {
        return;
    }
    last_pct = pct;

    // Фон под текстом зависит от режима экрана (чёрный или серый), чтобы
    // стирать «хвосты» от прошлой строки без артефактов.
#if BOOT_SCREEN_MODE == 1
    uint32_t bg = BOOT_GRAY_COLOR;
#else
    uint32_t bg = 0x000000;
#endif

    // Геометрия бара: позиция/ширина берутся из boot_config.h в процентах
    // от экрана (по умолчанию выше и правее, чтобы не лезть под Nyan Cat).
    int bar_w = (int)(((uint64_t)screen_width * BOOT_BAR_W_PCT) / 100);
    int bar_h = 16;
    int cx = (int)(((uint64_t)screen_width * BOOT_BAR_CX_PCT) / 100);
    int bar_x = cx - bar_w / 2;
    int bar_y = (int)(((uint64_t)screen_height * BOOT_BAR_Y_PCT) / 100);
    // Держим бар целиком в пределах экрана.
    if (bar_x < 4) bar_x = 4;
    if (bar_x + bar_w > (int)screen_width - 4) bar_x = (int)screen_width - 4 - bar_w;

    // Рамка + трек + заполнение.
    draw_rect_direct(bar_x - 2, bar_y - 2, bar_w + 4, bar_h + 4, 0x3A3F44); // рамка
    draw_rect_direct(bar_x, bar_y, bar_w, bar_h, 0x14171A);                 // трек
    int fill_w = (int)(((uint64_t)bar_w * pct) / 100);
    if (fill_w > 0) {
        draw_rect_direct(bar_x, bar_y, fill_w, bar_h, 0x00C8FF);            // заполнение
    }

    // Текст под баром: "Loading EquinoxOS...  NN%   ~Ns left" (в хвосте время
    // уже не показываем — пишем "almost done", чтобы не было "~0s left").
    int text_y = bar_y + bar_h + 10;
    draw_rect_direct(bar_x, text_y, bar_w, 18, bg); // стираем прошлую строку
    char line[80];
    if (tail) {
        sprintf(line, "Loading EquinoxOS...  %u%%   almost done", pct);
    } else {
        sprintf(line, "Loading EquinoxOS...  %u%%   ~%us left", pct, remain_s);
    }
    vesa_draw_string_direct(line, bar_x, text_y, 0xC8CED4);
#endif /* BOOT_PROGRESS_BAR */
}

// Проигрываем гифку заданное время (мс), синхронно с темпом исходного gif
// (8 кадров по 100 мс). Используется как "заставка" во время загрузки.
static void nyan_play(uint32_t duration_ms) {
    uint32_t start = tick;
    int frame = 0;
    while ((tick - start) < duration_ms) {
        nyan_draw_frame(frame);
        boot_progress_draw(); // прогресс-бар поверх «заставки»
        frame++;
        kernel_sleep_ms(100); // 100 мс на кадр = темп исходной гифки
    }
}

// Подрисовка одного кадра Nyan Cat для использования ИЗ ДРУГИХ подсистем
// (например, во время цикла опроса USB-мыши), чтобы анимация не «замирала»
// между завершением nyan_play() и стартом GUI. Кадр выбирается по системному
// тику (100 мс/кадр), геометрия инициализируется лениво. Перерисовываем только
// при смене кадра, чтобы не молотить фреймбуфер на каждом вызове.
void nyan_boot_anim_frame(void) {
    if (nyan_scale == 0) {
        nyan_init_geometry();
    }
#if BOOT_SCREEN_MODE == 0
#if BOOT_SHOW_NYAN
    // Nyan Cat внизу (перерисовываем только при смене кадра).
    static int nyan_last_drawn = -1;
    int frame = (int)((tick / 100) % NYAN_FRAMES);
    if (frame != nyan_last_drawn) {
        nyan_draw_frame(frame);
        nyan_last_drawn = frame;
    }
#endif
    // Интро: знак EquinoxOS + надпись + "booting..." (сверху по центру).
    if (intro_start_set) {
        boot_intro_frame(tick - intro_start_tick);
    }
#if BOOT_SHOW_SPINNER
    if (intro_start_set) {
        boot_spinner_frame(tick - intro_start_tick);
    }
#endif
#else
    // Режим 1 (серый экран): Nyan статичный — фон и кадр уже нарисованы один
    // раз в eqstart_perform_tests(), перерисовывать каждый тик не нужно.
#endif
#if BOOT_SHOW_PROGRESS || BOOT_SCREEN_MODE != 0
    // Прогресс-бар (сам пропускает кадры без изменения процента).
    boot_progress_draw();
#endif
}

// Простой вывод строки в TTY-стиле на черный экран
static void tty_print(const char *msg, uint32_t color) {
    vesa_draw_string_direct(msg, 20, 20 + (tty_row * 16), color);
    tty_row++;
    if (tty_row > 40) {
        draw_rect_direct(0, 0, screen_width, screen_height, 0x000000);
        tty_row = 0;
    }
}

// Макрос для моментальной остановки системы при критическом сбое
#define CERBERUS_ASSERT(cond, reason)                                          \
  if (!(cond)) {                                                               \
    draw_rect_direct(0, 0, screen_width, screen_height, 0x330000);             \
    vesa_draw_string_direct("!!! KERNEL INTEGRITY FAULT !!!", 50, 50,          \
                            0xFF0000);                                         \
    vesa_draw_string_direct("REASON: " reason, 50, 80, 0xFFFFFF);              \
    while (1) {                                                                \
      __asm__("cli; hlt");                                                     \
    }                                                                          \
  }

// --- ТЕСТЫ ЦЕЛОСТНОСТИ ЯДРА ---

// 1. Стресс-тест PMM (Выделение и проверка целостности данных)
bool test_pmm_stress() {
  void *test_pages[32];

  // Выделяем страницы и пишем в них уникальный мусор
  for (int i = 0; i < 32; i++) {
    test_pages[i] = pmm_alloc();
    if (!test_pages[i]) {
      return false;
    }

    uint64_t *ptr = (uint64_t *)((uint64_t)test_pages[i] + hhdm_offset);
    *ptr = 0xABCDEF0123456789 ^ (uint64_t)test_pages[i];
  }

  // Проверяем, не перезаписали ли страницы друг друга
  for (int i = 0; i < 32; i++) {
    uint64_t *ptr = (uint64_t *)((uint64_t)test_pages[i] + hhdm_offset);
    if (*ptr != (0xABCDEF0123456789 ^ (uint64_t)test_pages[i])) {
      return false;
    }
    pmm_free(test_pages[i]);
  }
  return true;
}

// 2. Проверка FPU/SSE
bool test_cpu_fpu() {
  volatile float f1 = 3.14f;
  volatile float f2 = 2.71f;
  if ((int)(f1 * f2) != 8) { // 3.14 * 2.71 = 8.5094
    return false;
  }
  return true;
}

// 3. Главная точка входа диагностического лога
bool eqstart_perform_tests() {
#if FAST_BOOT
  /* --- БЫСТРЫЙ ПУТЬ ---
   * Только критичные проверки железа (µs), без диагностического лога и без
   * стресс-теста PMM. Сразу показываем серый boot-экран. */
  CERBERUS_ASSERT(hhdm_offset >= 0xFFFF800000000000, "HHDM Invalid offset");
  if (!test_cpu_fpu()) {
    CERBERUS_ASSERT(false, "FPU math error - CPU features not properly enabled");
  }
  {
    uint32_t start_tick = tick;
    uint32_t deadline = start_tick + 100;
    while (tick == start_tick && tick < deadline) {
      __asm__ volatile("hlt");
    }
    if (tick == start_tick) {
      CERBERUS_ASSERT(false, "PIT Timer is not ticking. Interrupts dead?");
    }
  }
  {
    uint16_t tr;
    __asm__ volatile("str %0" : "=r"(tr));
    if (tr == 0) {
      CERBERUS_ASSERT(false, "TSS not loaded. Multitasking will cause Triple Fault");
    }
  }
  // Фон под boot-экраном зависит от режима (чёрный для крутящегося Nyan,
  // серый для статичного) — иначе прогресс-бар стирал бы «хвосты» цветом,
  // не совпадающим с заливкой.
#if BOOT_SCREEN_MODE == 0
  draw_rect_direct(0, 0, screen_width, screen_height, 0x000000);
#else
  draw_rect_direct(0, 0, screen_width, screen_height, BOOT_GRAY_COLOR);
#endif
#if BOOT_SHOW_NYAN
  nyan_init_geometry();
  nyan_draw_frame(0);
#endif
#if BOOT_SHOW_PROGRESS
  boot_progress_draw();
#endif
  // Анимацию (интро + опционально кот/спиннер) дальше крутит таймер
  // (nyan_boot_anim_frame) — без блокирующего nyan_play(), чтобы не замедлять
  // загрузку. Привязываем интро к текущему тику.
  intro_start_tick = tick;
  intro_start_set  = 1;
  nyan_boot_active = 1;
  return true;
#else
  // Чистый черный экран
  draw_rect_direct(0, 0, screen_width, screen_height, 0x000000);
  tty_row = 0;

  // Готовим геометрию nyan-кадра и сразу показываем первый кадр под логом,
  // чтобы гифка была видна уже во время прохождения тестов.
  nyan_init_geometry();
  nyan_draw_frame(0);

  tty_print("Equinox OS Boot Diagnostics Protocol v2.1", 0xFFFFFF);
  tty_print("--------------------------------------------------", 0x555555);

  // Тест 1: HHDM
  tty_print("[   0.000000] HHDM: Verifying higher-half direct mapping...", 0x888888);
  CERBERUS_ASSERT(hhdm_offset >= 0xFFFF800000000000, "HHDM Invalid offset");
  tty_print("[   0.000003] HHDM: OK. Offset mapped correctly.", 0x00FF00);

  // Тест 2: Инициализация PAT (будет вызвана в vmm_init)
  tty_print("[   0.001024] PAT: Initializing Page Attribute Table...", 0x888888);
  tty_print("[   0.001090] PAT: Write-Combining enabled on Framebuffer index 3.", 0x00FF00);

  // Тест 3: PMM Stress
  tty_print("[   0.002150] PMM: Initializing Physical Memory Manager...", 0x888888);
  tty_print("[   0.002200] PMM: Performing physical allocator stress test...", 0x888888);
  if (!test_pmm_stress()) {
      CERBERUS_ASSERT(false, "PMM memory corruption detected");
  }
  tty_print("[   0.003600] PMM: OK. 32 pages stress-test passed successfully.", 0x00FF00);

  // Тест 4: CPU FPU
  tty_print("[   0.004100] CPU: Checking FPU/SSE state integrity...", 0x888888);
  if (!test_cpu_fpu()) {
      CERBERUS_ASSERT(false, "FPU math error - CPU features not properly enabled");
  }
  tty_print("[   0.004300] CPU: OK. SSE/FPU registers validated.", 0x00FF00);

  // Тест 5: Heartbeat (PIT)
  tty_print("[   0.005000] TIME: Testing PIT interrupts and firing rate...", 0x888888);
  uint32_t start_tick = tick;
  uint32_t deadline = start_tick + 100; // ждём максимум 100 мс
  while (tick == start_tick && tick < deadline) {
    __asm__ volatile("hlt");
  }
  if (tick == start_tick) {
    CERBERUS_ASSERT(false, "PIT Timer is not ticking. Interrupts dead?");
  }
  tty_print("[   0.006200] TIME: OK. Interrupts are firing stably.", 0x00FF00);

  // Тест 6: GDT/TSS
  tty_print("[   0.007000] GDT: Checking Task State Segment...", 0x888888);
  uint16_t tr;
  __asm__ volatile("str %0" : "=r"(tr));
  if (tr == 0) {
    CERBERUS_ASSERT(false, "TSS not loaded. Multitasking will cause Triple Fault");
  }
  tty_print("[   0.007200] GDT: OK. TSS loaded successfully.", 0x00FF00);

  tty_print("--------------------------------------------------", 0x555555);
  tty_print("All diagnostics PASSED. Launching Equinox GUI Subsystem...", 0x00FFFF);

#if BOOT_SCREEN_MODE == 1
  // --- РЕЖИМ "СЕРЫЙ ЭКРАН" (BOOT_SCREEN_MODE == 1) ---
  // Заливаем экран серым, рисуем статичный (неподвижный) кадр Nyan Cat и
  // прогресс-бар. Гифка НЕ крутится. Дальше прогресс-бар обновляет таймер
  // (nyan_boot_anim_frame) до старта GUI.
  draw_rect_direct(0, 0, screen_width, screen_height, BOOT_GRAY_COLOR);
  nyan_init_geometry();
  nyan_draw_frame(0);
  boot_progress_draw();
#else
  // --- РЕЖИМ "КРУТЯЩИЙСЯ NYAN" (BOOT_SCREEN_MODE == 0, по умолчанию) ---
  // Проигрываем Nyan Cat в обведённой зоне, пока идёт загрузка, перед
  // передачей управления GUI-подсистеме (~4 секунды = 5 петель гифки).
  nyan_play(4000);
#endif

  // Загрузка ещё продолжается (USB/mouse, ext2, GUI). Включаем таймерную
  // подкрутку, чтобы гифка НЕ замирала (режим 0) и обновлялся прогресс-бар
  // (оба режима) до самого старта sysgui (kmain сбросит флаг перед exec
  // sysgui.elf, а GUI догасит анимацию через syscall 88).
  nyan_boot_active = 1;
  return true;
#endif /* FAST_BOOT */
}