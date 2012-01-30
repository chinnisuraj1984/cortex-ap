//============================================================================+
//
// $RCSfile: $
// $Revision: $
// $Date: $
// $Author: $
//
/// \brief main program
///
/// \todo
/// try to add message to log queue without blocking AHRS task, i.e. setting
/// max delay to 0 instead of portMAX_DELAY.
///
/// \todo
/// enale checking of stack overflow by setting configCHECK_FOR_STACK_OVERFLOW
/// to 2 in file freertosconfig.h.
///
/// \todo
/// have a look at :
/// http://sourceforge.net/projects/freertos/forums/forum/382005/topic/4033710
/// http://sourceforge.net/projects/freertos/forums/forum/382005/topic/4059693
/// in the former the problems was resolved by either increasing the stack size
/// or checking queue variable.
///
///
// Change: increased stack size of log task to 128, decreased satck size of 
//         remaining tasks, restored telemetry task.
//
//============================================================================*/

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include "stm32f10x.h"

#include "i2c_mems_driver.h"
#include "l3g4200d_driver.h"
#include "adxl345_driver.h"
#include "servodriver.h"
#include "ppmdriver.h"
#include "diskio.h"

#include "config.h"
#include "dcm.h"
#include "nav.h"
#include "telemetry.h"
#include "log.h"
#include "led.h"

/** @addtogroup cortex-ap
  * @{
  */

/** @addtogroup main
  * @{
  */

/*--------------------------------- Definitions ------------------------------*/

#ifdef VAR_STATIC
#undef VAR_STATIC
#endif
#define VAR_STATIC static
#ifdef VAR_GLOBAL
#undef VAR_GLOBAL
#endif
#define VAR_GLOBAL

/* Task priorities. */
#define mainAHRS_PRIORITY       ( tskIDLE_PRIORITY + 4 )
#define mainDISK_PRIORITY       ( tskIDLE_PRIORITY + 3 )
#define mainATTITUDE_PRIORITY   ( tskIDLE_PRIORITY + 3 )
#define mainLOG_PRIORITY        ( tskIDLE_PRIORITY + 2 )
#define mainTELEMETRY_PRIORITY  ( tskIDLE_PRIORITY + 2 )
#define mainNAVIGATION_PRIORITY ( tskIDLE_PRIORITY + 1 )

/*----------------------------------- Macros ---------------------------------*/

/*-------------------------------- Enumerations ------------------------------*/

/*----------------------------------- Types ----------------------------------*/

/*---------------------------------- Constants -------------------------------*/

VAR_STATIC const int16_t Sensor_Sign[6] = {
    -1,     // acceleration X, must be positive forward
     1,     // acceleration Y, must be positive rightward
     1,     // acceleration Z, must be positive downward
     1,	    // roll rate, must be positive when right wing lowers
    -1,     // pitch rate, must be positive when tail lowers
    -1      // yaw rate, must be positive when turning right
};

/*---------------------------------- Globals ---------------------------------*/

/*----------------------------------- Locals ---------------------------------*/

VAR_STATIC int16_t Aileron_Position = 1500;
VAR_STATIC int16_t Elevator_Position = 1500;
VAR_STATIC int16_t Message_Buffer[6];
VAR_STATIC uint8_t Sensor_Data[16];
VAR_STATIC int16_t Sensor_Offset[6] = {0, 0, 0, 0, 0, 0};

/*--------------------------------- Prototypes -------------------------------*/

void RCC_Configuration(void);
void GPIO_Configuration(void);
void AHRS_Task(void *pvParameters);
void Attitude_Control_Task(void *pvParameters);

/*--------------------------------- Functions --------------------------------*/

///----------------------------------------------------------------------------
///
/// \brief   hook for stack overflow check
/// \return  -
/// \remarks -
///
///----------------------------------------------------------------------------
void vApplicationStackOverflowHook( xTaskHandle *pxTask, int8_t *pcTaskName ) {
    ( void ) pxTask;
    ( void ) pcTaskName;
    for ( ;; );
}

///----------------------------------------------------------------------------
///
/// \brief   main
/// \return  -
/// \remarks -
///
///----------------------------------------------------------------------------
int main(void)
{
  /*!< At this stage the microcontroller clock setting is already configured,
       this is done through SystemInit() function which is called from startup
       file (startup_stm32f10x_xx.s) before to branch to application main.
       To reconfigure the default setting of SystemInit() function, refer to
       system_stm32f10x.c file */

  // Configure priority group
  NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);

  // System Clocks Configuration
  RCC_Configuration();

  // GPIO Configuration
  GPIO_Configuration();

  // Initialize PWM timers as servo outputs
  Servo_Init();

  // Initialize capture timers as RRC input
  PPM_Init();

  // I2C peripheral initialization
  I2C_MEMS_Init();

  Telemetry_Init();

  xTelemetry_Queue = xQueueCreate( 3, sizeof( xTelemetry_Message ) );
  while ( xTelemetry_Queue == 0 ) {
  }

  xLog_Queue = xQueueCreate( 3, sizeof( xLog_Message ) );
  while ( xLog_Queue == 0 ) {
  }

  xGps_Queue = xQueueCreate( 3, sizeof( xGps_Message ) );
  while ( xGps_Queue == 0 ) {
  }

  xTaskCreate(AHRS_Task, ( signed portCHAR * ) "AHRS", 64, NULL, mainAHRS_PRIORITY, NULL);
  xTaskCreate(Attitude_Control_Task, ( signed portCHAR * ) "Attitude", 64, NULL, mainATTITUDE_PRIORITY, NULL);
  xTaskCreate(disk_timerproc, ( signed portCHAR * ) "Disk", 32, NULL, mainDISK_PRIORITY, NULL);
//  xTaskCreate(Navigation_Task, ( signed portCHAR * ) "Navigation", 64, NULL, mainNAVIGATION_PRIORITY, NULL);
  xTaskCreate(Telemetry_Task, ( signed portCHAR * ) "Telemetry", 64, NULL, mainTELEMETRY_PRIORITY, NULL);
  xTaskCreate(Log_Task, ( signed portCHAR * ) "Log", 128, NULL, mainLOG_PRIORITY, NULL);

  vTaskStartScheduler();

  while (1) {
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

  /* TIM2 and TIM3 clock enable */
  RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2 | RCC_APB1Periph_TIM3, ENABLE);

  /* GPIOA and GPIOB clock enable */
  RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB |
                         RCC_APB2Periph_GPIOC | RCC_APB2Periph_AFIO |
                         RCC_APB2Periph_USART1, ENABLE);
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
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
  GPIO_InitStructure.GPIO_Pin = GPIO_Pin_9 ;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_Init(GPIOA, &GPIO_InitStructure);

  // USART 1 RX pin as input floating (10)
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
  GPIO_InitStructure.GPIO_Pin = GPIO_Pin_10;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_Init(GPIOA, &GPIO_InitStructure);

  /* GPIOB Configuration */

  // TIM3 Channel 3, 4 as alternate function push-pull */
  GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_1;
  GPIO_Init(GPIOB, &GPIO_InitStructure);

  /* GPIOC Configuration */

  // LED pins as push pull outputs
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
  GPIO_InitStructure.GPIO_Pin = GPIO_Pin_8 | GPIO_Pin_9;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_Init(GPIOC, &GPIO_InitStructure);

}


///----------------------------------------------------------------------------
///
/// \brief   Attitude and heading computation.
/// \return  -
/// \remarks -
///
///----------------------------------------------------------------------------
void AHRS_Task(void *pvParameters)
{
    uint8_t i = 0, j = 0, ucBlink = 0;
    int16_t * pSensor;
    xLog_Message message;
    portTickType Last_Wake_Time;

    Last_Wake_Time = xTaskGetTickCount();

    /* Task specific initializations */
    L3G4200_Init();                                 // init L3G4200 gyro
    ADXL345_Init();                                 // init ADXL345 accelerometer

    /* Wait until aircraft settles */
    LEDOn(BLUE);
    vTaskDelayUntil(&Last_Wake_Time, configTICK_RATE_HZ * 5);
    LEDOff(BLUE);

    /* Compute sensor offsets */
    for (i = 0; i < 64; i++) {
        vTaskDelayUntil(&Last_Wake_Time, configTICK_RATE_HZ / SAMPLES_PER_SECOND);
        GetAccelRaw(Sensor_Data);                   // accelerometer
        GetAngRateRaw((uint8_t *)&Sensor_Data[6]);  // gyroscope
        pSensor = (int16_t *)Sensor_Data;
        for (j = 0; j < 6; j++) {                   // accumulate
            Sensor_Offset[j] += *pSensor++;
        }
    }
    for (j = 0; j < 6; j++) {                       // average
        Sensor_Offset[j] = Sensor_Offset[j] / 64;
    }

    /* Compute attitude and heading */
    while (1) {
        vTaskDelayUntil(&Last_Wake_Time, configTICK_RATE_HZ / SAMPLES_PER_SECOND);

        if (++ucBlink == 10) {                      // blink led every 10 cycles
           ucBlink = 0;
           LEDToggle(BLUE);
        }

        GetAccelRaw(Sensor_Data);                   // acceleration
        GetAngRateRaw((uint8_t *)&Sensor_Data[6]);  // rotation

        pSensor = (int16_t *)Sensor_Data;
        for (j = 0; j < 6; j++) {
            *pSensor = *pSensor - Sensor_Offset[j]; // strip offset
            *pSensor = *pSensor * Sensor_Sign[j];   // correct sign
            if (j == 2) {
               *pSensor += (int16_t)GRAVITY;        // add gravity
            }
            pSensor++;
        }

        MatrixUpdate((int16_t *)Sensor_Data);       // compute DCM
        CompensateDrift();                          // compensate
        Normalize();                                // normalize DCM

        /* Extract attitude from DCM matrix */

        Message_Buffer[0] = (int16_t)(DCM_Matrix[2][0] * 32767.0f);
        Message_Buffer[1] = (int16_t)(DCM_Matrix[2][1] * 32767.0f);
        Message_Buffer[2] = (int16_t)(DCM_Matrix[1][1] * 32767.0f);
        Message_Buffer[3] = (int16_t)PPMGetChannel(AILERON_CHANNEL);
        Message_Buffer[4] = (int16_t)PPMGetChannel(ELEVATOR_CHANNEL);

        /* Save attitude to log file */

        message.ucLength = 5;                       // message length
        message.pcData = (uint16_t *)Message_Buffer;// message content
        xQueueSend( xLog_Queue, &message, 0 );
        xQueueSend( xTelemetry_Queue, &message, 0 );
    }
}


///----------------------------------------------------------------------------
///
/// \brief   Attitude control.
/// \return  -
/// \remarks -
///
///----------------------------------------------------------------------------
void Attitude_Control_Task(void *pvParameters)
{
    uint8_t ucMode, ucBlink = 0;
    portTickType Last_Wake_Time;

    Last_Wake_Time = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil(&Last_Wake_Time, configTICK_RATE_HZ / SAMPLES_PER_SECOND);

        Aileron_Position = SERVO_NEUTRAL + (int16_t)(DCM_Matrix[2][1] * 500.0f);
        Elevator_Position = SERVO_NEUTRAL + (int16_t)(DCM_Matrix[2][0] * 500.0f);

        ucMode = PPMGetMode();

        switch (ucMode) {
           case MODE_RTL:
              LEDOn(RED);
           break;
           case MODE_STABILIZE:
              Aileron_Position -= SERVO_NEUTRAL;
              Aileron_Position += PPMGetChannel(AILERON_CHANNEL);
              Elevator_Position -= SERVO_NEUTRAL;
              Elevator_Position += PPMGetChannel(ELEVATOR_CHANNEL);
              if (++ucBlink >= 5) {
                 ucBlink = 0;
                 LEDToggle(RED);
               }
           break;
           case MODE_AUTO:
              if (++ucBlink == 10) {
                ucBlink = 0;
                LEDToggle(RED);
           }
           break;
           default:
              LEDOff(RED);
              Aileron_Position = PPMGetChannel(AILERON_CHANNEL);
              Elevator_Position = PPMGetChannel(ELEVATOR_CHANNEL);
           break;
        }

        Servo_Set(SERVO_AILERON, Aileron_Position);
        Servo_Set(SERVO_ELEVATOR, Elevator_Position);
    }
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
