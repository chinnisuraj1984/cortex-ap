//============================================================================+
//
// $HeadURL: $
// $Revision: $
// $Date:  $
// $Author: $
//
/// \brief main program
///
/// \file
///
/// \todo
/// 1) Move task definitions in the main.c file, export relevant functions
/// from specific modules and call them from inside task, this should improve
/// testability.
///
/// \todo
/// 2) Use only one data structure for SD file read/write, add a semaphore
/// to manage multiple accesses, this will reduce RAM usage by 512 bytes.
///
// Change: increased stack sizes to 128 bytes
//
//============================================================================*/

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "stm32f10x_rcc.h"
#include "stm32f10x_wwdg.h"
#include "misc.h"

#include "i2c_mems_driver.h"
#include "l3g4200d_driver.h"
#include "adxl345_driver.h"
#include "servodriver.h"
#include "ppmdriver.h"
#include "usart1driver.h"
#include "diskio.h"
#include "ff.h"

#include "config.h"
#include "simulator.h"
#include "mav_telemetry.h"
#include "attitude.h"
#include "log.h"
#include "led.h"
#include "nav.h"
#include "globals.h"

/** @addtogroup cortex_ap
  * @{
  */

/** @addtogroup main
  * @{
  */

/*--------------------------------- Definitions ------------------------------*/

#ifndef VAR_STATIC
#define VAR_STATIC static
#endif
#ifdef VAR_GLOBAL
#undef VAR_GLOBAL
#endif
#define VAR_GLOBAL

/* Task priorities. */
#define mainAHRS_PRIORITY   ( tskIDLE_PRIORITY + 3 )    ///< AHRS priority
#define mainDISK_PRIORITY   ( tskIDLE_PRIORITY + 3 )    ///< SD file priority
#define mainLOG_PRIORITY    ( tskIDLE_PRIORITY + 2 )    ///< log priority
#define mainNAV_PRIORITY    ( tskIDLE_PRIORITY + 2 )    ///< navigation priority
#define mainTEL_PRIORITY    ( tskIDLE_PRIORITY + 2 )    ///< telemetry priority

/* Task frequencies. */
#define TELEMETRY_FREQUENCY 50  //!< frequency of telemetry task (50 Hz)

/* Task delays. */
#define TELEMETRY_DELAY     (configTICK_RATE_HZ / TELEMETRY_FREQUENCY) //!< delay for telemetry task

/*----------------------------------- Macros ---------------------------------*/

/*-------------------------------- Enumerations ------------------------------*/

/*----------------------------------- Types ----------------------------------*/

/*---------------------------------- Constants -------------------------------*/

/*---------------------------------- Globals ---------------------------------*/

VAR_GLOBAL FATFS st_Fat;    //!< FAT object
VAR_GLOBAL FIL st_File;     //!< file object
VAR_GLOBAL bool b_FS_Ok;    //!< file status

/*----------------------------------- Locals ---------------------------------*/

VAR_STATIC bool b_watchdog_reset;

/*--------------------------------- Prototypes -------------------------------*/

void RCC_Configuration(void);
void GPIO_Configuration(void);
void WWDG_Configuration(void);

/*--------------------------------- Functions --------------------------------*/

///----------------------------------------------------------------------------
///
/// \brief   hook for stack overflow check
/// \return  -
/// \remarks -
///
///----------------------------------------------------------------------------
#if(0)
void vApplicationStackOverflowHook( xTaskHandle *pxTask, int8_t *pcTaskName ) {
    ( void ) pxTask;
    ( void ) pcTaskName;
    for (;;) {
    }
}
#endif

///----------------------------------------------------------------------------
///
/// \brief  telemetry task
/// \return  -
/// \remarks -
///
///----------------------------------------------------------------------------
void Telemetry_Task( void *pvParameters ) {

#if (SIMULATOR != SIM_NONE)

    uint8_t ucCycles = 0;
    portTickType Last_Wake_Time;                //
    Last_Wake_Time = xTaskGetTickCount();       //

    while (TRUE) {
        vTaskDelayUntil(&Last_Wake_Time, TELEMETRY_DELAY);
        Simulator_Send_Controls();              // update simulator controls
		Simulator_Parse();                      // parse simulator data
        switch (++ucCycles) {
            case 8:
            case 16:
            case 24:
            case 32:
                Simulator_Send_DCM();           // send attitude
                break;

            case 40:
                ucCycles = 0;                   // reset cycle counter
                Simulator_Send_Waypoint();      // send waypoint information
                break;
        }
    }

#elif defined TELEMETRY_MAVLINK

    uint8_t ucCycles = 0;
    portTickType Last_Wake_Time;                //
    Last_Wake_Time = xTaskGetTickCount();       //
//    global_data_reset_param_defaults();         // Load default parameters as fallback

    (void)pvParameters;

    for (;;)  {
        vTaskDelayUntil(&Last_Wake_Time, TELEMETRY_DELAY);  // Use any wait function, better not use sleep
        Mavlink_Receive();                      // Process parameter request, if occured
        Mavlink_Stream_Send();                  // Send data streams
        Mavlink_Queued_Send(ucCycles);          // Send parameters at 10 Hz, if previously requested
        if (++ucCycles > 200) {
            ucCycles = 0;
        }
    }

#elif defined TELEMETRY_MULTIWII

    for (;;) {
        MWI_Receive();          				//
    }

#endif

}

///----------------------------------------------------------------------------
///
/// \brief   main
/// \return  -
/// \remarks -
///
///----------------------------------------------------------------------------
int32_t main(void) {

  /* At this stage the microcontroller clock setting is already configured,
     this is done through SystemInit() function which is called from startup
     file (startup_stm32f10x_xx.s) before to branch to application main.
     To reconfigure the default setting of SystemInit() function, refer to
     system_stm32f10x.c file */

  if (RCC_GetFlagStatus(RCC_FLAG_WWDGRST) != RESET) {   /* the system has resumed from WWDG reset */
    b_watchdog_reset = TRUE;                            /* */
    RCC_ClearFlag();                                    /* clear flags of reset source */
  } else {
    b_watchdog_reset = FALSE;                           /* */
  }

  NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);   // Configure priority group
  RCC_Configuration();                              // Configure System Clocks
  GPIO_Configuration();                             // Configure GPIO
  WWDG_Configuration();                             // Configure watchdog

  USART1_Init();              						// Initialize USART1 for telemetry
  Servo_Init();                                     // Initialize PWM timers as servo outputs
  PPM_Init();                                       // Initialize capture timers as RRC input
  I2C_MEMS_Init();                                  // Initialize I2C peripheral

/*
  xTelemetry_Queue = xQueueCreate( 3, sizeof( telStruct_Message ) );
  while ( xTelemetry_Queue == 0 ) {                 // Halt if queue wasn't created
  }

  xLog_Queue = xQueueCreate( 3, sizeof( xLog_Message ) );
  while ( xLog_Queue == 0 ) {                       // Halt if queue wasn't created
  }
*/
  if (FR_OK == f_mount(0, &st_Fat)) {               // Mount file system
    b_FS_Ok = TRUE;                                 //
  } else {
    b_FS_Ok = FALSE;                                //
  }

  if (b_watchdog_reset) {
     LEDOn(RED);
  } else {
     LEDOff(RED);
  }

  (void)xTaskCreate(Attitude_Task, (signed portCHAR *) "Attitude", 128, NULL, mainAHRS_PRIORITY, NULL);
  (void)xTaskCreate(disk_timerproc, (signed portCHAR *) "Disk", 128, NULL, mainDISK_PRIORITY, NULL);
  (void)xTaskCreate(Navigation_Task, (signed portCHAR *) "Navigation", 128, NULL, mainNAV_PRIORITY, NULL);
  (void)xTaskCreate(Telemetry_Task, (signed portCHAR *) "Telemetry", 128, NULL, mainTEL_PRIORITY, NULL);
  (void)xTaskCreate(Log_Task, (signed portCHAR *) "Log", 128, NULL, mainLOG_PRIORITY, NULL);

  vTaskStartScheduler();                            // Start scheduler

  for (;;) {
  }
}

///----------------------------------------------------------------------------
///
/// \brief   Configure the different system clocks.
/// \return  -
/// \remarks -
///
///----------------------------------------------------------------------------
void RCC_Configuration(void)
{
  /* HCLK = SYSCLK */
  RCC_HCLKConfig(RCC_SYSCLK_Div1);

  /* PCLK1 = HCLK/2 */
  RCC_PCLK1Config(RCC_HCLK_Div2);

  /* PCLK2 = HCLK */
  RCC_PCLK2Config(RCC_HCLK_Div1);

  /* TIM2, TIM3 USART2 clock enable */
  RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2 |   // Used for PPM signal capture
                         RCC_APB1Periph_TIM3 |   // Used for servo signal PWM
                         RCC_APB1Periph_USART2 | // Used for GPS communication
                         RCC_APB1Periph_WWDG,    // Used for watchdog
                         ENABLE);

  /* GPIOA, GPIOB, GPIOC, USART1 clock enable */
  RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | // TIM2, TIM3, USART1, USART2
                         RCC_APB2Periph_GPIOB | // TIM3
                         RCC_APB2Periph_GPIOC | // LED
                         RCC_APB2Periph_AFIO  | //
                         RCC_APB2Periph_USART1, // Used for telemetry
                         ENABLE);

  /* DMA 1 clock enable */
  RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

}

///----------------------------------------------------------------------------
///
/// \brief   Configure pins.
/// \return  -
/// \remarks -
///
///----------------------------------------------------------------------------
void GPIO_Configuration(void)
{
  GPIO_InitTypeDef GPIO_InitStructure;

  /* GPIOA Configuration */

  // TIM2 Channel 2 as input pull down (1)
  GPIO_InitStructure.GPIO_Pin = GPIO_Pin_1;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPD;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_Init(GPIOA, &GPIO_InitStructure);

  // TIM3 Channel 1, 2 as alternate function push-pull (6, 7)
  GPIO_InitStructure.GPIO_Pin = GPIO_Pin_6 | GPIO_Pin_7;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_Init(GPIOA, &GPIO_InitStructure);

  // USART 1 TX pin as alternate function push pull (9)
  GPIO_InitStructure.GPIO_Pin = GPIO_Pin_9;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_Init(GPIOA, &GPIO_InitStructure);

  // USART 1 RX pin as input floating (10)
  GPIO_InitStructure.GPIO_Pin = GPIO_Pin_10;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_Init(GPIOA, &GPIO_InitStructure);

  // USART 2 TX pin as alternate function push pull (2)
  GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_Init(GPIOA, &GPIO_InitStructure);

  // USART 2 RX pin as input floating (3)
  GPIO_InitStructure.GPIO_Pin = GPIO_Pin_3;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_Init(GPIOA, &GPIO_InitStructure);

  /* GPIOB Configuration */

  // TIM3 Channel 3, 4 as alternate function push-pull */
  GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_1;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_Init(GPIOB, &GPIO_InitStructure);

  /* GPIOC Configuration */

  // LED pins as push pull outputs
  GPIO_InitStructure.GPIO_Pin = GPIO_Pin_8 | GPIO_Pin_9;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_Init(GPIOC, &GPIO_InitStructure);
}

///----------------------------------------------------------------------------
///
/// \brief   Configuration of window watchdog
/// \param   -
/// \return  -
/// \remarks The WWDG clock is:
///
///          PCLK1 / 4096 / PRESCALER = 24MHz / 4096 / 4 = 1465 Hz (~682 us).
///
///          WWDG counter should be refreshed only when the counter is below
///          window value and above 64, otherwise a reset will be generated.
///
///          WWDG is enabled setting counter value to 127 (fixed).
///
///          The WWDG timeout is:
///
///          ~682 us * 64 = 43.69 ms
///
///          The refresh window is:
///
///          ~682us * (127 - 100) = 18.41ms < window < ~682us * 64 = 43.69ms
///
///----------------------------------------------------------------------------
void WWDG_Configuration(void)
{
  WWDG_SetPrescaler(WWDG_Prescaler_4);    /* Set WWDG clock */
  WWDG_SetWindowValue(100);               /* Set Window value to 100 */
}

#ifdef  USE_FULL_ASSERT
///----------------------------------------------------------------------------
///
/// \brief   Reports the name of the source file and the source line number
///          where the assert_param error has occurred.
/// \param   file: pointer to the source file name
/// \param   line: assert_param error line source number
/// \return  -
/// \remarks -
///
///----------------------------------------------------------------------------
void assert_failed(uint8_t* file, uint32_t line)
{
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */

  /* Infinite loop */
  while (1)
  {
  }
}
#endif

/**
  * @}
  */

/**
  * @}
  */

/*****END OF FILE****/
