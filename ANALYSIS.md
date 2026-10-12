# Анализ проекта Oscill (STM32F429I-DISC1) — техническая проверка

Дата анализа: 2026-10-12  
МК: STM32F429ZIT6 (LQFP144, 2 МБ Flash, 192 КБ SRAM + 64 КБ CCM RAM)  
Плата: STM32F429I-DISC1, LCD 240×320 RGB (ILI9341 нет, LTDC напрямую), SDRAM 8 МБ (IS42S32160J) на FMC  
FW: STM32Cube FW_F4 v1.28.3, CubeIDE v6.18.1

Источники: RM0090, STM32F429xx datasheet, UM1670, `stm32f429xx.h`, `stm32f4xx_hal_adc.h`, `stm32f4xx_hal_dma2d.h`, `oscill.ioc`.

---

## 1. Проверка 10 пунктов технической корректности

### Пункт 1: ADC1 и DMA — DMA1, а не DMA2

**Статус: подтверждено.** ADC1 использует **DMA1 Stream 0** (канал 0), а не DMA2.

- **Документ:** RM0090 §11.6.7 "DMA request mapping" — ADC1 request привязан к DMA1 Stream 0.
- **Заголовки:** `stm32f429xx.h` — `DMA1_Stream0_BASE`, `DMA1_Stream1_BASE`. Нет `ADC1_DMA` в DMA2.
- **DMA2** используется для LTDC framebuffer.
- **Влияние на план:** Инициализировать **DMA1_Stream0** для ADC1.

---

### Пункт 2: TIM1_TRGO не является источником EXTSEL для ADC1

**Статус: подтверждено.** TIM1_TRGO **недоступен** как триггер регулярной группы ADC1.

- **Документ:** RM0090 §19.3.10 "EXTSEL[3:0] bits" — регистр `ADC_CR2[27:24]`.
- **Заголовки:** `stm32f4xx_hal_adc.h` строки 330–344:
  ```c
  #define ADC_EXTERNALTRIGCONV_T1_CC1    0x00000000U   // TIM1 CC1
  #define ADC_EXTERNALTRIGCONV_T1_CC2    ADC_CR2_EXTSEL_0   // TIM1 CC2
  #define ADC_EXTERNALTRIGCONV_T1_CC3    ADC_CR2_EXTSEL_1   // TIM1 CC3
  #define ADC_EXTERNALTRIGCONV_T2_TRGO   (ADC_CR2_EXTSEL_2 | ADC_CR2_EXTSEL_1)  // TIM2 TRGO
  #define ADC_EXTERNALTRIGCONV_T3_TRGO   ADC_CR2_EXTSEL_3   // TIM3 TRGO
  #define ADC_EXTERNALTRIGCONV_T8_TRGO   (ADC_CR2_EXTSEL_3 | ADC_CR2_EXTSEL_2 | ADC_CR2_EXTSEL_1)  // TIM8 TRGO
  ```
- **Доступные триггеры регулярной группы:** TIM1_CC1/CC2/CC3, TIM2_TRGO/CC2/CC3/CC4, TIM3_TRGO/CC1, TIM4_CC4, TIM5_CC1/CC2/CC3, TIM8_TRGO/CC1, Ext_IT11.
- **TIM1_TRGO отсутствует в списке.**
- **Влияние на план:** Использовать **TIM2_TRGO** (TIM2 на APB1, clock = 42 МГц).

---

### Пункт 3: LTDC_VSYNC на PA4, а не PA5

**Статус: уточнено.** VSYNC находится на **PA4**. PA5 **не занят LTDC** и свободен для ADC1_IN5.

- **oscill.ioc:** `PA4.Signal = LTDC_VSYNC`, `PA5.Signal = ADCx_IN5`.
- **main.h:** `VSYNC_Pin GPIO_PIN_4`, `PA5` доступен как ADC1_IN5.
- **Влияние на план:** ADC1_IN5 (PA5) **доступен** как аналоговый вход.

---

### Пункт 4: Максимальный ADC clock — 36 МГц, фактический = 10.5 МГц

**Статус: подтверждено и пересчитано.** ADC clock в проекте = **10.5 МГц**.

- **Документ:** STM32F429xx datasheet §2.3 "DC characteristics" — `f_ADC` max = 36 МГц при VDDA = 3.3 В.
- **Заголовки:** `stm32f4xx_hal_adc.h` строки 272–275:
  ```c
  #define ADC_CLOCK_SYNC_PCLK_DIV2    0x00000000U
  #define ADC_CLOCK_SYNC_PCLK_DIV4    ADC_CCR_ADCPRE_0
  #define ADC_CLOCK_SYNC_PCLK_DIV6    ADC_CCR_ADCPRE_1
  #define ADC_CLOCK_SYNC_PCLK_DIV8    ADC_CCR_ADCPRE
  ```
- **Расчёт из проекта:**
  - `RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV8` → PCLK2 = 168 / 8 = **21 МГц**.
  - `hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV2` → ADC clock = 21 / 2 = **10.5 МГц**.
- **Влияние на план:** **Допустимо** (≤ 36 МГц), но ограничивает минимальное время выборки.

---

### Пункт 5: ADC_SAMPLETIME_192CYCLES не существует

**Статус: подтверждено.** 192 цикла **недоступен**.

- **Заголовки:** `stm32f4xx_hal_adc.h` строки 392–399:
  ```c
  #define ADC_SAMPLETIME_3CYCLES    0x00000000U
  #define ADC_SAMPLETIME_15CYCLES   ADC_SMPR1_SMP10_0
  #define ADC_SAMPLETIME_28CYCLES   ADC_SMPR1_SMP10_1
  #define ADC_SAMPLETIME_56CYCLES   (ADC_SMPR1_SMP10_1 | ADC_SMPR1_SMP10_0)
  #define ADC_SAMPLETIME_84CYCLES   ADC_SMPR1_SMP10_2
  #define ADC_SAMPLETIME_112CYCLES  (ADC_SMPR1_SMP10_2 | ADC_SMPR1_SMP10_0)
  #define ADC_SAMPLETIME_144CYCLES  (ADC_SMPR1_SMP10_2 | ADC_SMPR1_SMP10_1)
  #define ADC_SAMPLETIME_480CYCLES  ADC_SMPR1_SMP10
  ```
- **Доступные:** 3, 15, 28, 56, 84, 112, 144, 480 циклов.
- **Влияние на план:** Использовать **ADC_SAMPLETIME_15CYCLES** (компромисс).

---

### Пункт 6: 100 kSPS с выбранным временем выборки

**Пересчёт.**

- ADC clock = 10.5 МГц.
- Время одного преобразования = `(sampling_time + 12) / ADC_clock`.

| Sampling time | Время преобразования | Макс. частота |
|---|---|---|
| 3 цикла | 1.43 мкс | 699 kSPS |
| 15 циклов | 2.57 мкс | 389 kSPS |
| 144 цикла | 15.43 мкс | 64.8 kSPS |
| 480 цикл | 46.86 мкс | 21.3 kSPS |

- Для **100 kSPS** нужен sampling ≤ 9 циклов → доступен только **3 цикла** (699 kSPS).
- **Но** 3 цикла при входном импедансе > 100 Ом даст **большие искажения** (RM0090 §19.6 — max источник 10 кОм для 3 циклов).
- **Компромисс:** **15 циклов** → 389 kSPS (выше 100 kSPS с запасом).

---

### Пункт 7: TIM2 (APB1) vs TIM1 (APB2), TIMPRE

**Статус: подтверждено.** TIM1 и TIM2 на разных шинах с разными clock.

- **Заголовки:** `stm32f429xx.h` строки 11617–11619: `RCC_DCKCFGR_TIMPRE` — бит 24.
- **Значение TIMPRE:**
  - `TIMPRE = 0` (по умолчанию): TIM clock = PCLKx.
  - `TIMPRE = 1`: TIM clock = 2 × PCLKx (если APB prescaler ≠ 1).
- **В проекте:**
  - `RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4` → PCLK1 = 168 / 4 = **42 МГц**.
  - `RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV8` → PCLK2 = 168 / 8 = **21 МГц**.
  - **TIMPRE не установлен** (по умолчанию 0).
  - **TIM1 (APB2):** clock = **21 МГц**.
  - **TIM2 (APB1):** clock = **42 МГц**.
- **Влияние на план:** Для 100 kSPS с TIM2: Period = `(42 000 000 / 100 000) - 1 = 419`.

---

### Пункт 8: PA0 подключён к пользовательской кнопке

**Статус: подтверждено.** PA0 = B1 [Blue PushButton].

- **oscill.ioc:** `PA0/WKUP.GPIO_Label = B1 [Blue PushButton]`, `PA0/WKUP.Signal = GPXTI0`.
- **main.h:** `B1_Pin GPIO_PIN_0`.
- **PA0 занят** как GPIO_EXTI0 (прерывание по фронту кнопки).
- **Влияние на план:** ADC1_IN0 (PA0) **недоступен**. Использовать **ADC1_IN5 (PA5)** — свободен.

---

### Пункт 9: DMA2D R2M для заливки цветом

**Статус: подтверждено.** Для заливки постоянным цветом нужен режим **R2M** (Register to Memory).

- **Заголовки:** `stm32f4xx_hal_dma2d.h` строки 192–194, 577–578:
  ```c
  #define DMA2D_M2M                   0x00000000U  // Memory to Memory
  #define DMA2D_M2M_PFC               DMA2D_CR_MODE_0  // M2M с формат-конверт.
  #define DMA2D_M2M_BLEND             DMA2D_CR_MODE_1  // M2M с блендингом
  #define IS_DMA2D_MODE(MODE) ... || ((MODE) == DMA2D_R2M)
  ```
- **DMA2D_R2M** — режим записи из регистра цвета в память (без источника памяти).
- **Текущий проект:** `hdma2d.Init.Mode = DMA2D_M2M` — **не подходит** для заливки цветом.
- **Влияние на план:** Использовать `DMA2D_R2M` для заливки framebuffer.

---

### Пункт 10: ILI9341 инициализация и размещение framebuffer

**Статус: подтверждено.** ILI9341 **не инициализируется**, LCD подключён по **RGB интерфейсу** (LTDC напрямую).

- **Поиск:** `glob("ili*.c")` и `glob("lcd*.c")` — **0 файлов**. Нет драйверов ILI9341.
- **main.c:** `hltdc.Init.Backcolor` — LTDC настроен напрямую, без SPI/I2C инициализации LCD.
- **FMC:** `hsdram1.Instance = FMC_SDRAM_DEVICE` — SDRAM инициализируется.
- **Framebuffer адрес:** `pLayerCfg.FBStartAdress = 0xD0000000` (SDRAM Bank2).
- **RM0090 §39.4:** LTDC считывает framebuffer из внешней памяти по `FBStartAdress`.
- **Влияние на план:**
  - LCD не требует инициализации — режим RGB (всегда активен).
  - Framebuffer в **SDRAM @ 0xD0000000**.
  - SDRAM инициализирована **до** LTDC (порядок `MX_FMC_Init()` → `MX_LTDC_Init()` правильный).

---

## 2. Исправленный план реализации

### ЭТАП 1: Каркас — LTDC + SDRAM + framebuffer + DMA2D (R2M)

**Цель:** убедиться, что экран работает, framebuffer доступен, DMA2D заливает память.

**Действия:**
1. Выделить `uint16_t framebuffer[240 * 320]` в SDRAM (адрес `0xD0000000`).
2. Настроить DMA2D в режим **R2M**: `hdma2d.Init.Mode = DMA2D_R2M`.
3. Установить цвет через `HAL_DMA2D_ConfigLayer()` или `LL_DMA2D_ConfigOutputColor()`.
4. Запустить `HAL_DMA2D_Start()`.
5. Обновить `hltdc.LayerCfg[0].FBStartAdress = 0xD0000000`.
6. Добавить GPIO-метку кадра на **PG14 (LD4)** — мигать при обновлении.

**Ожидаемый результат:** LCD залит одним цветом, LD4 мигает.
**Проверка на плате:** визуально подтвердить цвет на экране, мигание LD4.

---

### ЭТАП 2: Исправление ADC1 + настройка TIM2 как триггера

**Цель:** ADC1 запускается от аппаратного события TIM2 TRGO, DMA1 circular захватывает данные.

**Действия:**
1. **Исправить ADC1:**
   - Канал → `ADC_CHANNEL_5` (PA5) — свободен.
   - `ExternalTrigConv` → `ADC_EXTERNALTRIGCONV_T2_TRGO`.
   - `ExternalTrigConvEdge` → `ADC_EXTERNALTRIGCONVEDGE_RISING`.
   - `DMAContinuousRequests` → `ENABLE`.
   - `SamplingTime` → `ADC_SAMPLETIME_15CYCLES` (компромисс).
2. **Настроить TIM2:**
   - TIM2 на APB1, clock = **42 МГц** (TIMPRE = 0, PCLK1 = 42 МГц).
   - Prescaler = 0, Period = **419** → f_TRGO = 42 MHz / 420 = **100 kHz**.
   - `MasterOutputTrigger` → `TIM_TRGO_UPDATE`.
   - ClockSource = INTERNAL.
3. **Добавить DMA1 для ADC1:**
   - **DMA1 Stream 0** (RM0090 §11.6.7).
   - Circular mode, peripheral (`&ADC1->DR`) → memory, 16-bit.
   - Half-transfer и transfer-complete прерывания.
4. **Буфер захвата:** `uint16_t adc_buffer[2048]` — в **внутренней SRAM** (@0x20000000), **НЕ в CCMRAM**.

**Ожидаемый результат:** ADC1 непрерывно конвертирует PA5, DMA1 circular заполняет `adc_buffer[2048]`.
**Проверка на плате:**
- Подать на PA5 постоянный уровень (0 В, 1.65 В, 3.0 В от генератора).
- Прочитать `adc_buffer` через ST-LINK или UART.
- Ожидается: среднее ~0, ~2130 (1.65 В), ~3900 (3.0 В).

---

### ЭТАП 3: Триггер и визуализация

**Цель:** отображение захваченных отсчётов на экране с триггером.

**Действия:**
1. ISR DMA half-transfer / transfer-complete: отметить новые данные, сбросить флаги overrun.
2. В `while(1)`:
   - Найти триггерную точку (пересечение порога).
   - Отрисовать waveform через DMA2D (M2M или M2M_PFC).
   - Нарисовать сетку, шкалу времени, шкалу напряжения.

**Ожидаемый результат:** на экране видна форма сигнала, триггер стабилизирует.
**Проверка на плате:** подать меандр 1 кГц, 0…3.0 В от генератора, визуальная проверка.

---

### ЭТАП 4: Сверка с эталоном

**Цель:** количественное сравнение с Hantek DSO2D15.

**Действия:**
1. Подать эталонный сигнал от Hantek.
2. Измерить фактическую частоту дискретизации (GPIO-метка на LD4).
3. Измерить амплитуду, положение фронтов, отклонения.
4. Записать в `VALIDATION_LOG.md`.

---

## 3. Сводка исправлений

| # | Пункт | Статус | Исправление |
|---|-------|--------|-------------|
| 1 | ADC1 и DMA | **Исправлено:** DMA1 Stream 0, не DMA2 | Использовать DMA1_Stream0 |
| 2 | TIM1_TRGO | **Подтверждено:** недоступен | Использовать **TIM2_TRGO** |
| 3 | LTDC_VSYNC | **Уточнено:** VSYNC на PA4, PA5 свободен | Использовать **ADC1_IN5 (PA5)** |
| 4 | ADC clock | **Пересчитано:** 10.5 МГц, не 84 МГц | Допустимо (≤ 36 МГц) |
| 5 | 192 цикла | **Подтверждено:** не существует | Использовать **15 циклов** |
| 6 | 100 kSPS | **Пересчитано:** 15 циклов → 389 kSPS макс | TIM2 Period = 419 для 100 kSPS |
| 7 | TIMPRE | **Подтверждено:** TIMPRE = 0, TIM2 clock = 42 МГц | Period = 419 (не 839) |
| 8 | PA0 | **Подтверждено:** PA0 = кнопка | Использовать **PA5 (ADC1_IN5)** |
| 9 | DMA2D R2M | **Подтверждено:** нужен R2M для заливки | `hdma2d.Init.Mode = DMA2D_R2M` |
| 10 | ILI9341 | **Подтверждено:** нет инициализации, LCD RGB | Framebuffer в SDRAM @ 0xD0000000 |

---

## 4. Критические проблемы исходной конфигурации

| # | Проблема | Влияние | Исправление |
|---|----------|---------|-------------|
| P1 | ADC программный запуск | Нарушает AGENTS.md | TIM2 TRGO |
| P2 | ADC_SAMPLETIME_3CYCLES | Искажения при входном импедансе > 100 Ом | 15 циклов |
| P3 | DMAContinuousRequests DISABLE | DMA перезапуск после каждого слова | ENABLE |
| P4 | Нет DMA-канала для ADC | Захват не работает | DMA1 Stream 0 |
| P5 | TIM1 TRGO = RESET | Нет триггера для ADC | TIM2 TRGO_UPDATE |
| P6 | DMA2D_M2M для заливки | Не работает для цвета | DMA2D_R2M |

---

## 5. Расчёт параметров

### TIM2 для 100 kSPS
- TIM2 clock = 42 МГц (APB1 = 42 МГц, TIMPRE = 0).
- Prescaler = 0.
- Period = `(42 000 000 / 100 000) - 1 = 419`.
- f_TRGO = 42 MHz / 420 = **100 kHz**.

### ADC время преобразования (15 циклов)
- ADC clock = 10.5 МГц.
- Время = `(15 + 12) / 10.5 MHz = 2.57 мкс`.
- Макс. частота = 1 / 2.57 мкс = **389 kSPS** (запас для 100 kSPS).

### Буферы
- `adc_buffer[2048]` — 4096 байт, внутренняя SRAM.
- `framebuffer[240 * 320]` — 153 600 байт, SDRAM @ 0xD0000000.
