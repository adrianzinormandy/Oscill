#include "sdram.h"

#define SDRAM_COMMAND_TIMEOUT_MS       10U
#define SDRAM_WAIT_LIMIT_POLLS         20000000UL
#define SDRAM_POWERUP_DELAY_MS         2U
#define SDRAM_RETENTION_DELAY_MS       100U
#define SDRAM_AUTO_REFRESH_COMMANDS    4U
#define SDRAM_MODE_REGISTER_BITS       0x0230U

volatile SDRAM_Diagnostic g_sdram_diagnostic
  __attribute__((section(".bss.sdram_diagnostic"), aligned(4)));

/* RM0090 SDRAM address mapping: exercise every word-address bit and 8 MiB end.
 * Sparse destructive CPU test, not a full-capacity RAM certification.
 */
static const uint32_t test_offsets_bytes[] = {
  0x000000U, 0x000002U, 0x000004U, 0x000008U,
  0x000010U, 0x000020U, 0x000040U, 0x000080U,
  0x000100U, 0x000200U, 0x000400U, 0x000800U,
  0x001000U, 0x002000U, 0x004000U, 0x008000U,
  0x010000U, 0x020000U, 0x040000U, 0x080000U,
  0x100000U, 0x200000U, 0x400000U, 0x7FFFFEU
};
static const uint16_t test_patterns[] = {0x0000U, 0xFFFFU, 0xAAAAU, 0x5555U};

#define SDRAM_TEST_ADDRESS_COUNT \
  (sizeof(test_offsets_bytes) / sizeof(test_offsets_bytes[0]))

_Static_assert(SDRAM_REFRESH_COUNT_CYCLES == 1105U, "Review refresh calculation");
_Static_assert(SDRAM_REFRESH_COUNT_CYCLES >= 41U, "RM0090 minimum refresh count");
_Static_assert(SDRAM_REFRESH_COUNT_CYCLES <= 8191U, "Refresh COUNT field width");

static HAL_StatusTypeDef fail_init(SDRAM_Result result, HAL_StatusTypeDef status)
{
  g_sdram_diagnostic.result = result;
  g_sdram_diagnostic.last_hal_status = status;
  g_sdram_diagnostic.error_count++;
  return status;
}

/* RM0090 37.7.5 SDSR.BUSY. Both tick timeout and finite poll budget are used.
 * A stalled AHB load itself cannot be interrupted by this software timeout.
 */
static HAL_StatusTypeDef wait_ready(SDRAM_HandleTypeDef *hsdram)
{
  const uint32_t start_tick_ms = HAL_GetTick();
  for (uint32_t poll = 0U; poll < SDRAM_WAIT_LIMIT_POLLS; ++poll) {
    const uint32_t status = hsdram->Instance->SDSR;
    g_sdram_diagnostic.last_sdsr = status;
    if ((status & FMC_SDSR_BUSY) == 0U) {
      return HAL_OK;
    }
    if ((HAL_GetTick() - start_tick_ms) >= SDRAM_COMMAND_TIMEOUT_MS) {
      break;
    }
  }
  return HAL_TIMEOUT;
}

/* TIM6 is the existing HAL time base. No timer/RCC configuration is changed.
 * One extra tick avoids an initial partial millisecond. Finite if tick stops.
 */
static HAL_StatusTypeDef delay_ms_bounded(uint32_t delay_ms)
{
  const uint32_t start_tick_ms = HAL_GetTick();
  for (uint32_t poll = 0U; poll < SDRAM_WAIT_LIMIT_POLLS; ++poll) {
    if ((HAL_GetTick() - start_tick_ms) >= (delay_ms + 1U)) {
      return HAL_OK;
    }
  }
  return HAL_TIMEOUT;
}

/* RM0090 37.7.3/37.7.5, SDCMR startup commands to Bank2 only.
 * HAL 1.8.5 discards FMC_SDRAM_SendCommand's return value. Timeout=0 prevents
 * a tick-dependent LL wait; the independent BUSY check below checks completion.
 */
static HAL_StatusTypeDef send_command(SDRAM_HandleTypeDef *hsdram,
                                      SDRAM_Stage stage, uint32_t mode,
                                      uint32_t refresh_commands, uint32_t mode_bits,
                                      uint32_t completed_bit)
{
  FMC_SDRAM_CommandTypeDef command = {0};
  g_sdram_diagnostic.stage = stage;
  HAL_StatusTypeDef status = wait_ready(hsdram);
  if (status != HAL_OK) {
    return fail_init(SDRAM_RESULT_WAIT_TIMEOUT, status);
  }
  command.CommandMode = mode;
  command.CommandTarget = FMC_SDRAM_CMD_TARGET_BANK2;
  command.AutoRefreshNumber = refresh_commands;
  command.ModeRegisterDefinition = mode_bits;
  status = HAL_SDRAM_SendCommand(hsdram, &command, 0U);
  g_sdram_diagnostic.last_hal_status = status;
  if (status != HAL_OK) {
    return fail_init(SDRAM_RESULT_HAL_ERROR, status);
  }
  status = wait_ready(hsdram);
  if (status != HAL_OK) {
    return fail_init(SDRAM_RESULT_WAIT_TIMEOUT, status);
  }
  g_sdram_diagnostic.completed_command_mask |= completed_bit;
  return HAL_OK;
}

HAL_StatusTypeDef SDRAM_Initialize(SDRAM_HandleTypeDef *hsdram)
{
  g_sdram_diagnostic = (SDRAM_Diagnostic){0};
  g_sdram_diagnostic.stage = SDRAM_STAGE_CONFIG_CHECK;
  g_sdram_diagnostic.result = SDRAM_RESULT_RUNNING;
  /* Existing CubeMX geometry, RM0090 37.7.5 SDCR; no second handle/FMC init. */
  if ((hsdram == NULL) || (hsdram->Instance != FMC_SDRAM_DEVICE) ||
      (hsdram->Init.SDBank != FMC_SDRAM_BANK2) ||
      (hsdram->Init.MemoryDataWidth != FMC_SDRAM_MEM_BUS_WIDTH_16) ||
      (hsdram->Init.RowBitsNumber != FMC_SDRAM_ROW_BITS_NUM_12) ||
      (hsdram->Init.ColumnBitsNumber != FMC_SDRAM_COLUMN_BITS_NUM_8) ||
      (hsdram->Init.InternalBankNumber != FMC_SDRAM_INTERN_BANKS_NUM_4) ||
      (hsdram->Init.CASLatency != FMC_SDRAM_CAS_LATENCY_3) ||
      (hsdram->Init.SDClockPeriod != FMC_SDRAM_CLOCK_PERIOD_2)) {
    return fail_init(SDRAM_RESULT_CONFIG_ERROR, HAL_ERROR);
  }
  g_sdram_diagnostic.nominal_sdclk_hz = HAL_RCC_GetHCLKFreq() / 2U;
  g_sdram_diagnostic.refresh_count_cycles = SDRAM_REFRESH_COUNT_CYCLES;
  if (g_sdram_diagnostic.nominal_sdclk_hz != SDRAM_SDCLK_NOMINAL_HZ) {
    return fail_init(SDRAM_RESULT_CONFIG_ERROR, HAL_ERROR);
  }
  if (send_command(hsdram, SDRAM_STAGE_CLOCK_ENABLE,
                   FMC_SDRAM_CMD_CLK_ENABLE, 1U, 0U, 1U) != HAL_OK) {
    return HAL_ERROR;
  }
  /* ISSI G1 Initialization / AC Characteristics; >=100 us (power-up note 200 us).
   * Three software ticks with HSI margin exceed both requirements. BSP uses 1 ms.
   */
  g_sdram_diagnostic.stage = SDRAM_STAGE_POWERUP_DELAY;
  if (delay_ms_bounded(SDRAM_POWERUP_DELAY_MS) != HAL_OK) {
    return fail_init(SDRAM_RESULT_WAIT_TIMEOUT, HAL_TIMEOUT);
  }
  if (send_command(hsdram, SDRAM_STAGE_PRECHARGE,
                   FMC_SDRAM_CMD_PALL, 1U, 0U, 2U) != HAL_OK) {
    return HAL_ERROR;
  }
  /* Four Auto Refresh commands, as in STM32F429I-Discovery BSP. */
  if (send_command(hsdram, SDRAM_STAGE_AUTO_REFRESH,
                   FMC_SDRAM_CMD_AUTOREFRESH_MODE,
                   SDRAM_AUTO_REFRESH_COMMANDS, 0U, 4U) != HAL_OK) {
    return HAL_ERROR;
  }
  /* ISSI Mode Register: BL=1, sequential, CAS=3, standard, single write burst.
   * RM0090 37.7.3 requires BL=1 and CAS matching FMC_SDCR2.
   */
  if (send_command(hsdram, SDRAM_STAGE_LOAD_MODE,
                   FMC_SDRAM_CMD_LOAD_MODE, 1U, SDRAM_MODE_REGISTER_BITS, 8U)
      != HAL_OK) {
    return HAL_ERROR;
  }
  /* RM0090 37.7.5 SDRTR: floor(f_SDCLK * 64 ms / 4096 - 20).
   * Nominal 80 MHz gives 1230; use 72 MHz (-10% assumption) => 1105.
   * This is a frequency margin, not a guarantee outside supply specifications.
   */
  g_sdram_diagnostic.stage = SDRAM_STAGE_REFRESH_RATE;
  HAL_StatusTypeDef status = HAL_SDRAM_ProgramRefreshRate(
      hsdram, SDRAM_REFRESH_COUNT_CYCLES);
  g_sdram_diagnostic.last_hal_status = status;
  if (status != HAL_OK) {
    return fail_init(SDRAM_RESULT_HAL_ERROR, status);
  }
  status = wait_ready(hsdram);
  if (status != HAL_OK) {
    return fail_init(SDRAM_RESULT_WAIT_TIMEOUT, status);
  }
  const uint32_t refresh_count = (hsdram->Instance->SDRTR & FMC_SDRTR_COUNT)
                                 >> FMC_SDRTR_COUNT_Pos;
  if ((refresh_count != SDRAM_REFRESH_COUNT_CYCLES) ||
      ((g_sdram_diagnostic.last_sdsr & FMC_SDSR_MODES2) != 0U)) {
    return fail_init(SDRAM_RESULT_CONFIG_ERROR, HAL_ERROR);
  }
  g_sdram_diagnostic.init_complete = 1U;
  g_sdram_diagnostic.stage = SDRAM_STAGE_INITIALIZED;
  return HAL_OK;
}

static uint16_t address_signature(uint32_t index)
{
  return (uint16_t)(0x1357U ^ (index * 0x0101U));
}

static void write_test_words(uint16_t pattern, uint32_t use_signature)
{
  for (uint32_t i = 0U; i < SDRAM_TEST_ADDRESS_COUNT; ++i) {
    const uint32_t address = SDRAM_BASE_ADDRESS + test_offsets_bytes[i];
    volatile uint16_t * const word = (volatile uint16_t *)(uintptr_t)address;
    g_sdram_diagnostic.current_address = address;
    *word = use_signature ? address_signature(i) : pattern;
    g_sdram_diagnostic.writes_completed++;
  }
  /* Cortex-M4 has no data cache; drain CPU writes before separate reads/wait. */
  __DSB();
}

static uint32_t check_test_words(uint16_t pattern, uint32_t use_signature)
{
  for (uint32_t i = 0U; i < SDRAM_TEST_ADDRESS_COUNT; ++i) {
    const uint32_t address = SDRAM_BASE_ADDRESS + test_offsets_bytes[i];
    volatile uint16_t * const word = (volatile uint16_t *)(uintptr_t)address;
    const uint16_t expected = use_signature ? address_signature(i) : pattern;
    g_sdram_diagnostic.current_address = address;
    const uint16_t observed = *word;
    g_sdram_diagnostic.reads_completed++;
    if (observed != expected) {
      g_sdram_diagnostic.error_count++;
      g_sdram_diagnostic.first_error_address = address;
      g_sdram_diagnostic.expected_value = expected;
      g_sdram_diagnostic.observed_value = observed;
      g_sdram_diagnostic.result = SDRAM_RESULT_MEMORY_MISMATCH;
      g_sdram_diagnostic.test_result[g_sdram_diagnostic.current_test] = SDRAM_TEST_FAIL;
      return 0U; /* No further external-memory access after a mismatch. */
    }
  }
  return 1U;
}

void SDRAM_RunCpuTests(void)
{
  if ((g_sdram_diagnostic.init_complete == 0U) ||
      (g_sdram_diagnostic.result != SDRAM_RESULT_RUNNING)) {
    return;
  }
  /* RM0090 memory map / SDRAM mapping; volatile halfword CPU accesses only.
   * Four pattern tests, distinct-address alias test, then ~100 ms retention.
   */
  g_sdram_diagnostic.stage = SDRAM_STAGE_PATTERNS;
  for (uint32_t p = 0U; p < 4U; ++p) {
    g_sdram_diagnostic.current_test = p;
    g_sdram_diagnostic.test_result[p] = SDRAM_TEST_RUNNING;
    write_test_words(test_patterns[p], 0U);
    if (!check_test_words(test_patterns[p], 0U)) {
      return;
    }
    g_sdram_diagnostic.test_result[p] = SDRAM_TEST_PASS;
  }
  g_sdram_diagnostic.stage = SDRAM_STAGE_ALIAS;
  g_sdram_diagnostic.current_test = 4U;
  g_sdram_diagnostic.test_result[4] = SDRAM_TEST_RUNNING;
  write_test_words(0U, 1U);
  if (!check_test_words(0U, 1U)) {
    return;
  }
  g_sdram_diagnostic.test_result[4] = SDRAM_TEST_PASS;

  g_sdram_diagnostic.current_test = 5U;
  g_sdram_diagnostic.test_result[5] = SDRAM_TEST_RUNNING;
  g_sdram_diagnostic.stage = SDRAM_STAGE_RETENTION_WRITE;
  write_test_words(0U, 1U);
  g_sdram_diagnostic.stage = SDRAM_STAGE_RETENTION_WAIT;
  g_sdram_diagnostic.retention_start_tick_ms = HAL_GetTick();
  const HAL_StatusTypeDef status = delay_ms_bounded(SDRAM_RETENTION_DELAY_MS);
  g_sdram_diagnostic.retention_elapsed_tick_ms =
    HAL_GetTick() - g_sdram_diagnostic.retention_start_tick_ms;
  if (status != HAL_OK) {
    g_sdram_diagnostic.test_result[5] = SDRAM_TEST_FAIL;
    (void)fail_init(SDRAM_RESULT_WAIT_TIMEOUT, status);
    return;
  }
  g_sdram_diagnostic.stage = SDRAM_STAGE_RETENTION_READ;
  if (!check_test_words(0U, 1U)) {
    return;
  }
  g_sdram_diagnostic.test_result[5] = SDRAM_TEST_PASS;
  g_sdram_diagnostic.stage = SDRAM_STAGE_COMPLETE;
  g_sdram_diagnostic.result = SDRAM_RESULT_PASS;
}

/* Extended CPU-only diagnostic. RM0090 37.4.3 Table 261: x16, 8 column,
 * 12 row bits => HADDR[8:1] columns, [20:9] rows, [22:21] internal banks.
 * Code/constants stay in Flash; records and the existing stack stay in SRAM.
 */
#define SDRAM_WORD_COUNT             (SDRAM_SIZE_BYTES / 2U)
#define SDRAM_RETENTION_POLL_LIMIT   500000000UL
/* Measured SDCLK <=81.9 MHz vs nominal 80 MHz. 3% extra HAL ticks provides
 * >= requested wall time within that measured range, without RCC changes.
 */
#define SDRAM_WAIT_1S_TICKS_MS        1030U
#define SDRAM_WAIT_10S_TICKS_MS       10300U

volatile SDRAM_ExtendedDiagnostic g_sdram_extended
  __attribute__((section(".bss.sdram_extended"), aligned(8)));

static const uint32_t ext_probe_offsets_bytes[] = {
  0U, 2U, 4U, 8U, 16U, 32U, 64U, 128U, 256U, 512U,
  1024U, 2048U, 4096U, 8192U, 16384U, 32768U, 65536U,
  131072U, 262144U, 524288U, 1048576U, 2097152U, 4194304U,
  6291456U, SDRAM_SIZE_BYTES - 2U
};
static const uint32_t ext_region_offsets_bytes[] = {
  0U, 0x0001F8U, 0x1FFFF8U, 0x200000U,
  0x400000U, 0x600000U, 0x6001F8U, SDRAM_SIZE_BYTES - 8U
};
static const uint16_t ext_fixed_patterns[] = {
  0x0000U, 0xFFFFU, 0xAAAAU, 0x5555U, 0x3333U, 0xCCCCU
};
#define EXT_PROBE_COUNT (sizeof(ext_probe_offsets_bytes) / sizeof(ext_probe_offsets_bytes[0]))
#define EXT_REGION_COUNT (sizeof(ext_region_offsets_bytes) / sizeof(ext_region_offsets_bytes[0]))

static void ext_snapshot(volatile SDRAM_RegisterSnapshot *s)
{
  /* Read only: RM0090 37.7.5; SDSR.BUSY may differ between snapshots. */
  s->sdcr1 = FMC_Bank5_6->SDCR[0]; s->sdcr2 = FMC_Bank5_6->SDCR[1];
  s->sdtr1 = FMC_Bank5_6->SDTR[0]; s->sdtr2 = FMC_Bank5_6->SDTR[1];
  s->sdrtr = FMC_Bank5_6->SDRTR; s->sdsr = FMC_Bank5_6->SDSR;
}

static void ext_begin(uint32_t index)
{
  g_sdram_extended.test_id = index + 1U;
  g_sdram_extended.tests[index].state = SDRAM_EXT_RUNNING;
  g_sdram_extended.phase = SDRAM_PHASE_WRITE;
}

static void ext_write(uint32_t address, uint32_t value, uint32_t bytes)
{
  g_sdram_extended.phase = SDRAM_PHASE_WRITE;
  g_sdram_extended.current_address = address;
  g_sdram_extended.access_bytes = bytes;
  g_sdram_extended.expected = value;
  if (bytes == 1U) {
    *(volatile uint8_t *)(uintptr_t)address = (uint8_t)value;
  } else if (bytes == 2U) {
    *(volatile uint16_t *)(uintptr_t)address = (uint16_t)value;
  } else {
    *(volatile uint32_t *)(uintptr_t)address = value;
  }
  g_sdram_extended.write_count++;
  g_sdram_extended.tests[g_sdram_extended.test_id - 1U].write_count++;
}

static void ext_check(uint32_t address, uint32_t expected, uint32_t bytes)
{
  const uint32_t index = g_sdram_extended.test_id - 1U;
  g_sdram_extended.phase = SDRAM_PHASE_READ;
  g_sdram_extended.current_address = address;
  g_sdram_extended.access_bytes = bytes;
  g_sdram_extended.expected = expected;
  uint32_t actual;
  if (bytes == 1U) {
    actual = *(volatile uint8_t *)(uintptr_t)address;
  } else if (bytes == 2U) {
    actual = *(volatile uint16_t *)(uintptr_t)address;
  } else {
    actual = *(volatile uint32_t *)(uintptr_t)address;
  }
  g_sdram_extended.actual = actual;
  g_sdram_extended.read_count++;
  g_sdram_extended.tested_bytes += bytes;
  g_sdram_extended.tests[index].read_count++;
  g_sdram_extended.tests[index].tested_bytes += bytes;
  if (actual != expected) {
    if (g_sdram_extended.error_count == 0U) {
      g_sdram_extended.first_error_addr = address;
      g_sdram_extended.first_expected = expected;
      g_sdram_extended.first_actual = actual;
    }
    g_sdram_extended.error_count++;
    g_sdram_extended.tests[index].error_count++;
    const uint32_t slot = g_sdram_extended.stored_errors;
    if (slot < SDRAM_ERROR_RECORD_COUNT) {
      g_sdram_extended.errors[slot] = (SDRAM_ErrorRecord){
        address, expected, actual, index + 1U, bytes
      };
      g_sdram_extended.stored_errors = slot + 1U;
    }
  }
}

static uint32_t ext_end(uint32_t start_tick_ms)
{
  const uint32_t index = g_sdram_extended.test_id - 1U;
  const uint32_t ok = (g_sdram_extended.tests[index].error_count == 0U)
    && (g_sdram_extended.test_state == SDRAM_EXT_RUNNING);
  g_sdram_extended.tests[index].elapsed_ms = HAL_GetTick() - start_tick_ms;
  g_sdram_extended.tests[index].state = ok ? SDRAM_EXT_PASS : SDRAM_EXT_FAIL;
  if (!ok && g_sdram_extended.test_state == SDRAM_EXT_RUNNING) {
    g_sdram_extended.test_state = SDRAM_EXT_FAIL;
  }
  return ok;
}

static uint16_t ext_signature(uint32_t word_index)
{
  /* Avalanche all 22 word-index bits into 16 data bits; fixed seed, no buffer.
   * A single 16-bit value cannot uniquely encode 4M addresses. Test B separately
   * checks single address bits; this pattern checks full-capacity addressing.
   */
  uint32_t x = word_index ^ 0x6D2B79F5U;
  x ^= x >> 16; x *= 0x7FEB352DU;
  x ^= x >> 15; x *= 0x846CA68BU;
  x ^= x >> 16;
  return (uint16_t)x;
}

static void ext_full_write(uint16_t pattern, uint32_t signature, uint32_t invert)
{
  for (uint32_t i = 0U; i < SDRAM_WORD_COUNT; ++i) {
    uint16_t expected = signature ? ext_signature(i) : pattern;
    if (invert) { expected = (uint16_t)~expected; }
    ext_write(SDRAM_BASE_ADDRESS + 2U * i, expected, 2U);
  }
  __DSB(); /* Complete the entire fill before any verification or delay. */
}

static void ext_full_read(uint16_t pattern, uint32_t signature, uint32_t invert)
{
  for (uint32_t i = 0U; i < SDRAM_WORD_COUNT; ++i) {
    uint16_t expected = signature ? ext_signature(i) : pattern;
    if (invert) { expected = (uint16_t)~expected; }
    ext_check(SDRAM_BASE_ADDRESS + 2U * i, expected, 2U);
  }
  /* All errors in this pass counted, first 16 preserved. No next test on FAIL. */
  g_sdram_extended.unique_covered_bytes = SDRAM_SIZE_BYTES;
}

static void ext_data_lines(void)
{
  for (uint32_t r = 0U; r < EXT_REGION_COUNT; ++r) {
    const uint32_t address = SDRAM_BASE_ADDRESS + ext_region_offsets_bytes[r];
    for (uint32_t bit = 0U; bit < 16U; ++bit) {
      for (uint32_t inverse = 0U; inverse < 2U; ++inverse) {
        const uint16_t one = (uint16_t)(1U << bit);
        const uint16_t pattern = inverse ? (uint16_t)~one : one;
        ext_write(address, pattern, 2U); __DSB();
        ext_check(address, pattern, 2U);
      }
    }
  }
}

static void ext_address_bus(void)
{
  for (uint32_t i = 0U; i < EXT_PROBE_COUNT; ++i) {
    ext_write(SDRAM_BASE_ADDRESS + ext_probe_offsets_bytes[i], 0xAAAAU, 2U);
  }
  __DSB();
  /* Change each probe alone, including base and the last halfword. Check all
   * probes for collateral changes, then restore. Detect stuck-high/low/shorts.
   * All powers of two are expressed as byte offset 2*(1<<word_address_bit).
   */
  for (uint32_t i = 0U; i < EXT_PROBE_COUNT; ++i) {
    const uint32_t address = SDRAM_BASE_ADDRESS + ext_probe_offsets_bytes[i];
    ext_write(address, 0x5555U, 2U); __DSB();
    for (uint32_t j = 0U; j < EXT_PROBE_COUNT; ++j) {
      ext_check(SDRAM_BASE_ADDRESS + ext_probe_offsets_bytes[j],
                j == i ? 0x5555U : 0xAAAAU, 2U);
    }
    ext_write(address, 0xAAAAU, 2U); __DSB();
  }
}

static void ext_width(uint32_t bytes)
{
  static const uint8_t byte_patterns[] = {0x00U, 0xFFU, 0xAAU, 0x55U};
  for (uint32_t r = 0U; r < EXT_REGION_COUNT; ++r) {
    const uint32_t address = SDRAM_BASE_ADDRESS + ext_region_offsets_bytes[r];
    if (bytes == 1U) {
      /* RM0090 37.4.3 byte masks: verify NBL0 and NBL1 independently. */
      ext_write(address, 0xA55AU, 2U); __DSB();
      for (uint32_t p = 0U; p < 4U; ++p) {
        ext_write(address, byte_patterns[p], 1U); __DSB();
        ext_check(address, byte_patterns[p], 1U);
        ext_check(address + 1U, 0xA5U, 1U);
        ext_check(address, 0xA500U | byte_patterns[p], 2U);
      }
      ext_write(address, 0x5AA5U, 2U); __DSB();
      for (uint32_t p = 0U; p < 4U; ++p) {
        ext_write(address + 1U, byte_patterns[p], 1U); __DSB();
        ext_check(address, 0xA5U, 1U);
        ext_check(address + 1U, byte_patterns[p], 1U);
        ext_check(address, (uint32_t)byte_patterns[p] << 8 | 0xA5U, 2U);
      }
    } else if (bytes == 2U) {
      for (uint32_t i = 0U; i < 4U; ++i) {
        ext_write(address + 2U * i, (uint16_t)(0x1234U ^ (0x3333U * i)), 2U);
      }
      __DSB();
      for (uint32_t i = 0U; i < 4U; ++i) {
        ext_check(address + 2U * i, (uint16_t)(0x1234U ^ (0x3333U * i)), 2U);
      }
    } else {
      /* Addresses and two 32-bit words are naturally aligned and in range. */
      ext_write(address, 0x01234567U ^ r, 4U);
      ext_write(address + 4U, 0xFEDCBA98U ^ r, 4U); __DSB();
      ext_check(address, 0x01234567U ^ r, 4U);
      ext_check(address + 4U, 0xFEDCBA98U ^ r, 4U);
      ext_write(address, ~(0x01234567U ^ r), 4U);
      ext_write(address + 4U, ~(0xFEDCBA98U ^ r), 4U); __DSB();
      ext_check(address, ~(0x01234567U ^ r), 4U);
      ext_check(address + 4U, ~(0xFEDCBA98U ^ r), 4U);
    }
  }
}

static uint32_t ext_retention_wait(uint32_t ticks_ms, uint32_t slot)
{
  g_sdram_extended.phase = SDRAM_PHASE_WAIT;
  const uint32_t start = HAL_GetTick();
  /* TIM6 ticks continue while CPU waits. No reads/writes of SDRAM here.
   * No self-refresh, SDCLK or SDRTR changes. Finite even if HAL ticks stop.
   */
  for (uint32_t poll = 0U; poll < SDRAM_RETENTION_POLL_LIMIT; ++poll) {
    const uint32_t elapsed = HAL_GetTick() - start;
    if (elapsed >= ticks_ms + 1U) {
      g_sdram_extended.retention_wait_tick_ms[slot] = elapsed;
      return 1U;
    }
  }
  g_sdram_extended.retention_wait_tick_ms[slot] = HAL_GetTick() - start;
  g_sdram_extended.test_state = SDRAM_EXT_TIMEOUT;
  return 0U;
}

static void ext_finish(uint32_t start_tick_ms)
{
  __DSB();
  ext_snapshot(&g_sdram_extended.after);
  const volatile SDRAM_RegisterSnapshot *a = &g_sdram_extended.before;
  const volatile SDRAM_RegisterSnapshot *b = &g_sdram_extended.after;
  g_sdram_extended.config_unchanged = a->sdcr1 == b->sdcr1 && a->sdcr2 == b->sdcr2
    && a->sdtr1 == b->sdtr1 && a->sdtr2 == b->sdtr2 && a->sdrtr == b->sdrtr;
  g_sdram_extended.cfsr = SCB->CFSR;
  g_sdram_extended.hfsr = SCB->HFSR;
  g_sdram_extended.bfar_valid = (SCB->CFSR & SCB_CFSR_BFARVALID_Msk) != 0U;
  if (g_sdram_extended.bfar_valid) { g_sdram_extended.bfar = SCB->BFAR; }
  g_sdram_extended.elapsed_ms = HAL_GetTick() - start_tick_ms;
  g_sdram_extended.phase = SDRAM_PHASE_COMPLETE;
  if (g_sdram_extended.test_state == SDRAM_EXT_RUNNING) {
    g_sdram_extended.test_state = g_sdram_extended.config_unchanged
      && g_sdram_extended.cfsr == 0U && g_sdram_extended.hfsr == 0U
      ? SDRAM_EXT_PASS : SDRAM_EXT_CONFIG_ERROR;
  }
}

void SDRAM_RunExtendedTests(void)
{
  g_sdram_extended = (SDRAM_ExtendedDiagnostic){0};
  if (g_sdram_diagnostic.init_complete == 0U) {
    g_sdram_extended.test_state = SDRAM_EXT_CONFIG_ERROR;
    return;
  }
  const uint32_t start = HAL_GetTick();
  uint32_t test_start;
  g_sdram_extended.test_state = SDRAM_EXT_RUNNING;
  ext_snapshot(&g_sdram_extended.before);
  if (g_sdram_extended.before.sdrtr != (SDRAM_REFRESH_COUNT_CYCLES << 1)) {
    g_sdram_extended.test_state = SDRAM_EXT_CONFIG_ERROR;
    goto finish;
  }

  ext_begin(0U); test_start = HAL_GetTick(); ext_data_lines();
  if (!ext_end(test_start)) { goto finish; }
  ext_begin(1U); test_start = HAL_GetTick(); ext_address_bus();
  if (!ext_end(test_start)) { goto finish; }
  for (uint32_t p = 0U; p < 6U; ++p) {
    ext_begin(2U + p); test_start = HAL_GetTick();
    ext_full_write(ext_fixed_patterns[p], 0U, 0U);
    ext_full_read(ext_fixed_patterns[p], 0U, 0U);
    if (!ext_end(test_start)) { goto finish; }
  }
  for (uint32_t inverse = 0U; inverse < 2U; ++inverse) {
    ext_begin(8U + inverse); test_start = HAL_GetTick();
    ext_full_write(0U, 1U, inverse); ext_full_read(0U, 1U, inverse);
    if (!ext_end(test_start)) { goto finish; }
  }
  for (uint32_t width = 0U; width < 3U; ++width) {
    ext_begin(10U + width); test_start = HAL_GetTick(); ext_width(1U << width);
    if (!ext_end(test_start)) { goto finish; }
  }
  ext_begin(13U); test_start = HAL_GetTick(); ext_full_write(0U, 1U, 0U);
  if (ext_retention_wait(SDRAM_WAIT_1S_TICKS_MS, 0U)) {
    ext_full_read(0U, 1U, 0U);
  }
  if (!ext_end(test_start)) { goto finish; }
  /* No rewriting between the first and second retention read. */
  ext_begin(14U); test_start = HAL_GetTick();
  if (ext_retention_wait(SDRAM_WAIT_10S_TICKS_MS, 1U)) {
    ext_full_read(0U, 1U, 0U);
  }
  (void)ext_end(test_start);
finish:
  ext_finish(start);
}

void SDRAM_RecordHardFault(void)
{
  /* PM0214 SCB fault status / CMSIS core_cm4.h. SRAM and PPB accesses only;
   * do not attempt FMC reads on a fault path. BFAR meaningful only if valid.
   */
  g_sdram_extended.cfsr = SCB->CFSR;
  g_sdram_extended.hfsr = SCB->HFSR;
  g_sdram_extended.bfar_valid = (SCB->CFSR & SCB_CFSR_BFARVALID_Msk) != 0U;
  if (g_sdram_extended.bfar_valid) { g_sdram_extended.bfar = SCB->BFAR; }
  g_sdram_extended.fault_test_id = g_sdram_extended.test_id;
  g_sdram_extended.fault_address = g_sdram_extended.current_address;
  g_sdram_extended.test_state = SDRAM_EXT_HARDFAULT;
  if (g_sdram_extended.test_id >= 1U
      && g_sdram_extended.test_id <= SDRAM_EXTENDED_TEST_COUNT) {
    g_sdram_extended.tests[g_sdram_extended.test_id - 1U].state = SDRAM_EXT_FAIL;
  }
  __DSB();
}

__attribute__((noreturn, noinline)) void SDRAM_DiagnosticStop(void)
{
  /* Stable debugger endpoint on PASS, FAIL or init failure. TIM6 only continues. */
  for (;;) { __WFI(); }
}
