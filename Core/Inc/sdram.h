#ifndef OSCILL_SDRAM_H
#define OSCILL_SDRAM_H

#include "stm32f4xx_hal.h"

#define SDRAM_BASE_ADDRESS             0xD0000000UL
#define SDRAM_SIZE_BYTES               (8UL * 1024UL * 1024UL)
#define SDRAM_SDCLK_NOMINAL_HZ         80000000UL
#define SDRAM_SDCLK_MIN_ASSUMED_HZ     72000000UL
#define SDRAM_REFRESH_WINDOW_MS        64UL
#define SDRAM_REFRESH_ROWS             4096UL
#define SDRAM_REFRESH_COUNT_CYCLES     \
  ((uint32_t)(((uint64_t)SDRAM_SDCLK_MIN_ASSUMED_HZ * SDRAM_REFRESH_WINDOW_MS) \
              / (1000ULL * SDRAM_REFRESH_ROWS) - 20ULL))
#define SDRAM_CPU_TEST_COUNT           6U

typedef enum {
  SDRAM_STAGE_NOT_STARTED = 0,
  SDRAM_STAGE_CONFIG_CHECK,
  SDRAM_STAGE_CLOCK_ENABLE,
  SDRAM_STAGE_POWERUP_DELAY,
  SDRAM_STAGE_PRECHARGE,
  SDRAM_STAGE_AUTO_REFRESH,
  SDRAM_STAGE_LOAD_MODE,
  SDRAM_STAGE_REFRESH_RATE,
  SDRAM_STAGE_INITIALIZED,
  SDRAM_STAGE_PATTERNS,
  SDRAM_STAGE_ALIAS,
  SDRAM_STAGE_RETENTION_WRITE,
  SDRAM_STAGE_RETENTION_WAIT,
  SDRAM_STAGE_RETENTION_READ,
  SDRAM_STAGE_COMPLETE
} SDRAM_Stage;

typedef enum {
  SDRAM_RESULT_NOT_STARTED = 0,
  SDRAM_RESULT_RUNNING,
  SDRAM_RESULT_PASS,
  SDRAM_RESULT_CONFIG_ERROR,
  SDRAM_RESULT_HAL_ERROR,
  SDRAM_RESULT_WAIT_TIMEOUT,
  SDRAM_RESULT_MEMORY_MISMATCH
} SDRAM_Result;

typedef enum {
  SDRAM_TEST_NOT_TESTED = 0,
  SDRAM_TEST_RUNNING,
  SDRAM_TEST_PASS,
  SDRAM_TEST_FAIL
} SDRAM_TestResult;

/* Fixed-size debugger record. Linker .bss is internal RAM, never SDRAM/CCM. */
typedef struct {
  uint32_t stage;
  uint32_t result;
  uint32_t init_complete;
  uint32_t error_count;
  uint32_t first_error_address;
  uint32_t expected_value;
  uint32_t observed_value;
  uint32_t last_hal_status;
  uint32_t last_sdsr;
  uint32_t completed_command_mask;
  uint32_t nominal_sdclk_hz;
  uint32_t refresh_count_cycles;
  uint32_t current_address;
  uint32_t current_test;
  uint32_t writes_completed;
  uint32_t reads_completed;
  uint32_t retention_start_tick_ms;
  uint32_t retention_elapsed_tick_ms;
  uint32_t test_result[SDRAM_CPU_TEST_COUNT];
} SDRAM_Diagnostic;

extern volatile SDRAM_Diagnostic g_sdram_diagnostic;

HAL_StatusTypeDef SDRAM_Initialize(SDRAM_HandleTypeDef *hsdram);
void SDRAM_RunCpuTests(void);

/* Set 0 to select the original sparse test; does not enable any peripherals. */
#define SDRAM_EXTENDED_TESTS           1U
#define SDRAM_EXTENDED_TEST_COUNT      15U
#define SDRAM_ERROR_RECORD_COUNT       16U

typedef enum {
  SDRAM_EXT_NOT_STARTED = 0, SDRAM_EXT_RUNNING, SDRAM_EXT_PASS,
  SDRAM_EXT_FAIL, SDRAM_EXT_HARDFAULT, SDRAM_EXT_TIMEOUT, SDRAM_EXT_CONFIG_ERROR
} SDRAM_ExtState;

typedef enum {
  SDRAM_PHASE_IDLE = 0, SDRAM_PHASE_WRITE, SDRAM_PHASE_READ,
  SDRAM_PHASE_WAIT, SDRAM_PHASE_COMPLETE
} SDRAM_ExtPhase;

typedef struct {
  uint32_t state;
  uint32_t elapsed_ms; /* HAL ticks; see measured HSI correction in report. */
  uint64_t tested_bytes; /* Sum of compared read widths, including repetitions. */
  uint64_t write_count;
  uint64_t read_count;
  uint64_t error_count;
} SDRAM_TestSummary;

typedef struct {
  uint32_t address, expected, actual, test_id, access_bytes;
} SDRAM_ErrorRecord;

typedef struct {
  uint32_t sdcr1, sdcr2, sdtr1, sdtr2, sdrtr, sdsr;
} SDRAM_RegisterSnapshot;

typedef struct {
  uint32_t test_id; /* 1..15; table in SDRAM_EXTENDED_DIAGNOSTIC.md. */
  uint32_t test_state;
  uint32_t phase;
  uint32_t current_address;
  uint32_t access_bytes;
  uint32_t expected;
  uint32_t actual;
  uint32_t first_error_addr;
  uint32_t first_expected;
  uint32_t first_actual;
  uint32_t elapsed_ms;
  uint32_t unique_covered_bytes; /* 8 MiB only after a complete full read pass. */
  uint64_t tested_bytes;
  uint64_t write_count;
  uint64_t read_count;
  uint64_t error_count;
  uint32_t stored_errors;
  uint32_t config_unchanged;
  uint32_t cfsr, hfsr, bfar_valid, bfar;
  uint32_t fault_test_id, fault_address;
  uint32_t retention_wait_tick_ms[2];
  SDRAM_RegisterSnapshot before, after;
  SDRAM_TestSummary tests[SDRAM_EXTENDED_TEST_COUNT];
  SDRAM_ErrorRecord errors[SDRAM_ERROR_RECORD_COUNT];
} SDRAM_ExtendedDiagnostic;

extern volatile SDRAM_ExtendedDiagnostic g_sdram_extended;
void SDRAM_RunExtendedTests(void);
void SDRAM_RecordHardFault(void);
__attribute__((noreturn, noinline)) void SDRAM_DiagnosticStop(void);

#endif
