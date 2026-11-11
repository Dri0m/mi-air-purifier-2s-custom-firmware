/* FreeRTOSConfig.h - Mi Air Purifier 2S custom firmware (88MW300).
 *
 * Kernel: amazon-freertos freertos_kernel V10.4.3 LTS Patch 2 (the one the
 * prebuilt WMSDK libs in WMSDK/mw320/bin/libs were built against).
 * Derived from vendors/marvell/boards/mw300_rd/aws_demos/config_files.
 *
 * Do not change options that alter kernel object layout or the API used by
 * the prebuilt libwifidriver/libwlcmgr (mutexes, recursive mutexes, counting
 * semaphores, timers, task notifications, dynamic allocation) without
 * re-checking those libs' undefined-symbol list.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

int board_cpu_freq(void);
void vAssertCalled(const char *pcFile, uint32_t ulLine);

#define configUSE_PREEMPTION                    1
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configCPU_CLOCK_HZ                      ((unsigned long)board_cpu_freq())
#define configTICK_RATE_HZ                      ((TickType_t)1000)
#define configMAX_PRIORITIES                    (7)
#define configMINIMAL_STACK_SIZE                ((unsigned short)256)  /* words */
#define configMAX_TASK_NAME_LEN                 (16)
#define configUSE_TRACE_FACILITY                1
#define configUSE_STATS_FORMATTING_FUNCTIONS    0
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_TASK_NOTIFICATIONS            1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES   1
#define configQUEUE_REGISTRY_SIZE               0
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configSUPPORT_STATIC_ALLOCATION         0
/* heap_marvell.c owns the heap (linker symbols _heap_start/_heap_end in
 * SRAM0, plus the SRAM1 bank added by os.c:add_heap_2_bank()). */
#define configAPPLICATION_ALLOCATED_HEAP        1
/* Do NOT define configTOTAL_HEAP_SIZE: heap_marvell.c declares a variable
 * with that name. */

/* Hooks implemented in platform/hooks.c. */
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_MALLOC_FAILED_HOOK            1

#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_CO_ROUTINES                   0
#define configMAX_CO_ROUTINE_PRIORITIES         (2)

#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            (configMINIMAL_STACK_SIZE * 2)

#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskCleanUpResources           0
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_pcTaskGetTaskName               1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTimerPendFunctionCall          1
#define INCLUDE_eTaskGetState                   1

/* Interrupt priorities: same values as the AFR mw300_rd demo config.
 * configPRIO_BITS is deliberately NOT defined (the AFR config doesn't
 * either): the FreeRTOS port then skips its "configured bits == bits the
 * NVIC implements" check (port.c:351).  With it defined as 4 that assert
 * fired in xPortStartScheduler() before main() on real hardware
 * (diag1, 2026-11-04: S90 ASSERT port.c v=351 after S30) -- the MW300 reads
 * back a different number of implemented priority bits.  The values below
 * are masked by the hardware, so they work for 3 or 4 implemented bits. */
#define configKERNEL_INTERRUPT_PRIORITY         0xf0
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    0xa0   /* priority 5 */

#define configASSERT(x)  do { if ((x) == 0) vAssertCalled(__FILE__, __LINE__); } while (0)

/* Map the port's handlers onto the names in the WMSDK vector table
 * (sdk/src/drivers/mw300/mdev_startup.c). */
#define xPortPendSVHandler                      PendSV_IRQHandler
#define xPortSysTickHandler                     SysTick_IRQHandler
#define vPortSVCHandler                         SVC_IRQHandler

#endif /* FREERTOS_CONFIG_H */
