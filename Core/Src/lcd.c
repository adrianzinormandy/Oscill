/* ILI9341 command table derived from STMicroelectronics ili9341.c V1.0.2,
 * Copyright (c) 2014 STMicroelectronics. BSD-3-Clause; see LCD_ST_LICENSE.txt.
 * Only control commands are sent over SPI. Pixels use LTDC from SDRAM.
 */
#include "lcd.h"
#include "main.h"

#define LCD_SPI_TIMEOUT_MS          50U
#define LCD_DELAY_LIMIT_POLLS       50000000UL
#define LCD_IMAGE_HOLD_MS           2000U
#define LCD_STATIC_HOLD_MS          4000U
#define LCD_ANIMATION_HOLD_MS       10000U
#define LCD_ANIMATION_PERIOD_MS     50U
#define LCD_STABILITY_TARGET_MS     62000U
#define LCD_SDRAM_TEST_PASSES        3U
#define LCD_RECT_SIZE_PIXELS        16U
#define LCD_RECT_Y_PIXELS           152U

#define RGB565_BLACK   0x0000U
#define RGB565_WHITE   0xFFFFU
#define RGB565_RED     0xF800U
#define RGB565_GREEN   0x07E0U
#define RGB565_BLUE    0x001FU
#define RGB565_YELLOW  0xFFE0U
#define RGB565_CYAN    0x07FFU
#define RGB565_MAGENTA 0xF81FU

volatile LCD_Diagnostic g_lcd_diagnostic
  __attribute__((section(".bss.lcd_diagnostic"), aligned(4)));

/* ILI9341 init commands, data and the two delays are generated verbatim from
 * installed CubeF4 V1.28.3 Drivers/BSP/Components/ili9341/ili9341.c/.h.
 * No software reset or COLMOD command is added: board NRST resets the LCD;
 * ST's board sequence selects RGB via B0/B6/F6, using panel reset defaults.
 */
typedef struct {
  uint8_t command, count;
  uint16_t delay_ms;
  uint8_t data[15];
} ILI9341_Command;
static const ILI9341_Command ili9341_commands[] = {
  {0xCAU, 3U, 0U, {0xC3U, 0x08U, 0x50U}},
  {0xCFU, 3U, 0U, {0x00U, 0xC1U, 0x30U}},
  {0xEDU, 4U, 0U, {0x64U, 0x03U, 0x12U, 0x81U}},
  {0xE8U, 3U, 0U, {0x85U, 0x00U, 0x78U}},
  {0xCBU, 5U, 0U, {0x39U, 0x2CU, 0x00U, 0x34U, 0x02U}},
  {0xF7U, 1U, 0U, {0x20U}},
  {0xEAU, 2U, 0U, {0x00U, 0x00U}},
  {0xB1U, 2U, 0U, {0x00U, 0x1BU}},
  {0xB6U, 2U, 0U, {0x0AU, 0xA2U}},
  {0xC0U, 1U, 0U, {0x10U}},
  {0xC1U, 1U, 0U, {0x10U}},
  {0xC5U, 2U, 0U, {0x45U, 0x15U}},
  {0xC7U, 1U, 0U, {0x90U}},
  {0x36U, 1U, 0U, {0xC8U}},
  {0xF2U, 1U, 0U, {0x00U}},
  {0xB0U, 1U, 0U, {0xC2U}},
  {0xB6U, 4U, 0U, {0x0AU, 0xA7U, 0x27U, 0x04U}},
  {0x2AU, 4U, 0U, {0x00U, 0x00U, 0x00U, 0xEFU}},
  {0x2BU, 4U, 0U, {0x00U, 0x00U, 0x01U, 0x3FU}},
  {0xF6U, 3U, 0U, {0x01U, 0x00U, 0x06U}},
  {0x2CU, 0U, 200U, {0U}},
  {0x26U, 1U, 0U, {0x01U}},
  {0xE0U, 15U, 0U, {0x0FU, 0x29U, 0x24U, 0x0CU, 0x0EU, 0x09U, 0x4EU, 0x78U, 0x3CU, 0x09U, 0x13U, 0x05U, 0x17U, 0x11U, 0x00U}},
  {0xE1U, 15U, 0U, {0x00U, 0x16U, 0x1BU, 0x04U, 0x11U, 0x07U, 0x31U, 0x33U, 0x42U, 0x05U, 0x0CU, 0x0AU, 0x28U, 0x2FU, 0x0FU}},
  {0x11U, 0U, 200U, {0U}},
  {0x29U, 0U, 0U, {0U}},
  {0x2CU, 0U, 0U, {0U}},
};

static uint32_t image_start_tick_ms;
static uint32_t animation_last_tick_ms;
static uint32_t animation_x_pixels;

_Static_assert(LCD_FRAMEBUFFER_BYTES == 153600U, "Framebuffer size");
_Static_assert(LCD_FRAMEBUFFER_ADDRESS + LCD_FRAMEBUFFER_BYTES
               <= SDRAM_BASE_ADDRESS + SDRAM_SIZE_BYTES, "Framebuffer in SDRAM");
_Static_assert(LCD_SDRAM_TEST_ADDRESS >= LCD_FRAMEBUFFER_ADDRESS + LCD_FRAMEBUFFER_BYTES,
               "Concurrent test must not overlap framebuffer");
_Static_assert(LCD_SDRAM_TEST_ADDRESS + LCD_SDRAM_TEST_BYTES
               <= SDRAM_BASE_ADDRESS + SDRAM_SIZE_BYTES, "Test region in SDRAM");

static void snapshot_fmc(volatile SDRAM_RegisterSnapshot *s)
{
  /* RM0090 37.7.5: read-only configuration/status snapshots. */
  s->sdcr1 = FMC_Bank5_6->SDCR[0]; s->sdcr2 = FMC_Bank5_6->SDCR[1];
  s->sdtr1 = FMC_Bank5_6->SDTR[0]; s->sdtr2 = FMC_Bank5_6->SDTR[1];
  s->sdrtr = FMC_Bank5_6->SDRTR; s->sdsr = FMC_Bank5_6->SDSR;
}

static void snapshot_ltdc(volatile LCD_RegisterSnapshot *s)
{
  /* RM0090 16.7: APB register reads; safe for short IRQ snapshots, no FMC. */
  s->sscr = LTDC->SSCR; s->bpcr = LTDC->BPCR;
  s->awcr = LTDC->AWCR; s->twcr = LTDC->TWCR;
  s->gcr = LTDC->GCR; s->srcr = LTDC->SRCR;
  s->ier = LTDC->IER; s->isr = LTDC->ISR;
  s->cpsr = LTDC->CPSR; s->cdsr = LTDC->CDSR;
  s->l1cr = LTDC_Layer1->CR; s->l1whpcr = LTDC_Layer1->WHPCR;
  s->l1wvpcr = LTDC_Layer1->WVPCR; s->l1pfcr = LTDC_Layer1->PFCR;
  s->l1cacr = LTDC_Layer1->CACR; s->l1bfcr = LTDC_Layer1->BFCR;
  s->l1cfbar = LTDC_Layer1->CFBAR; s->l1cfblr = LTDC_Layer1->CFBLR;
  s->l1cfblnr = LTDC_Layer1->CFBLNR; s->l2cr = LTDC_Layer2->CR;
}

void LCD_Fail(LCD_Error error)
{
  if (error == LCD_ERROR_SPI_INIT) { g_lcd_diagnostic.spi_error_count++; }
  if (g_lcd_diagnostic.first_error == LCD_ERROR_NONE) {
    g_lcd_diagnostic.first_error = error;
    g_lcd_diagnostic.first_error_stage = g_lcd_diagnostic.stage;
  }
  g_lcd_diagnostic.last_error = error;
  g_lcd_diagnostic.state = LCD_STATE_FAIL;
}

static void pixel(uint32_t x, uint32_t y, uint16_t color)
{
  if (x >= LCD_WIDTH_PIXELS || y >= LCD_HEIGHT_PIXELS) {
    LCD_Fail(LCD_ERROR_PIXEL_BOUNDS); return;
  }
  volatile uint16_t * const fb = (volatile uint16_t *)(uintptr_t)LCD_FRAMEBUFFER_ADDRESS;
  g_lcd_diagnostic.current_address = LCD_FRAMEBUFFER_ADDRESS + 2U * (y * LCD_WIDTH_PIXELS + x);
  fb[y * LCD_WIDTH_PIXELS + x] = color;
}

static void fill(uint16_t color)
{
  volatile uint16_t * const fb = (volatile uint16_t *)(uintptr_t)LCD_FRAMEBUFFER_ADDRESS;
  for (uint32_t i = 0U; i < LCD_WIDTH_PIXELS * LCD_HEIGHT_PIXELS; ++i) {
    g_lcd_diagnostic.current_address = LCD_FRAMEBUFFER_ADDRESS + 2U * i;
    fb[i] = color;
  }
  __DSB();
}

HAL_StatusTypeDef LCD_PrepareFramebuffer(void)
{
  g_lcd_diagnostic = (LCD_Diagnostic){0};
  g_lcd_diagnostic.stage = LCD_STAGE_FRAMEBUFFER;
  g_lcd_diagnostic.state = LCD_STATE_RUNNING;
  g_lcd_diagnostic.framebuffer_addr = LCD_FRAMEBUFFER_ADDRESS;
  g_lcd_diagnostic.framebuffer_bytes = LCD_FRAMEBUFFER_BYTES;
  g_lcd_diagnostic.pixel_format = LTDC_PIXEL_FORMAT_RGB565;
  g_lcd_diagnostic.width = LCD_WIDTH_PIXELS;
  g_lcd_diagnostic.height = LCD_HEIGHT_PIXELS;
  if (g_sdram_diagnostic.init_complete == 0U) {
    LCD_Fail(LCD_ERROR_SDRAM_INIT); return HAL_ERROR;
  }
  snapshot_fmc(&g_lcd_diagnostic.fmc_before);
  fill(RGB565_BLACK); /* Complete all 153600 bytes before enabling LTDC/layer. */
  return HAL_OK;
}

static HAL_StatusTypeDef transmit_byte(SPI_HandleTypeDef *hspi, uint8_t value,
                                      uint32_t is_command)
{
  /* MB1075B SPI5 shared bus: gyro PC1 stays high; LCD PC2 active low.
   * RM0090 28.3.3/28.3.5. Same per-byte CS convention as ST LCD_IO_Write*.
   */
  HAL_GPIO_WritePin(NCS_MEMS_SPI_GPIO_Port, NCS_MEMS_SPI_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(WRX_DCX_GPIO_Port, WRX_DCX_Pin,
                    is_command ? GPIO_PIN_RESET : GPIO_PIN_SET);
  HAL_GPIO_WritePin(CSX_GPIO_Port, CSX_Pin, GPIO_PIN_RESET);
  const HAL_StatusTypeDef status = HAL_SPI_Transmit(hspi, &value, 1U, LCD_SPI_TIMEOUT_MS);
  HAL_GPIO_WritePin(CSX_GPIO_Port, CSX_Pin, GPIO_PIN_SET);
  g_lcd_diagnostic.spi_status = status;
  g_lcd_diagnostic.spi_hal_error = HAL_SPI_GetError(hspi);
  if (status != HAL_OK) {
    g_lcd_diagnostic.spi_error_count++;
    if (status == HAL_TIMEOUT) { g_lcd_diagnostic.spi_timeout_count++; }
    LCD_Fail(LCD_ERROR_SPI_TRANSFER); return status;
  }
  g_lcd_diagnostic.spi_byte_count++;
  if (is_command) {
    g_lcd_diagnostic.spi_command_count++;
    g_lcd_diagnostic.last_command = value;
  }
  return HAL_OK;
}

static HAL_StatusTypeDef panel_delay(uint32_t duration_ms)
{
  /* Existing HAL TIM6 base, no RCC changes. 3% compensates measured HSI error. */
  const uint32_t ticks_ms = duration_ms + (duration_ms * 3U + 99U) / 100U + 1U;
  const uint32_t start = HAL_GetTick();
  for (uint32_t poll = 0U; poll < LCD_DELAY_LIMIT_POLLS; ++poll) {
    if (HAL_GetTick() - start >= ticks_ms) { return HAL_OK; }
  }
  LCD_Fail(LCD_ERROR_DELAY_TIMEOUT); return HAL_TIMEOUT;
}

HAL_StatusTypeDef LCD_InitializeIli9341(SPI_HandleTypeDef *hspi)
{
  g_lcd_diagnostic.stage = LCD_STAGE_ILI9341;
  g_lcd_diagnostic.nominal_spi_clock_hz = HAL_RCC_GetPCLK2Freq() / 4U;
  if (hspi == NULL || hspi->Instance != SPI5
      || HAL_SPI_GetState(hspi) != HAL_SPI_STATE_READY) {
    LCD_Fail(LCD_ERROR_SPI_INIT); return HAL_ERROR;
  }
  for (uint32_t i = 0U; i < sizeof(ili9341_commands) / sizeof(ili9341_commands[0]); ++i) {
    const ILI9341_Command *cmd = &ili9341_commands[i];
    HAL_StatusTypeDef status = transmit_byte(hspi, cmd->command, 1U);
    if (status != HAL_OK) { g_lcd_diagnostic.ili9341_status = status; return status; }
    for (uint32_t j = 0U; j < cmd->count; ++j) {
      status = transmit_byte(hspi, cmd->data[j], 0U);
      if (status != HAL_OK) { g_lcd_diagnostic.ili9341_status = status; return status; }
    }
    if (cmd->delay_ms != 0U) {
      status = panel_delay(cmd->delay_ms);
      if (status != HAL_OK) { g_lcd_diagnostic.ili9341_status = status; return status; }
    }
    g_lcd_diagnostic.last_ili_step = i + 1U;
  }
  /* HAL_OK confirms transmissions only; panel acceptance requires observation. */
  g_lcd_diagnostic.ili9341_status = HAL_OK;
  return HAL_OK;
}

static uint32_t verify_configuration(void)
{
  snapshot_fmc(&g_lcd_diagnostic.fmc_after);
  snapshot_ltdc(&g_lcd_diagnostic.ltdc_current);
  const volatile SDRAM_RegisterSnapshot *a = &g_lcd_diagnostic.fmc_before;
  const volatile SDRAM_RegisterSnapshot *b = &g_lcd_diagnostic.fmc_after;
  g_lcd_diagnostic.fmc_unchanged = a->sdcr1 == b->sdcr1 && a->sdcr2 == b->sdcr2
    && a->sdtr1 == b->sdtr1 && a->sdtr2 == b->sdtr2 && a->sdrtr == b->sdrtr;
  if (!g_lcd_diagnostic.fmc_unchanged) { LCD_Fail(LCD_ERROR_FMC_CHANGED); return 0U; }
  const volatile LCD_RegisterSnapshot *s = &g_lcd_diagnostic.ltdc_current;
  if ((s->gcr & LTDC_GCR_LTDCEN) == 0U || (s->l1cr & LTDC_LxCR_LEN) == 0U
      || (s->l1cr & LTDC_LxCR_COLKEN) != 0U || (s->l2cr & LTDC_LxCR_LEN) != 0U
      || s->l1cfbar != LCD_FRAMEBUFFER_ADDRESS || s->l1pfcr != LTDC_PIXEL_FORMAT_RGB565
      || s->l1cfblr != ((LCD_WIDTH_PIXELS * 2U) << 16 | (LCD_WIDTH_PIXELS * 2U + 3U))
      || s->l1cfblnr != LCD_HEIGHT_PIXELS || s->l1cacr != 255U) {
    LCD_Fail(LCD_ERROR_LTDC_CONFIG); return 0U;
  }
  return 1U;
}

HAL_StatusTypeDef LCD_ConfirmStart(void)
{
  /* RM0090 6.3.24/25: existing LTDC MSP enables PLLSAI N192/R4/div8.
   * Shared input HSI/PLLM16, nominal pixel clock 6 MHz; no system PLL changes.
   */
  g_lcd_diagnostic.rcc_pllcfgr = RCC->PLLCFGR;
  g_lcd_diagnostic.rcc_cfgr = RCC->CFGR;
  g_lcd_diagnostic.rcc_pllsaicfgr = RCC->PLLSAICFGR;
  g_lcd_diagnostic.rcc_dckcfgr = RCC->DCKCFGR;
  g_lcd_diagnostic.nominal_pixel_clock_hz = 6000000U;
  if (!verify_configuration() || g_lcd_diagnostic.state == LCD_STATE_FAIL) { return HAL_ERROR; }
  g_lcd_diagnostic.stage = LCD_STAGE_IMAGES;
  g_lcd_diagnostic.image = LCD_IMAGE_BLACK;
  g_lcd_diagnostic.images_shown_mask = 1U << LCD_IMAGE_BLACK;
  g_lcd_diagnostic.start_tick_ms = HAL_GetTick();
  image_start_tick_ms = g_lcd_diagnostic.start_tick_ms;
  return HAL_OK;
}

static uint16_t checker_color(uint32_t x, uint32_t y)
{
  return ((x / 8U + y / 8U) & 1U) ? RGB565_WHITE : RGB565_BLACK;
}

static void draw_image(uint32_t image)
{
  static const uint16_t colors[] = {
    RGB565_BLACK, RGB565_WHITE, RGB565_RED, RGB565_GREEN, RGB565_BLUE
  };
  static const uint16_t bars[] = {
    RGB565_RED, RGB565_GREEN, RGB565_BLUE, RGB565_YELLOW, RGB565_CYAN, RGB565_MAGENTA
  };
  if (image <= LCD_IMAGE_BLUE) { fill(colors[image]); return; }
  for (uint32_t y = 0U; y < LCD_HEIGHT_PIXELS; ++y) {
    for (uint32_t x = 0U; x < LCD_WIDTH_PIXELS; ++x) {
      pixel(x, y, image == LCD_IMAGE_BARS ? bars[x / 40U] : checker_color(x, y));
    }
  }
  if (image == LCD_IMAGE_GEOMETRY) {
    fill(RGB565_BLACK);
    for (uint32_t i = 0U; i < 16U; ++i) {
      for (uint32_t j = 0U; j < 16U; ++j) {
        pixel(i, j, RGB565_RED); pixel(LCD_WIDTH_PIXELS - 1U - i, j, RGB565_GREEN);
        pixel(i, LCD_HEIGHT_PIXELS - 1U - j, RGB565_BLUE);
        pixel(LCD_WIDTH_PIXELS - 1U - i, LCD_HEIGHT_PIXELS - 1U - j, RGB565_YELLOW);
      }
    }
    for (uint32_t x = 0U; x < LCD_WIDTH_PIXELS; ++x) {
      pixel(x, 0U, RGB565_WHITE); pixel(x, LCD_HEIGHT_PIXELS - 1U, RGB565_WHITE);
      pixel(x, LCD_HEIGHT_PIXELS / 2U, RGB565_WHITE);
    }
    for (uint32_t y = 0U; y < LCD_HEIGHT_PIXELS; ++y) {
      pixel(0U, y, RGB565_WHITE); pixel(LCD_WIDTH_PIXELS - 1U, y, RGB565_WHITE);
      pixel(LCD_WIDTH_PIXELS / 2U, y, RGB565_WHITE);
      uint32_t x = y * (LCD_WIDTH_PIXELS - 1U) / (LCD_HEIGHT_PIXELS - 1U);
      pixel(x, y, RGB565_WHITE); pixel(LCD_WIDTH_PIXELS - 1U - x, y, RGB565_WHITE);
    }
  }
  __DSB();
}

static void animate(void)
{
  /* Restore previous 16x16 patch, then draw next; no LTDC reload or full fill. */
  for (uint32_t y = LCD_RECT_Y_PIXELS; y < LCD_RECT_Y_PIXELS + LCD_RECT_SIZE_PIXELS; ++y) {
    for (uint32_t x = animation_x_pixels; x < animation_x_pixels + LCD_RECT_SIZE_PIXELS; ++x) {
      pixel(x, y, checker_color(x, y));
    }
  }
  animation_x_pixels = (animation_x_pixels + 4U)
    % (LCD_WIDTH_PIXELS - LCD_RECT_SIZE_PIXELS + 1U);
  for (uint32_t y = LCD_RECT_Y_PIXELS; y < LCD_RECT_Y_PIXELS + LCD_RECT_SIZE_PIXELS; ++y) {
    for (uint32_t x = animation_x_pixels; x < animation_x_pixels + LCD_RECT_SIZE_PIXELS; ++x) {
      pixel(x, y, RGB565_MAGENTA);
    }
  }
  __DSB();
  g_lcd_diagnostic.animation_updates++;
}

static uint16_t concurrent_signature(uint32_t i, uint32_t pass)
{
  return (uint16_t)((i * 0x9E37U) ^ (i >> 7) ^ (0x1357U + pass * 0x1234U));
}

static uint32_t concurrent_test(void)
{
  g_lcd_diagnostic.stage = LCD_STAGE_SDRAM_CONCURRENT;
  volatile uint16_t * const region = (volatile uint16_t *)(uintptr_t)LCD_SDRAM_TEST_ADDRESS;
  for (uint32_t pass = 0U; pass < LCD_SDRAM_TEST_PASSES; ++pass) {
    for (uint32_t i = 0U; i < LCD_SDRAM_TEST_BYTES / 2U; ++i) {
      g_lcd_diagnostic.current_address = LCD_SDRAM_TEST_ADDRESS + 2U * i;
      region[i] = concurrent_signature(i, pass);
      g_lcd_diagnostic.sdram_test_writes++;
    }
    __DSB();
    for (uint32_t i = 0U; i < LCD_SDRAM_TEST_BYTES / 2U; ++i) {
      const uint16_t expected = concurrent_signature(i, pass);
      g_lcd_diagnostic.current_address = LCD_SDRAM_TEST_ADDRESS + 2U * i;
      const uint16_t actual = region[i];
      g_lcd_diagnostic.sdram_test_reads++;
      if (actual != expected) {
        if (g_lcd_diagnostic.sdram_error_count == 0U) {
          g_lcd_diagnostic.first_sdram_error_addr = LCD_SDRAM_TEST_ADDRESS + 2U * i;
          g_lcd_diagnostic.expected = expected; g_lcd_diagnostic.actual = actual;
        }
        g_lcd_diagnostic.sdram_error_count++;
      }
    }
    g_lcd_diagnostic.sdram_passes++;
    if (g_lcd_diagnostic.sdram_error_count != 0U) {
      LCD_Fail(LCD_ERROR_SDRAM_DATA); return 0U;
    }
    if (g_lcd_diagnostic.state == LCD_STATE_FAIL) { return 0U; }
  }
  return verify_configuration();
}

void LCD_Process(void)
{
  if (g_lcd_diagnostic.state == LCD_STATE_FAIL) { LCD_DiagnosticStop(); }
  const uint32_t now = HAL_GetTick();
  g_lcd_diagnostic.elapsed_tick_ms = now - g_lcd_diagnostic.start_tick_ms;
  const uint32_t image = g_lcd_diagnostic.image;
  if (image == LCD_IMAGE_ANIMATION && now - animation_last_tick_ms >= LCD_ANIMATION_PERIOD_MS) {
    animate(); animation_last_tick_ms = now;
  }
  const uint32_t hold_ms = image <= LCD_IMAGE_BLUE ? LCD_IMAGE_HOLD_MS
    : (image == LCD_IMAGE_ANIMATION ? LCD_ANIMATION_HOLD_MS : LCD_STATIC_HOLD_MS);
  if (now - image_start_tick_ms >= hold_ms) {
    const uint32_t next = (image + 1U) % LCD_IMAGE_COUNT;
    if (next == LCD_IMAGE_BLACK) { g_lcd_diagnostic.cycles_completed++; }
    draw_image(next);
    g_lcd_diagnostic.image = next;
    g_lcd_diagnostic.images_shown_mask |= 1U << next;
    image_start_tick_ms = HAL_GetTick();
    if (next == LCD_IMAGE_BARS && g_lcd_diagnostic.sdram_passes == 0U) {
      if (!concurrent_test()) { LCD_DiagnosticStop(); }
    }
    g_lcd_diagnostic.stage = next == LCD_IMAGE_ANIMATION ? LCD_STAGE_ANIMATION : LCD_STAGE_IMAGES;
    if (next == LCD_IMAGE_ANIMATION) {
      animation_x_pixels = 0U; animation_last_tick_ms = image_start_tick_ms;
    }
    if (!verify_configuration()) { LCD_DiagnosticStop(); }
  }
  if (g_lcd_diagnostic.elapsed_tick_ms >= LCD_STABILITY_TARGET_MS
      && g_lcd_diagnostic.images_shown_mask == (1U << LCD_IMAGE_COUNT) - 1U
      && g_lcd_diagnostic.sdram_passes == LCD_SDRAM_TEST_PASSES
      && g_lcd_diagnostic.state != LCD_STATE_FAIL) {
    /* Atomic promotion: an IRQ error between the check and store must not be
     * overwritten by PASS. Mask only these few SRAM operations, never drawing.
     * Digital criteria only; visual_state never promoted by software.
     */
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    if (g_lcd_diagnostic.state != LCD_STATE_FAIL
        && g_lcd_diagnostic.fifo_underrun_count == 0U
        && g_lcd_diagnostic.transfer_error_count == 0U
        && g_lcd_diagnostic.sdram_error_count == 0U) {
      g_lcd_diagnostic.state = LCD_STATE_PASS;
    }
    __set_PRIMASK(primask);
  }
}

void LCD_LTDC_IRQHandler(void)
{
  /* RM0090 16.5 and 16.7.8-10: flags are sticky events, not frame counters.
   * Preserve first error snapshot before W1C. Do not let HAL disable FU/TE
   * after one event. Only short APB/SRAM operations; no rendering/FMC waits.
   */
  const uint32_t isr = LTDC->ISR;
  const uint32_t ier = LTDC->IER;
  uint32_t clear = 0U;
  const uint32_t active = isr & ier & (LTDC_ISR_FUIF | LTDC_ISR_TERRIF);
  if (active == 0U) { return; }
  g_lcd_diagnostic.irq_last_isr = isr;
  g_lcd_diagnostic.irq_last_ier = ier;
  if (g_lcd_diagnostic.fifo_underrun_count == 0U && g_lcd_diagnostic.transfer_error_count == 0U) {
    snapshot_ltdc(&g_lcd_diagnostic.ltdc_first_error);
    g_lcd_diagnostic.first_error_isr = isr;
  }
  if ((active & LTDC_ISR_FUIF) != 0U) {
    g_lcd_diagnostic.fifo_underrun_count++;
    clear |= LTDC_ICR_CFUIF; LCD_Fail(LCD_ERROR_FIFO);
  }
  if ((active & LTDC_ISR_TERRIF) != 0U) {
    g_lcd_diagnostic.transfer_error_count++;
    clear |= LTDC_ICR_CTERRIF; LCD_Fail(LCD_ERROR_TRANSFER);
  }
  LTDC->ICR = clear;
  __DSB();
}

void LCD_RecordHardFault(void)
{
  /* PM0214/CMSIS fault registers; do not touch FMC or LCD peripheral here. */
  g_lcd_diagnostic.cfsr = SCB->CFSR;
  g_lcd_diagnostic.hfsr = SCB->HFSR;
  g_lcd_diagnostic.bfar_valid = (SCB->CFSR & SCB_CFSR_BFARVALID_Msk) != 0U;
  if (g_lcd_diagnostic.bfar_valid) { g_lcd_diagnostic.bfar = SCB->BFAR; }
  g_lcd_diagnostic.fault_address = g_lcd_diagnostic.current_address;
  LCD_Fail(LCD_ERROR_HARDFAULT);
  __DSB();
}

__attribute__((noreturn, noinline)) void LCD_DiagnosticStop(void)
{
  for (;;) { __WFI(); } /* Preserve image and diagnosis; no retry/reinitialization. */
}
