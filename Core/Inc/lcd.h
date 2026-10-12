#ifndef OSCILL_LCD_H
#define OSCILL_LCD_H

#include "sdram.h"

/* 1: LCD only; 0: return to the preserved SDRAM-only diagnostic in main. */
#define LCD_DIAGNOSTIC_ENABLE       1U
#define LCD_WIDTH_PIXELS            240U
#define LCD_HEIGHT_PIXELS           320U
#define LCD_FRAMEBUFFER_ADDRESS     SDRAM_BASE_ADDRESS
#define LCD_FRAMEBUFFER_BYTES       (LCD_WIDTH_PIXELS * LCD_HEIGHT_PIXELS * 2U)
#define LCD_SDRAM_TEST_ADDRESS      0xD0100000UL
#define LCD_SDRAM_TEST_BYTES        4096U

typedef enum {
  LCD_STATE_NOT_STARTED = 0, LCD_STATE_RUNNING = 1,
  LCD_STATE_PASS = 2, LCD_STATE_FAIL = 3
} LCD_State;
typedef enum {
  LCD_STAGE_NOT_STARTED = 0, LCD_STAGE_FRAMEBUFFER = 1, LCD_STAGE_SPI = 2,
  LCD_STAGE_ILI9341 = 3, LCD_STAGE_LTDC = 4, LCD_STAGE_IMAGES = 5,
  LCD_STAGE_SDRAM_CONCURRENT = 6, LCD_STAGE_ANIMATION = 7
} LCD_Stage;
typedef enum {
  LCD_ERROR_NONE = 0, LCD_ERROR_SDRAM_INIT = 1, LCD_ERROR_SPI_INIT = 2,
  LCD_ERROR_SPI_TRANSFER = 3, LCD_ERROR_DELAY_TIMEOUT = 4,
  LCD_ERROR_LTDC_INIT = 5, LCD_ERROR_LTDC_CONFIG = 6,
  LCD_ERROR_FIFO = 7, LCD_ERROR_TRANSFER = 8, LCD_ERROR_SDRAM_DATA = 9,
  LCD_ERROR_FMC_CHANGED = 10, LCD_ERROR_HARDFAULT = 11,
  LCD_ERROR_HAL_FATAL = 12, LCD_ERROR_PIXEL_BOUNDS = 13
} LCD_Error;
typedef enum {
  LCD_IMAGE_BLACK = 0, LCD_IMAGE_WHITE, LCD_IMAGE_RED, LCD_IMAGE_GREEN,
  LCD_IMAGE_BLUE, LCD_IMAGE_BARS, LCD_IMAGE_GEOMETRY,
  LCD_IMAGE_CHECKER, LCD_IMAGE_ANIMATION, LCD_IMAGE_COUNT
} LCD_Image;

typedef struct {
  uint32_t sscr, bpcr, awcr, twcr, gcr, srcr, ier, isr, cpsr, cdsr;
  uint32_t l1cr, l1whpcr, l1wvpcr, l1pfcr, l1cacr, l1bfcr;
  uint32_t l1cfbar, l1cfblr, l1cfblnr, l2cr;
} LCD_RegisterSnapshot;

typedef struct {
  uint32_t stage, state; /* state is digital status; visual_state remains NOT_STARTED. */
  uint32_t visual_state;
  uint32_t spi_status, ili9341_status, ltdc_status;
  uint32_t framebuffer_addr, pixel_format, width, height, framebuffer_bytes;
  uint32_t spi_error_count, spi_timeout_count, spi_hal_error;
  uint32_t spi_command_count, spi_byte_count, last_command, last_ili_step;
  uint32_t fifo_underrun_count, transfer_error_count, sdram_error_count;
  uint32_t last_error, first_error, first_error_stage, first_error_isr;
  uint32_t image, images_shown_mask, cycles_completed, animation_updates;
  uint32_t sdram_passes, sdram_test_writes, sdram_test_reads;
  uint32_t first_sdram_error_addr, expected, actual;
  uint32_t start_tick_ms, elapsed_tick_ms, nominal_pixel_clock_hz, nominal_spi_clock_hz;
  uint32_t rcc_pllcfgr, rcc_cfgr, rcc_pllsaicfgr, rcc_dckcfgr;
  uint32_t fmc_unchanged, cfsr, hfsr, bfar_valid, bfar, fault_address, current_address;
  uint32_t irq_last_isr, irq_last_ier;
  SDRAM_RegisterSnapshot fmc_before, fmc_after;
  LCD_RegisterSnapshot ltdc_current, ltdc_first_error;
} LCD_Diagnostic;

extern volatile LCD_Diagnostic g_lcd_diagnostic;
HAL_StatusTypeDef LCD_PrepareFramebuffer(void);
HAL_StatusTypeDef LCD_InitializeIli9341(SPI_HandleTypeDef *hspi);
HAL_StatusTypeDef LCD_ConfirmStart(void);
void LCD_Process(void);
void LCD_LTDC_IRQHandler(void);
void LCD_RecordHardFault(void);
void LCD_Fail(LCD_Error error);
__attribute__((noreturn, noinline)) void LCD_DiagnosticStop(void);

#endif
