/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usb_device.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "usbd_cdc_if.h"
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
// PC13 LED control (active low: 0 = ON, 1 = OFF)
static inline void LED_On(void)  { GPIOC->BSRR = (uint32_t)GPIO_PIN_13 << 16U; }
static inline void LED_Off(void) { GPIOC->BSRR = (uint32_t)GPIO_PIN_13; }
static inline void LED_Toggle(void) { GPIOC->ODR ^= GPIO_PIN_13; }
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
ADC_HandleTypeDef hadc2;
DMA_HandleTypeDef hdma_adc1;

/* USER CODE BEGIN PV */
#define CIRCULAR_BUFFER_SIZE 120  // 120 x uint32_t = 240 samples (interleaved ADC1/ADC2)
#define TOTAL_SAMPLES        240
#define PRE_TRIGGER_SAMPLES  80   // 10 us @ 8 MSPS
#define POST_TRIGGER_SAMPLES 160  // 20 us @ 8 MSPS

// Circular DMA buffer: lower 16-bits = ADC1, upper 16-bits = ADC2
uint32_t adc_raw_buffer[CIRCULAR_BUFFER_SIZE];

// Linear reconstructed timeline
uint16_t decoded_signal[TOTAL_SAMPLES];

// Volatile flags for ISR <-> main communication
volatile uint8_t  data_ready              = 0;
volatile uint8_t  trigger_detected        = 0;
volatile uint16_t trigger_pos             = 0;
volatile uint8_t  post_trigger_dma_events = 0;
volatile uint8_t  uart_busy               = 0;  // Guard: ignore EXTI during UART
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_ADC1_Init(void);
static void MX_ADC2_Init(void);
/* USER CODE BEGIN PFP */
static void Unpack_Ultrasound_Timeline(uint16_t pos);
static void Send_Signal_Over_UART(void);
static inline void Check_PostTrigger_Completion(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/**
  * @brief  EXTI rising-edge callback (trigger event at time t)
  *         Captures the current DMA write position immediately.
  *         Runs in ISR context — must be minimal and fast.
  */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == GPIO_PIN_4)
  {
    // Ignore trigger during UART transmission (echo rejection)
    if (uart_busy) return;
    // Ignore subsequent edges after first trigger (re-entry guard)
    if (trigger_detected) return;
    
    // Read remaining DMA transfers to pinpoint circular index
    uint32_t remaining = __HAL_DMA_GET_COUNTER(&hdma_adc1);
    // Bounds guard: protect against NDTR reload transient (remaining==0)
    if (remaining == 0 || remaining > CIRCULAR_BUFFER_SIZE)
      remaining = 1;
    trigger_pos = (uint16_t)(CIRCULAR_BUFFER_SIZE - remaining);
    
    // Reset post-trigger event counter
    post_trigger_dma_events = 0;
    
    // Signal that trigger occurred
    trigger_detected = 1;
  }
}

/**
  * @brief  Shared post-trigger completion logic for HT/TC DMA callbacks.
  *         Need 2 events (HT + TC) to guarantee >= 80 post-trigger words.
  */
static inline void Check_PostTrigger_Completion(void)
{
  if (trigger_detected)
  {
    post_trigger_dma_events++;
    if (post_trigger_dma_events >= 2)
    {
      HAL_ADCEx_MultiModeStop_DMA(&hadc1);
      data_ready = 1;
      trigger_detected = 0;
    }
  }
}

/**
  * @brief  DMA Half-Transfer callback (DMA has written 60 words)
  */
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef* hadc)
{
  if (hadc->Instance == ADC1)
    Check_PostTrigger_Completion();
}

/**
  * @brief  DMA Transfer-Complete callback (DMA has wrapped around 120 words)
  */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* hadc)
{
  if (hadc->Instance == ADC1)
    Check_PostTrigger_Completion();
}

/**
  * @brief  Unpack circular DMA buffer into linear chronological timeline.
  *         Uses a snapshot of trigger_pos to avoid ISR corruption.
  *         ADC1 -> even indices, ADC2 -> odd indices.
  */
static void Unpack_Ultrasound_Timeline(uint16_t pos)
{
  uint16_t start_idx = (pos + CIRCULAR_BUFFER_SIZE - (PRE_TRIGGER_SAMPLES / 2))
                       % CIRCULAR_BUFFER_SIZE;
  
  uint16_t timeline_idx = 0;
  
  for (uint16_t i = 0; i < CIRCULAR_BUFFER_SIZE; i++)
  {
    uint16_t buf_idx = (start_idx + i) % CIRCULAR_BUFFER_SIZE;
    uint32_t word = adc_raw_buffer[buf_idx];
    
    decoded_signal[timeline_idx++] = (uint16_t)(word & 0xFFFF);          // ADC1
    decoded_signal[timeline_idx++] = (uint16_t)((word >> 16) & 0xFFFF);  // ADC2
  }
}

/**
  * @brief  Transmit all decoded ADC samples over USB CDC as decimal strings.
  *         Each sample on its own line, terminated by '\n'.
  *         Builds full output buffer then sends in one CDC_Transmit_FS call.
  */
static void Send_Signal_Over_UART(void)
{
  // Max: 240 samples x 6 chars ("65535\n") = 1440 bytes
  char buf[TOTAL_SAMPLES * 6];
  uint16_t pos = 0;

  for (uint16_t i = 0; i < TOTAL_SAMPLES; i++)
  {
    uint16_t val = decoded_signal[i];

    if (val == 0)
    {
      buf[pos++] = '0';
    }
    else
    {
      char tmp[5];
      uint8_t tlen = 0;
      while (val > 0)
      {
        tmp[tlen++] = '0' + (uint8_t)(val % 10);
        val /= 10;
      }
      for (uint8_t j = 0; j < tlen; j++)
        buf[pos++] = tmp[tlen - 1 - j];
    }
    buf[pos++] = '\n';
  }

  // Send entire buffer via USB CDC (non-blocking, queued to endpoint)
  while (CDC_Transmit_FS((uint8_t*)buf, pos) != USBD_OK)
  {
    HAL_Delay(1);  // Retry if USB endpoint buffer is full
  }
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_ADC2_Init();
  MX_USB_Device_Init();
  /* USER CODE BEGIN 2 */
  // Start the interleaved master/slave DMA pipeline in circular mode
  if (HAL_ADCEx_MultiModeStart_DMA(&hadc1, (uint32_t*)adc_raw_buffer,
      CIRCULAR_BUFFER_SIZE) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    if (data_ready)
    {
      // 1. Snapshot trigger_pos and lock immediately (closes atomicity gap)
      uint16_t snap_pos = trigger_pos;
      trigger_detected = 0;
      data_ready = 0;
      uart_busy = 1;
      
      // 2. Reconstruct circular buffer using snapshot (ISR-safe)
      Unpack_Ultrasound_Timeline(snap_pos);
      
      // 3. Transmit decoded signal over USB CDC
      Send_Signal_Over_UART();
      
      // 4. Unlock: ready for next trigger
      uart_busy = 0;
      
      // 5. Restart DMA and wait for next ultrasound arrival
      if (HAL_ADCEx_MultiModeStart_DMA(&hadc1, (uint32_t*)adc_raw_buffer,
          CIRCULAR_BUFFER_SIZE) != HAL_OK)
      {
        Error_Handler();
      }
    }
    /* USER CODE END 3 */
  }
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_HSI48;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV1;
  RCC_OscInitStruct.PLL.PLLN = 15;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV4;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_MultiModeTypeDef multimode = {0};
  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Common config
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV1;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.GainCompensation = 0;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.ContinuousConvMode = ENABLE;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.DMAContinuousRequests = ENABLE;
  hadc1.Init.Overrun = ADC_OVR_DATA_PRESERVED;
  hadc1.Init.OversamplingMode = DISABLE;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure the ADC multi-mode
  */
  multimode.Mode = ADC_DUALMODE_INTERL;
  multimode.DMAAccessMode = ADC_DMAACCESSMODE_12_10_BITS;
  multimode.TwoSamplingDelay = ADC_TWOSAMPLINGDELAY_4CYCLES;
  if (HAL_ADCEx_MultiModeConfigChannel(&hadc1, &multimode) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_3;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_2CYCLES_5;
  sConfig.SingleDiff = ADC_DIFFERENTIAL_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief ADC2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC2_Init(void)
{

  /* USER CODE BEGIN ADC2_Init 0 */

  /* USER CODE END ADC2_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC2_Init 1 */

  /* USER CODE END ADC2_Init 1 */

  /** Common config
  */
  hadc2.Instance = ADC2;
  hadc2.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV1;
  hadc2.Init.Resolution = ADC_RESOLUTION_12B;
  hadc2.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc2.Init.GainCompensation = 0;
  hadc2.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc2.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc2.Init.LowPowerAutoWait = DISABLE;
  hadc2.Init.ContinuousConvMode = ENABLE;
  hadc2.Init.NbrOfConversion = 1;
  hadc2.Init.DiscontinuousConvMode = DISABLE;
  hadc2.Init.DMAContinuousRequests = DISABLE;
  hadc2.Init.Overrun = ADC_OVR_DATA_PRESERVED;
  hadc2.Init.OversamplingMode = DISABLE;
  if (HAL_ADC_Init(&hadc2) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_3;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_2CYCLES_5;
  sConfig.SingleDiff = ADC_DIFFERENTIAL_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0;
  if (HAL_ADC_ConfigChannel(&hadc2, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC2_Init 2 */

  /* USER CODE END ADC2_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMAMUX1_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Channel1_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /*Configure GPIO pin : PA4 */
  GPIO_InitStruct.Pin = GPIO_PIN_4;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI4_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI4_IRQn);

  /* USER CODE BEGIN MX_GPIO_Init_2 */
    // PC13 LED — Open-Drain output (active low)
    __HAL_RCC_GPIOC_CLK_ENABLE();
  
    GPIO_InitStruct.Pin   = GPIO_PIN_13;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_OD;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);
  
    LED_Off();  // Start with LED off
  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
