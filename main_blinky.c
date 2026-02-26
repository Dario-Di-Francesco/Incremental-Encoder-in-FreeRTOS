/* Standard includes. */
#include <stdio.h>
#include <stdlib.h>

#include <sys/time.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>

/* Kernel includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "semphr.h"

/*
 * ===========================
 * Incremental Encoder (FreeRTOS)
 * ===========================
 *
 * This file implements a FreeRTOS-based solution for an incremental encoder exercise.
 * The application creates 5 tasks:
 *
 *  1) enc         : encoder emulator (generates slit and home_slit signals)
 *  2) rt_task1    : counts rising edges of slit (position/count)
 *  3) rt_task2    : measures time between two consecutive home events (one revolution period)
 *  4) scope       : "polling server" / UI task that prints count and RPM
 *  5) diagnostic  : computes and prints average slack time (optional/diagnostic)
 *
 * Synchronization:
 *  - Shared variables are protected using FreeRTOS mutexes (xSemaphoreCreateMutex()).
 *
 * Timing:
 *  - Periodic tasks are implemented with vTaskDelayUntil() (tick-based periodic execution).
 */

/* Total number of tasks */
#define TASK_NUM 5

/* Encoder emulator base period (ms) */
#define ENCODER_PERIOD_MS 5

/* Index of the printing task (Polling Server) - not used in this code */
#define PRINT_TASK_POSITION 0

/* Milliseconds per second */
#define MS_PER_SEC 1000

/* Task priorities (relative to idle priority) */
#define ENCODER_TASK_PRIORITY       ( tskIDLE_PRIORITY + 5 )
#define RT1_PRIORITY                ( tskIDLE_PRIORITY + 6 )
#define RT2_PRIORITY                ( tskIDLE_PRIORITY + 6 )
#define SCOPE_TASK_PRIORITY         ( tskIDLE_PRIORITY + 2 )
#define DIAGNOSTIC_TASK_PRIORITY    ( tskIDLE_PRIORITY + 4 )

/*-----------------------------------------------------------*/

/* Task prototypes */
static void enc(void* pvParameters);
static void rt_task1(void* pvParameters);     /* RT1 */
static void rt_task2(void* pvParameters);     /* RT2 */
static void scope(void* pvParameters);
static void diagnostic(void* pvParameters);

/*-----------------------------------------------------------*/

/* Periods array (declared but not used in this snippet) */
static int periods[TASK_NUM];

/* Counter array (declared but not used in this snippet) */
static int counter[TASK_NUM];

/*-----------------------------------------------------------*/
/* Shared structures + mutexes */

/*
 * Encoder shared data:
 * - slit:      square wave (0/1)
 * - home_slit: home pulse (1 when at "home", 0 otherwise)
 * - lock:      mutex for protecting concurrent access
 */
struct enc_str {
    unsigned int slit;       /* toggles between 0 and 1 */
    unsigned int home_slit;  /* 1 at home position, 0 otherwise */
    SemaphoreHandle_t lock;  /* mutex */
};
static struct enc_str enc_data;

/*
 * Rising edge counter:
 * - count: number of detected rising edges of slit
 * - lock:  mutex
 */
struct _rising_edge {
    unsigned int count;
    SemaphoreHandle_t lock;
};
static struct _rising_edge rising_edge;

/*
 * Revolution time (period between two home pulses):
 * - time_diff: delta time in ticks between consecutive home events
 * - lock:      mutex
 */
struct _round_time {
    TickType_t time_diff;
    SemaphoreHandle_t lock;
};
static struct _round_time round_time;

/*
 * Slack time estimation for RT1 and RT2:
 * - slack_time: estimated slack (in ticks) for the last activation
 * - lock:       mutex
 */
struct _slack_rt1 {
    TickType_t slack_time;
    SemaphoreHandle_t lock;
};
static struct _slack_rt1 slack_rt1;

struct _slack_rt2 {
    TickType_t slack_time;
    SemaphoreHandle_t lock;
};
static struct _slack_rt2 slack_rt2;

/*-----------------------------------------------------------*/

void main_blinky(void)
{
    console_print("Main\n");

    /*
     * Create FreeRTOS mutexes.
     * FreeRTOS mutexes support priority inheritance (useful in RT systems).
     */
    enc_data.lock    = xSemaphoreCreateMutex();
    rising_edge.lock = xSemaphoreCreateMutex();
    round_time.lock  = xSemaphoreCreateMutex();
    slack_rt1.lock   = xSemaphoreCreateMutex();
    slack_rt2.lock   = xSemaphoreCreateMutex();

    /* Create tasks only if all mutexes were created successfully */
    if ((enc_data.lock != NULL) &&
        (rising_edge.lock != NULL) &&
        (round_time.lock != NULL) &&
        (slack_rt1.lock != NULL) &&
        (slack_rt2.lock != NULL))
    {
        console_print("Semaphores created \n");

        /* Create tasks */
        xTaskCreate(enc,
                    "EncoderTask",
                    configMINIMAL_STACK_SIZE,
                    NULL,
                    ENCODER_TASK_PRIORITY,
                    NULL);
        console_print("Encoder Task created\n");

        xTaskCreate(rt_task1,
                    "RT1 Task",
                    configMINIMAL_STACK_SIZE,
                    NULL,
                    RT1_PRIORITY,
                    NULL);
        console_print("Rising edge Task created\n");

        xTaskCreate(rt_task2,
                    "RT2 Task",
                    configMINIMAL_STACK_SIZE,
                    NULL,
                    RT2_PRIORITY,
                    NULL);
        console_print("Round_time Task created\n");

        xTaskCreate(scope,
                    "Scope Task",
                    configMINIMAL_STACK_SIZE,
                    NULL,
                    SCOPE_TASK_PRIORITY,
                    NULL);
        console_print("Scope Task created\n");

        xTaskCreate(diagnostic,
                    "Diagnostic Task",
                    configMINIMAL_STACK_SIZE,
                    NULL,
                    DIAGNOSTIC_TASK_PRIORITY,
                    NULL);
        console_print("Diagnostic Task created\n");

        /* Start the scheduler (tasks start running) */
        vTaskStartScheduler();
    }

    /* If the scheduler fails to start, stay here */
    for (;;);
}

/*-----------------------------------------------------------*/
/*
 * enc task: Encoder emulator
 * Period: ENCODER_PERIOD_MS (5 ms).
 *
 * It generates:
 * - slit: toggles 0/1 (square wave)
 * - home_slit: pulse set to 1 when the "home" position is reached (once every 8 rising edges)
 *
 * The variable 'semi_per' randomizes the speed (approx. range required by assignment).
 */
static void enc(void* pvParameters)
{
    TickType_t xNextWakeTime;
    const TickType_t xBlockTime = pdMS_TO_TICKS(ENCODER_PERIOD_MS);

    xNextWakeTime = xTaskGetTickCount();
    console_print("Start Encoder Task \n");

    /* Initialize shared encoder signals */
    xSemaphoreTake(enc_data.lock, portMAX_DELAY);
        enc_data.slit = 0;
        enc_data.home_slit = 0;
    xSemaphoreGive(enc_data.lock);

    unsigned int count = 0;
    unsigned int slit_count = 0;
    unsigned int prev_slit = 0;

    /* Randomized period: semi_per in [1..10] */
    srand(time(NULL));
    unsigned int semi_per = (rand() % 10) + 1;
    /* semi_per = 5; */ /* DEBUG: fixed speed */

    for (;;)
    {
        /* Periodic activation */
        vTaskDelayUntil(&xNextWakeTime, xBlockTime);

        xSemaphoreTake(enc_data.lock, portMAX_DELAY);

            prev_slit = enc_data.slit;

            /* Toggle slit every 'semi_per' cycles */
            if (count % semi_per == 0) {
                enc_data.slit++;
                enc_data.slit %= 2;
            }

            /* Detect rising edge (0 -> 1) and update slit_count */
            if (prev_slit == 0 && enc_data.slit == 1)
                slit_count = (++slit_count) % 8;

            /*
             * Generate home pulse when slit_count wraps to 0.
             * home_slit is set to slit value (1 at the rising edge moment), otherwise 0.
             */
            if (slit_count == 0) enc_data.home_slit = enc_data.slit;
            else enc_data.home_slit = 0;

            /* DEBUG: print raw encoder signals */
            /* console_print("%d:\t\t %d %d\n",count,enc_data.slit,enc_data.home_slit); */

            count++;

        xSemaphoreGive(enc_data.lock);
    }
}

/*-----------------------------------------------------------*/
/*
 * rt_task1: Rising edge counter (RT1)
 * Period: ENCODER_PERIOD_MS/2.
 *
 * - Reads enc_data.slit and counts rising edges (0 -> 1).
 * - Updates rising_edge.count (protected by mutex).
 *
 * Slack estimation:
 * - finish_time is captured at end of the loop.
 * - It compares finish_time against xNextWakeTime (used as a reference deadline).
 */
static void rt_task1(void* pvParameters)
{
    TickType_t xNextWakeTime;

    console_print("rt_task1: \n");
    const TickType_t xBlockTime = pdMS_TO_TICKS(ENCODER_PERIOD_MS / 2);

    /* Initialize shared counter */
    xSemaphoreTake(rising_edge.lock, portMAX_DELAY);
        rising_edge.count = 0;
    xSemaphoreGive(rising_edge.lock);

    int last_value = 0;
    int first_cycle = 1;
    TickType_t finish_time;

    xNextWakeTime = xTaskGetTickCount();
    console_print("Start RT1 Task \n");

    for (;;)
    {
        vTaskDelayUntil(&xNextWakeTime, xBlockTime);

        /* Slack / deadline miss check (starting from second activation) */
        if (first_cycle == 1) {
            first_cycle = 0;
        } else {
            if (finish_time <= xNextWakeTime) {
                xSemaphoreTake(slack_rt1.lock, portMAX_DELAY);
                    slack_rt1.slack_time = (xNextWakeTime - finish_time);
                xSemaphoreGive(slack_rt1.lock);
            } else {
                console_print("DEADLINE MISS  finish time: %ld ms\t  deadline:%ld ms\n",
                              finish_time, xNextWakeTime);
            }
        }

        /* Read encoder signal and detect rising edge */
        xSemaphoreTake(enc_data.lock, portMAX_DELAY);

            if (last_value == 0 && enc_data.slit == 1) {
                last_value = 1;

                xSemaphoreTake(rising_edge.lock, portMAX_DELAY);
                    rising_edge.count++;
                xSemaphoreGive(rising_edge.lock);

            } else if (last_value == 1 && enc_data.slit == 0) {
                last_value = 0;
            }

        xSemaphoreGive(enc_data.lock);

        /* End-of-cycle timestamp */
        finish_time = xTaskGetTickCount();
    }
}

/*-----------------------------------------------------------*/
/*
 * rt_task2: Home-to-home time measurement (RT2)
 * Period: ENCODER_PERIOD_MS/2.
 *
 * - Detects rising edge of enc_data.home_slit (0 -> 1).
 * - Measures delta time between two consecutive home events.
 * - Stores delta in round_time.time_diff (protected by mutex).
 *
 * Slack estimation logic is similar to rt_task1.
 */
static void rt_task2(void* pvParameters)
{
    TickType_t xNextWakeTime;

    console_print("rt_task2: \n");
    const TickType_t xBlockTime = pdMS_TO_TICKS(ENCODER_PERIOD_MS / 2);

    TickType_t time_home;
    TickType_t last_time_home;

    int first_measure = 1;
    int first_cycle = 1;
    int last_home_slit = 0;

    TickType_t finish_time;

    xNextWakeTime = xTaskGetTickCount();

    /* NOTE: this print says RT1 in your original code; should be "Start RT2 Task" */
    console_print("Start RT1 Task \n");

    for (;;)
    {
        vTaskDelayUntil(&xNextWakeTime, xBlockTime);

        /* Slack / deadline miss check */
        if (first_cycle == 1) {
            first_cycle = 0;
        } else {
            if (finish_time <= xNextWakeTime) {
                xSemaphoreTake(slack_rt2.lock, portMAX_DELAY);
                    slack_rt2.slack_time = (xNextWakeTime - finish_time);
                xSemaphoreGive(slack_rt2.lock);
            } else {
                console_print("DEADLINE MISS  finish time: %ld ms\t  deadline:%ld ms\n",
                              finish_time, xNextWakeTime);
            }
        }

        /* Detect rising edge of home_slit */
        xSemaphoreTake(enc_data.lock, portMAX_DELAY);

            if (enc_data.home_slit == 1 && last_home_slit == 0) {
                last_home_slit = 1;

                if (first_measure) {
                    /* First home event: initialize reference time */
                    last_time_home = xTaskGetTickCount();
                    first_measure = 0;
                } else {
                    /* Next home event: compute delta ticks */
                    time_home = xTaskGetTickCount();

                    xSemaphoreTake(round_time.lock, portMAX_DELAY);
                        round_time.time_diff = time_home - last_time_home;
                    xSemaphoreGive(round_time.lock);

                    last_time_home = time_home;
                }
            }
            else if (enc_data.home_slit == 0) {
                last_home_slit = 0;
            }

        xSemaphoreGive(enc_data.lock);

        finish_time = xTaskGetTickCount();
    }
}

/*-----------------------------------------------------------*/
/*
 * scope task: UI / Polling server
 * Period: ENCODER_PERIOD_MS*2.
 *
 * - Reads rising_edge.count and round_time.time_diff.
 * - Computes RPM from time_diff.
 * - Prints count and RPM.
 *
 * RPM computation:
 * - time_diff is in ticks
 * - seconds_per_rev = time_diff / configTICK_RATE_HZ
 * - rpm = 60 / seconds_per_rev = 60 * configTICK_RATE_HZ / time_diff
 */
static void scope(void* pvParameters)
{
    TickType_t xNextWakeTime;

    console_print("scope: \n");
    const TickType_t xBlockTime = pdMS_TO_TICKS(ENCODER_PERIOD_MS * 2);

    unsigned int count = 0;
    float diff = 0;
    float rpm = 0;

    xNextWakeTime = xTaskGetTickCount();
    console_print("Start Scope Task \n");

    for (;;)
    {
        vTaskDelayUntil(&xNextWakeTime, xBlockTime);

        /* Read rising edge counter */
        xSemaphoreTake(rising_edge.lock, portMAX_DELAY);
            count = rising_edge.count;
        xSemaphoreGive(rising_edge.lock);

        console_print("Rising Edge Counter : %d\t", count);

        /* Read home-to-home delta ticks */
        xSemaphoreTake(round_time.lock, portMAX_DELAY);
            /* console_print("Tick difference : %ld\n",round_time.time_diff); */ /* DEBUG */
            diff = (float)round_time.time_diff;
        xSemaphoreGive(round_time.lock);

        /* Compute RPM (diff in ticks) */
        rpm = (60 * 1000 / (diff * MS_PER_SEC / configTICK_RATE_HZ));

        console_print("RPM : %f\n", rpm);
    }
}

/*******************************************************************/
/*
 * diagnostic task:
 * Period: ENCODER_PERIOD_MS*100 (very slow).
 *
 * - Reads slack_rt1 and slack_rt2 (ticks)
 * - Accumulates average slack
 * - Every 'rounds' iterations prints the average slack in milliseconds
 */
static void diagnostic(void* pvParameters)
{
    TickType_t xNextWakeTime;

    console_print("diagnostic: \n");
    const TickType_t xBlockTime = pdMS_TO_TICKS(ENCODER_PERIOD_MS * 100);
    /* const TickType_t xBlockTime = pdMS_TO_TICKS(ENCODER_PERIOD_MS * 10); */ /* faster debug */

    unsigned long int avg_slack = 0;
    unsigned long int slack_1 = 0, slack_2 = 0;

    int i = 0;
    int rounds = 100;
    /* int rounds = 10; */ /* faster debug */

    xNextWakeTime = xTaskGetTickCount();
    console_print("Start Diagnostic Task \n");

    for (;;)
    {
        vTaskDelayUntil(&xNextWakeTime, xBlockTime);

        /* Read slack times of RT1 and RT2 */
        xSemaphoreTake(slack_rt1.lock, portMAX_DELAY);
        xSemaphoreTake(slack_rt2.lock, portMAX_DELAY);

            slack_1 = (unsigned long int)slack_rt1.slack_time;
            slack_2 = (unsigned long int)slack_rt2.slack_time;

        xSemaphoreGive(slack_rt1.lock);
        xSemaphoreGive(slack_rt2.lock);

        /* Accumulate mean slack (in ticks) */
        avg_slack += (slack_1 + slack_2) / 2;

        i++;
        if (i == rounds) {
            /* Convert from ticks to ms: ms = ticks * (1000 / configTICK_RATE_HZ) */
            avg_slack = (avg_slack / rounds) * MS_PER_SEC / configTICK_RATE_HZ;

            console_print("**********AVERAGE SLACK TIME: %ld ms**********\n", avg_slack);

            i = 0;
        }
    }
}
