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
 * Questo file implementa un sistema FreeRTOS con 5 task:
 *
 *  1) enc         : emulatore encoder, genera i segnali (slit, home_slit)
 *  2) rt_task1    : conta i fronti di salita di slit (rising edge counter)
 *  3) rt_task2    : misura il tempo tra due home consecutive (round time)
 *  4) scope       : "polling server" / task di stampa (bassa priorità)
 *  5) diagnostic  : calcola e stampa lo slack time medio dei task RT
 *
 * Sincronizzazione:
 *  - Tutte le variabili condivise sono protette da mutex FreeRTOS (xSemaphoreCreateMutex).
 *
 * Tempo:
 *  - I task periodici usano vTaskDelayUntil (periodicità basata su tick).
 */

/* Numero totale di task creati */
#define TASK_NUM 5

/* Periodo dell'emulatore encoder (in ms) */
#define ENCODER_PERIOD_MS 5

/* Indice del task "scope" (qui non usato davvero nel codice, ma lasciato come costante) */
#define PRINT_TASK_POSITION 0

/* Milliseconds per second */
#define MS_PER_SEC 1000

/* Priorità dei task (relative a tskIDLE_PRIORITY) */
#define ENCODER_TASK_PRIORITY       ( tskIDLE_PRIORITY + 5 )
#define RT1_PRIORITY                ( tskIDLE_PRIORITY + 6 )
#define RT2_PRIORITY                ( tskIDLE_PRIORITY + 6 )
#define SCOPE_TASK_PRIORITY         ( tskIDLE_PRIORITY + 2 )
#define DIAGNOSTIC_TASK_PRIORITY    ( tskIDLE_PRIORITY + 4 )

/*-----------------------------------------------------------*/

/* Prototipi dei task */
static void enc(void* pvParameters);          /* Encoder emulator */
static void rt_task1(void* pvParameters);     /* Conteggio rising edge */
static void rt_task2(void* pvParameters);     /* Misura tempo tra home */
static void scope(void* pvParameters);        /* Stampa count e RPM */
static void diagnostic(void* pvParameters);   /* Slack-time medio */

/*-----------------------------------------------------------*/

/* Array dei periodi (nel tuo codice non viene inizializzato/uso: placeholder) */
static int periods[TASK_NUM];

/* Array counter (non usato: placeholder) */
static int counter[TASK_NUM];

/*-----------------------------------------------------------*/
/* Strutture dati condivise + mutex */

/*
 * enc_str:
 * - slit:      onda quadra 0/1 (simula i "denti" dell'encoder)
 * - home_slit: impulso home (1 quando in home, 0 altrimenti)
 * - lock:      mutex FreeRTOS per proteggere l'accesso concorrente
 */
struct enc_str {
    unsigned int slit;
    unsigned int home_slit;
    SemaphoreHandle_t lock;
};
static struct enc_str enc_data;

/*
 * rising_edge:
 * - count: numero di fronti di salita rilevati su slit
 * - lock: mutex
 */
struct _rising_edge {
    unsigned int count;
    SemaphoreHandle_t lock;
};
static struct _rising_edge rising_edge;

/*
 * round_time:
 * - time_diff: differenza in tick tra due home consecutive
 * - lock: mutex
 */
struct _round_time {
    TickType_t time_diff;
    SemaphoreHandle_t lock;
};
static struct _round_time round_time;

/*
 * slack_rt1/slack_rt2:
 * - slack_time: stima dello slack time dell'ultima attivazione (in tick)
 * - lock: mutex
 *
 * Nota: qui lo slack viene stimato come (deadline - finish_time), usando
 * xNextWakeTime come riferimento di periodicità.
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
     * Creazione dei mutex (FreeRTOS mutex -> include priority inheritance).
     * Ogni struttura condivisa ha il suo lock.
     */
    enc_data.lock     = xSemaphoreCreateMutex();
    rising_edge.lock  = xSemaphoreCreateMutex();
    round_time.lock   = xSemaphoreCreateMutex();
    slack_rt1.lock    = xSemaphoreCreateMutex();
    slack_rt2.lock    = xSemaphoreCreateMutex();

    if ((enc_data.lock != NULL) &&
        (rising_edge.lock != NULL) &&
        (round_time.lock != NULL) &&
        (slack_rt1.lock != NULL) &&
        (slack_rt2.lock != NULL))
    {
        console_print("Semaphores created \n");

        /* Creazione task: stack minimo, param NULL, priorità definite sopra */

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
                    configMINimal_STACK_SIZE,
                    NULL,
                    DIAGNOSTIC_TASK_PRIORITY,
                    NULL);
        console_print("Diagnostic Task created\n");

        /* Avvio scheduler: da qui in poi eseguono i task */
        vTaskStartScheduler();
    }

    /* Se lo scheduler non parte (es. heap insufficiente), resta qui */
    for (;;);
}

/*-----------------------------------------------------------*/
/*
 * Task enc (Encoder emulator)
 * Periodo: ENCODER_PERIOD_MS (5ms).
 *
 * Logica:
 * - genera slit come onda quadra 0/1
 * - incrementa "slit_count" sui fronti di salita
 * - ogni 8 fronti di salita (slit_count==0) genera home_slit
 *
 * "semi_per" randomizza la velocità simulata (75-750 RPM circa nella traccia).
 */
static void enc(void* pvParameters)
{
    TickType_t xNextWakeTime;

    const TickType_t xBlockTime = pdMS_TO_TICKS(ENCODER_PERIOD_MS);
    xNextWakeTime = xTaskGetTickCount();

    console_print("Start Encoder Task \n");

    /* Inizializza segnali condivisi */
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
    /* semi_per = 5; */ /* DEBUG: fissa la velocità */

    for (;;)
    {
        /* Periodic execution */
        vTaskDelayUntil(&xNextWakeTime, xBlockTime);

        xSemaphoreTake(enc_data.lock, portMAX_DELAY);

            prev_slit = enc_data.slit;

            /* Toggle slit ogni semi_per cicli */
            if (count % semi_per == 0) {
                enc_data.slit++;
                enc_data.slit %= 2;
            }

            /* Rileva fronte di salita (0->1) per avanzare slit_count */
            if (prev_slit == 0 && enc_data.slit == 1)
                slit_count = (++slit_count) % 8;

            /* home_slit attivo solo quando slit_count torna a 0 */
            if (slit_count == 0)
                enc_data.home_slit = enc_data.slit;
            else
                enc_data.home_slit = 0;

            /* console_print(...) DEBUG encoder */
            count++;

        xSemaphoreGive(enc_data.lock);
    }
}

/*-----------------------------------------------------------*/
/*
 * Task rt_task1 (RT1) - Rising edge counter
 * Periodo: ENCODER_PERIOD_MS/2.
 *
 * Scopo:
 * - legge slit
 * - conta i fronti di salita (0->1)
 * - salva il conteggio in rising_edge.count (protetto da mutex)
 *
 * Slack-time:
 * - finish_time = tick alla fine del ciclo
 * - confronto con xNextWakeTime (usato come riferimento di deadline periodica)
 */
static void rt_task1(void* pvParameters)
{
    TickType_t xNextWakeTime;

    console_print("rt_task1: \n");
    const TickType_t xBlockTime = pdMS_TO_TICKS(ENCODER_PERIOD_MS / 2);

    /* Inizializza counter */
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

        /* Slack-time / deadline miss check (dal secondo ciclo in poi) */
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

        /* Lettura slit e rilevamento fronte di salita */
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

        /* Fine ciclo: salva tempo di completamento */
        finish_time = xTaskGetTickCount();
    }
}

/*-----------------------------------------------------------*/
/*
 * Task rt_task2 (RT2) - Round time (tempo tra due home)
 * Periodo: ENCODER_PERIOD_MS/2.
 *
 * Scopo:
 * - rileva fronti di salita di home_slit (0->1)
 * - alla seconda home e successive: calcola delta tick tra due home
 * - salva round_time.time_diff (protetto da mutex)
 *
 * Slack-time analogo a rt_task1.
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
    console_print("Start RT2 Task \n");

    for (;;)
    {
        vTaskDelayUntil(&xNextWakeTime, xBlockTime);

        /* Slack-time / deadline miss check */
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

        /* Rileva home event (0->1) */
        xSemaphoreTake(enc_data.lock, portMAX_DELAY);

            if (enc_data.home_slit == 1 && last_home_slit == 0) {
                last_home_slit = 1;

                if (first_measure) {
                    last_time_home = xTaskGetTickCount();
                    first_measure = 0;
                } else {
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
 * Task scope (stampa / polling server)
 * Periodo: ENCODER_PERIOD_MS*2 (più lento degli RT).
 *
 * Scopo:
 * - legge rising_edge.count
 * - legge round_time.time_diff
 * - calcola RPM e stampa (console_print)
 *
 * RPM:
 * - diff è in tick
 * - secondi = diff / configTICK_RATE_HZ
 * - rpm = 60 / secondi = 60 * configTICK_RATE_HZ / diff
 * Il tuo calcolo è algebraicamente equivalente a questo.
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

        /* Leggi conteggio rising edge */
        xSemaphoreTake(rising_edge.lock, portMAX_DELAY);
            count = rising_edge.count;
        xSemaphoreGive(rising_edge.lock);

        console_print("Rising Edge Counter : %d\t", count);

        /* Leggi delta-t tra home */
        xSemaphoreTake(round_time.lock, portMAX_DELAY);
            diff = (float)round_time.time_diff;
        xSemaphoreGive(round_time.lock);

        /* Calcolo RPM (diff in tick) */
        rpm = (60 * 1000 / (diff * MS_PER_SEC / configTICK_RATE_HZ));

        console_print("RPM : %f\n", rpm);
    }
}

/*-----------------------------------------------------------*/
/*
 * Task diagnostic
 * Periodo: ENCODER_PERIOD_MS*100 (molto lento).
 *
 * Scopo:
 * - legge slack_rt1 e slack_rt2 (in tick)
 * - calcola slack medio, accumula, e ogni N iterazioni stampa la media
 *
 * Conversione:
 * - avg_slack calcolato in tick -> convertito in ms tramite:
 *     ms = tick * (1000 / configTICK_RATE_HZ)
 */
static void diagnostic(void* pvParameters)
{
    TickType_t xNextWakeTime;

    console_print("diagnostic: \n");
    const TickType_t xBlockTime = pdMS_TO_TICKS(ENCODER_PERIOD_MS * 100);

    unsigned long int avg_slack = 0;
    unsigned long int slack_1 = 0, slack_2 = 0;

    int i = 0;
    int rounds = 100;

    xNextWakeTime = xTaskGetTickCount();
    console_print("Start Diagnostic Task \n");

    for (;;)
    {
        vTaskDelayUntil(&xNextWakeTime, xBlockTime);

        /* Legge slack time dei due task RT */
        xSemaphoreTake(slack_rt1.lock, portMAX_DELAY);
        xSemaphoreTake(slack_rt2.lock, portMAX_DELAY);

            slack_1 = (unsigned long int)slack_rt1.slack_time;
            slack_2 = (unsigned long int)slack_rt2.slack_time;

        xSemaphoreGive(slack_rt1.lock);
        xSemaphoreGive(slack_rt2.lock);

        /* Aggiorna media accumulata (slack medio tra RT1 e RT2) */
        avg_slack += (slack_1 + slack_2) / 2;

        i++;
        if (i == rounds) {
            /* Converti da tick a ms */
            avg_slack = (avg_slack / rounds) * MS_PER_SEC / configTICK_RATE_HZ;

            console_print("**********SLACK TIME MEDIO: %ld ms**********\n", avg_slack);

            /* Reset contatore (NB: avg_slack non viene azzerato nel tuo codice) */
            i = 0;
        }
    }
}
