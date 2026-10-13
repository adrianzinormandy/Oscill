#include "scope_view.h"
#include "dma2d_graphics.h"
#include <string.h>

/* Software rotation only: physical LTDC remains RGB565, 240 x 320.
 * Logical 320 x 240 becomes upright when the board is turned 90 degrees CCW.
 * RM0090 16.7: preserve layer address, pitch, format and all timing registers. */
#define SV_WIDTH_PIXELS 320U
#define SV_HEIGHT_PIXELS 240U
#define SV_LEFT_PIXELS 8U
#define SV_RIGHT_PIXELS 311U
#define SV_TOP_PIXELS 26U
#define SV_BOTTOM_PIXELS 218U
#define SV_COLUMNS_PIXELS (SV_RIGHT_PIXELS-SV_LEFT_PIXELS+1U)
#define SV_ADC_FULL_SCALE_CODE 4095U
#define SV_GRAY_RGB565 0x4208U
#define SV_GREEN_RGB565 0x07E0U
#define SV_WHITE_RGB565 0xFFFFU
#define SV_AMBER_RGB565 0xFD20U
#define SV_Q16_ONE 65536U
#define SV_OPT __attribute__((optimize("O2")))
_Static_assert(SV_WINDOW_SAMPLES < AC_BLOCK_SAMPLES, "Window must fit SRAM snapshot");
_Static_assert(SV_PRETRIGGER_SAMPLES < SV_WINDOW_INTERVALS_SAMPLES, "Invalid pretrigger");
volatile SV_Diagnostic g_scope_diagnostic;
/* Frozen columns belong to main, and survive later ADC snapshots during R2M. */
static uint16_t column_min[SV_COLUMNS_PIXELS], column_max[SV_COLUMNS_PIXELS];
static uint32_t next_frame_tick_ms, last_match_tick_ms;
static uint32_t have_trace, match_seen, display_dirty;
static uint32_t button_raw, button_stable, button_change_tick_ms;

static SV_OPT uint32_t framebuffer_index(uint32_t x, uint32_t y)
{
    return x * LCD_WIDTH_PIXELS + (LCD_WIDTH_PIXELS - 1U - y);
}
static SV_OPT void pixel(uint32_t x, uint32_t y, uint16_t color)
{
    if (x < SV_WIDTH_PIXELS && y < SV_HEIGHT_PIXELS)
        ((volatile uint16_t *)LCD_FRAMEBUFFER_ADDRESS)[framebuffer_index(x, y)] = color;
}
static SV_OPT uint32_t ordinate(uint32_t code)
{
    return SV_BOTTOM_PIXELS - code * (SV_BOTTOM_PIXELS - SV_TOP_PIXELS) / SV_ADC_FULL_SCALE_CODE;
}

/* Search only the coherent, main-owned snapshot. No work in capture ISR.
 * Schmitt qualification rejects excursions that never reach level + hysteresis.
 * The anchor is the mid-level crossing, not the later qualification sample.
 * Q16 interpolation reduces display phase quantisation; it adds no ADC samples. */
static SV_OPT uint32_t find_rising(const uint16_t *samples, uint32_t count, uint32_t *crossing_q16)
{
    uint32_t armed = 0U, candidate = 0U, pending = 0U;
    const uint32_t before = SV_PRETRIGGER_SAMPLES * SV_Q16_ONE;
    const uint32_t span = SV_WINDOW_INTERVALS_SAMPLES * SV_Q16_ONE;
    for (uint32_t i = 1U; i < count; ++i) {
        uint32_t a = samples[i - 1U], b = samples[i];
        if (a <= SV_TRIGGER_LEVEL_CODE - SV_TRIGGER_HYSTERESIS_CODE)
            armed = 1U;
        if (armed && a < SV_TRIGGER_LEVEL_CODE && b >= SV_TRIGGER_LEVEL_CODE) {
            candidate = (i - 1U) * SV_Q16_ONE +
                (SV_TRIGGER_LEVEL_CODE - a) * SV_Q16_ONE / (b - a);
            pending = 1U;
        }
        if (pending && b < SV_TRIGGER_LEVEL_CODE) pending = 0U;
        if (pending && b >= SV_TRIGGER_LEVEL_CODE + SV_TRIGGER_HYSTERESIS_CODE) {
            armed = pending = 0U;
            if (candidate >= before && candidate - before + span <= (count - 1U) * SV_Q16_ONE) {
                *crossing_q16 = candidate;
                return 1U;
            }
        }
    }
    return 0U;
}
static SV_OPT uint32_t interpolate(const uint16_t *samples, uint32_t position_q16)
{
    uint32_t i = position_q16 / SV_Q16_ONE, fraction = position_q16 % SV_Q16_ONE;
    if (!fraction) return samples[i];
    return ((uint32_t)samples[i] * (SV_Q16_ONE - fraction) +
        (uint32_t)samples[i + 1U] * fraction + SV_Q16_ONE / 2U) / SV_Q16_ONE;
}
static SV_OPT void prepare_columns(uint32_t start_q16)
{
    const uint32_t span = SV_WINDOW_INTERVALS_SAMPLES * SV_Q16_ONE;
    for (uint32_t x = 0U; x < SV_COLUMNS_PIXELS; ++x) {
        uint32_t q0 = start_q16 + x * span / (SV_COLUMNS_PIXELS - 1U);
        uint32_t q1 = x + 1U < SV_COLUMNS_PIXELS ?
            start_q16 + (x + 1U) * span / (SV_COLUMNS_PIXELS - 1U) : q0;
        uint32_t a = interpolate(adc_snapshot, q0), b = interpolate(adc_snapshot, q1);
        uint32_t lo = a < b ? a : b, hi = a > b ? a : b;
        /* Retain true sample extrema if a column crosses a sample boundary. */
        for (uint32_t i = (q0 + SV_Q16_ONE - 1U) / SV_Q16_ONE; i <= q1 / SV_Q16_ONE; ++i) {
            uint32_t v = adc_snapshot[i];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        column_min[x] = (uint16_t)lo;
        column_max[x] = (uint16_t)hi;
    }
}
/* UM1670 Rev2 6.4 and MB1075B sheet 6: B1/USER is active high on PA0.
 * GPIO already configured by CubeMX. RM0090 8.4.5: poll IDR; no new EXTI ISR.
 * Require a stable release before another press; holding B1 never repeats. */
static uint32_t debounce_press(uint32_t raw, uint32_t now)
{
    if (raw != button_raw) {
        button_raw = raw;
        button_change_tick_ms = now;
    }
    if (raw != button_stable && now - button_change_tick_ms >= SV_BUTTON_DEBOUNCE_TICK_MS) {
        button_stable = raw;
        return raw;
    }
    return 0U;
}
static void poll_button(uint32_t now)
{
    if (debounce_press((B1_GPIO_Port->IDR & B1_Pin) != 0U, now)) {
        g_scope_diagnostic.trigger_mode ^= 1U;
        g_scope_diagnostic.button_presses++;
        match_seen = have_trace = 0U;
        g_scope_diagnostic.trigger_waiting = g_scope_diagnostic.trigger_mode;
        display_dirty = 1U;
        next_frame_tick_ms = now;
    }
}

/* Small fixed font, only glyphs used by this demo; no heap or UI library. */
static const struct { char character; uint8_t columns[5]; } font[] = {
    {'0',{0x3E,0x51,0x49,0x45,0x3E}}, {'1',{0,0x42,0x7F,0x40,0}},
    {'2',{0x42,0x61,0x51,0x49,0x46}}, {'A',{0x7E,0x11,0x11,0x11,0x7E}},
    {'B',{0x7F,0x49,0x49,0x49,0x36}}, {'D',{0x7F,0x41,0x41,0x22,0x1C}},
    {'E',{0x7F,0x49,0x49,0x49,0x41}},
    {'F',{0x7F,0x09,0x09,0x09,0x01}}, {'G',{0x3E,0x41,0x49,0x49,0x7A}},
    {'I',{0,0x41,0x7F,0x41,0}}, {'M',{0x7F,0x02,0x0C,0x02,0x7F}},
    {'O',{0x3E,0x41,0x41,0x41,0x3E}}, {'R',{0x7F,0x09,0x19,0x29,0x46}},
    {'S',{0x46,0x49,0x49,0x49,0x31}}, {'T',{0x01,0x01,0x7F,0x01,0x01}},
    {'U',{0x3F,0x40,0x40,0x40,0x3F}}, {'V',{0x1F,0x20,0x40,0x20,0x1F}},
    {'W',{0x3F,0x40,0x38,0x40,0x3F}}, {'/',{0x20,0x10,0x08,0x04,0x02}},
    {':',{0,0x36,0x36,0,0}}
};
static SV_OPT void text(uint32_t x, uint32_t y, const char *s, uint16_t color, uint32_t scale)
{
    while (*s) {
        for (uint32_t k = 0U; k < sizeof(font) / sizeof(font[0]); ++k) {
            if (font[k].character != *s) continue;
            for (uint32_t cx = 0U; cx < 5U; ++cx)
                for (uint32_t cy = 0U; cy < 7U; ++cy)
                    if (font[k].columns[cx] & (1U << cy))
                        for (uint32_t dx = 0U; dx < scale; ++dx)
                            for (uint32_t dy = 0U; dy < scale; ++dy)
                                pixel(x + cx * scale + dx, y + cy * scale + dy, color);
            break;
        }
        x += 6U * scale;
        ++s;
    }
}
static SV_OPT void render(void)
{
    uint32_t begin = DWT->CYCCNT;
    for (uint32_t i = 0U; i <= 10U; ++i) {
        uint32_t x = SV_LEFT_PIXELS + i * (SV_RIGHT_PIXELS - SV_LEFT_PIXELS) / 10U;
        for (uint32_t y = SV_TOP_PIXELS; y <= SV_BOTTOM_PIXELS; ++y) pixel(x, y, SV_GRAY_RGB565);
    }
    for (uint32_t i = 0U; i <= 8U; ++i) {
        uint32_t y = SV_TOP_PIXELS + i * (SV_BOTTOM_PIXELS - SV_TOP_PIXELS) / 8U;
        for (uint32_t x = SV_LEFT_PIXELS; x <= SV_RIGHT_PIXELS; ++x) pixel(x, y, SV_GRAY_RGB565);
    }
    if (have_trace) {
        uint32_t previous = ordinate((column_min[0] + column_max[0]) / 2U);
        for (uint32_t x = 0U; x < SV_COLUMNS_PIXELS; ++x) {
            uint32_t top = ordinate(column_max[x]), bottom = ordinate(column_min[x]);
            for (uint32_t y = top; y <= bottom; ++y) pixel(SV_LEFT_PIXELS + x, y, SV_GREEN_RGB565);
            uint32_t centre = ordinate((column_min[x] + column_max[x]) / 2U);
            if (x) {
                uint32_t lo = centre < previous ? centre : previous, hi = centre > previous ? centre : previous;
                for (uint32_t y = lo; y <= hi; ++y) pixel(SV_LEFT_PIXELS + x, y, SV_GREEN_RGB565);
            }
            previous = centre;
        }
    }
    text(8U, 5U, "20US/DIV", SV_WHITE_RGB565, 2U);
    text(188U, 5U, g_scope_diagnostic.rendered_trigger_mode ? "TRIG RISE" : "TRIG OFF", SV_WHITE_RGB565, 2U);
    text(8U, 228U, "B1:TRIG", SV_WHITE_RGB565, 1U);
    if (g_scope_diagnostic.rendered_trigger_mode) {
        /* Level / time marker: crossing sits at 20% of the window, two divisions. */
        uint32_t tx = SV_LEFT_PIXELS + SV_PRETRIGGER_SAMPLES *
            (SV_RIGHT_PIXELS - SV_LEFT_PIXELS) / SV_WINDOW_INTERVALS_SAMPLES;
        for (uint32_t d = 0U; d < 4U; ++d) pixel(tx, SV_TOP_PIXELS - 1U - d, SV_AMBER_RGB565);
        for (uint32_t d = 0U; d < 4U; ++d) pixel(SV_LEFT_PIXELS - 1U - d, ordinate(SV_TRIGGER_LEVEL_CODE), SV_AMBER_RGB565);
        text(258U, 228U, g_scope_diagnostic.rendered_trigger_waiting ? "WAIT" : "RISE", SV_AMBER_RGB565, 1U);
    }
    __DSB();
    uint32_t cycles = DWT->CYCCNT - begin;
    if (cycles > g_scope_diagnostic.render_max_cycles) g_scope_diagnostic.render_max_cycles = cycles;
    g_scope_diagnostic.frames++;
    g_adc_diagnostic.display_frames++;
    if (have_trace && g_scope_diagnostic.rendered_trigger_mode && !g_scope_diagnostic.rendered_trigger_waiting)
        g_scope_diagnostic.triggered_frames++;
}
static void check(uint32_t condition)
{
    g_scope_diagnostic.algorithm_checks++;
    if (!condition) g_scope_diagnostic.algorithm_failures++;
}
static void algorithm_self_test(void)
{
    /* Executed once before capture; verifies the actual compiled C on MCU.
     * These synthetic checks do not certify physical signal/trigger quality. */
    uint16_t samples[128];
    uint32_t q = 0U;
    for (uint32_t i = 0U; i < 128U; ++i) samples[i] = 1000U;
    check(!find_rising(samples, 128U, &q));
    for (uint32_t i = 40U; i < 128U; ++i) samples[i] = 3096U;
    check(find_rising(samples, 128U, &q) && q == 39U * SV_Q16_ONE + SV_Q16_ONE / 2U);
    check(interpolate(samples, q) == SV_TRIGGER_LEVEL_CODE);
    for (uint32_t i = 0U; i < 128U; ++i) samples[i] = i < 40U ? 3096U : 1000U;
    check(!find_rising(samples, 128U, &q)); /* falling only */
    for (uint32_t i = 0U; i < 128U; ++i) samples[i] = i < 5U ? 1000U : 3096U;
    check(!find_rising(samples, 128U, &q)); /* insufficient pretrigger */
    for (uint32_t i = 0U; i < 128U; ++i) samples[i] = i < 90U ? 1000U : 3096U;
    check(!find_rising(samples, 128U, &q)); /* insufficient posttrigger */
    for (uint32_t i = 0U; i < 128U; ++i) samples[i] = i & 1U ? 2060U : 2036U;
    check(!find_rising(samples, 128U, &q)); /* noise inside hysteresis */
    check(framebuffer_index(0U, 0U) == 239U && framebuffer_index(319U, 239U) == 76560U);
    check(framebuffer_index(0U, 239U) == 0U && framebuffer_index(319U, 0U) == 76799U);
    button_raw = button_stable = button_change_tick_ms = 0U;
    check(!debounce_press(1U, 1U) && !debounce_press(0U, 8U) && !debounce_press(1U, 10U));
    check(!debounce_press(1U, 39U) && debounce_press(1U, 40U));
    check(!debounce_press(1U, 1000U)); /* no repeat while held */
    check(!debounce_press(0U, 1001U) && !debounce_press(0U, 1031U));
    check(!debounce_press(1U, 1032U) && debounce_press(1U, 1062U));
}
void SV_Init(void)
{
    memset((void *)&g_scope_diagnostic, 0, sizeof(g_scope_diagnostic));
    algorithm_self_test();
    if (g_scope_diagnostic.algorithm_failures) { AC_Fail(AC_CONFIG); return; }
    g_scope_diagnostic.frame_period_tick_ms = SV_FRAME_PERIOD_TICK_MS;
    g_scope_diagnostic.window_samples = SV_WINDOW_SAMPLES;
    g_scope_diagnostic.window_intervals_samples = SV_WINDOW_INTERVALS_SAMPLES;
    g_scope_diagnostic.logical_width_pixels = SV_WIDTH_PIXELS;
    g_scope_diagnostic.logical_height_pixels = SV_HEIGHT_PIXELS;
    g_scope_diagnostic.time_div_target_us = SV_TIME_DIV_US;
    g_scope_diagnostic.trigger_mode = SV_TRIGGER_OFF;
    g_scope_diagnostic.trigger_level_code = SV_TRIGGER_LEVEL_CODE;
    g_scope_diagnostic.trigger_hysteresis_code = SV_TRIGGER_HYSTERESIS_CODE;
    g_scope_diagnostic.trigger_waiting = 0U;
    next_frame_tick_ms = last_match_tick_ms = HAL_GetTick();
    button_change_tick_ms = next_frame_tick_ms;
    button_raw = button_stable = (B1_GPIO_Port->IDR & B1_Pin) != 0U;
    have_trace = match_seen = 0U;
    display_dirty = 1U;
}
uint32_t SV_IsIdle(void) { return !g_scope_diagnostic.clear_pending; }
SV_OPT void SV_Process(void)
{
    uint32_t now = HAL_GetTick();
    poll_button(now);
    if (g_scope_diagnostic.clear_pending) {
        if (g_dma2d_diagnostic.state == LCD_STATE_FAIL || now - g_scope_diagnostic.clear_start_tick_ms > 100U)
            { AC_Fail(AC_GRAPHICS); return; }
        /* RM0090 11.3.11/11.4: no CPU framebuffer writes while R2M runs. */
        if (g_dma2d_diagnostic.async_inflight) return;
        if (DG_Wait(100U) != DG_OK) { AC_Fail(AC_GRAPHICS); return; }
        g_scope_diagnostic.clear_pending = 0U;
        render();
        return;
    }
    uint32_t waiting = g_scope_diagnostic.trigger_mode &&
        (!match_seen || now - last_match_tick_ms >= SV_TRIGGER_WAIT_TICK_MS);
    if (waiting != g_scope_diagnostic.trigger_waiting) {
        g_scope_diagnostic.trigger_waiting = waiting;
        display_dirty = 1U;
    }
    if ((int32_t)(now - next_frame_tick_ms) < 0 ||
        g_adc_diagnostic.snapshot_samples != AC_BLOCK_SAMPLES ||
        (!display_dirty && g_scope_diagnostic.frame_generation == g_adc_diagnostic.snapshot_generation)) return;
    g_scope_diagnostic.frame_generation = g_adc_diagnostic.snapshot_generation;
    if (g_adc_diagnostic.sampling_rate_hz)
        g_scope_diagnostic.time_div_estimate_ns = (uint32_t)((uint64_t)SV_WINDOW_INTERVALS_SAMPLES *
            1000000000ULL / (10U * g_adc_diagnostic.sampling_rate_hz));
    uint32_t crossing = 0U;
    if (!g_scope_diagnostic.trigger_mode) {
        prepare_columns((AC_BLOCK_SAMPLES - SV_WINDOW_SAMPLES) * SV_Q16_ONE);
        have_trace = display_dirty = 1U;
    } else if (find_rising(adc_snapshot, AC_BLOCK_SAMPLES, &crossing)) {
        prepare_columns(crossing - SV_PRETRIGGER_SAMPLES * SV_Q16_ONE);
        g_scope_diagnostic.trigger_q16 = crossing;
        g_scope_diagnostic.trigger_matches++;
        g_scope_diagnostic.trigger_waiting = 0U;
        have_trace = match_seen = display_dirty = 1U;
        last_match_tick_ms = now;
    } else {
        g_scope_diagnostic.trigger_misses++;
        /* Hold last trace: do not silently replace TRIG mode with free-running. */
    }
    next_frame_tick_ms = now + SV_FRAME_PERIOD_TICK_MS;
    if (!display_dirty) return;
    g_scope_diagnostic.rendered_trigger_mode = g_scope_diagnostic.trigger_mode;
    g_scope_diagnostic.rendered_trigger_waiting = g_scope_diagnostic.trigger_waiting;
    DG_Job fill = { .operation = DG_FILL, .dst = &dg_framebuffer,
        .width = LCD_WIDTH_PIXELS, .height = LCD_HEIGHT_PIXELS, .color = 0U };
    if (DG_Start(&fill, 1U) != DG_OK) { AC_Fail(AC_GRAPHICS); return; }
    display_dirty = 0U;
    g_scope_diagnostic.clear_pending = 1U;
    g_scope_diagnostic.clear_start_tick_ms = now;
}
