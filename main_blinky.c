

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


/* Numero di task */
#define TASK_NUM 5

/* Periodo Encoder */
#define ENCODER_PERIOD_MS 5

/* Indice del task di stampa (Polling Server) */
#define PRINT_TASK_POSITION 0

/*Milleseconds per second */

#define MS_PER_SEC 1000

/* Priorità dei task */
#define	ENCODER_TASK_PRIORITY		( tskIDLE_PRIORITY + 5 )
#define	RT1_PRIORITY		( tskIDLE_PRIORITY + 6 )
#define	RT2_PRIORITY		( tskIDLE_PRIORITY + 6 )
#define	SCOPE_TASK_PRIORITY		( tskIDLE_PRIORITY + 2 )
#define	DIAGNOSTIC_TASK_PRIORITY		( tskIDLE_PRIORITY + 4 )


/*-----------------------------------------------------------*/

/* Definizione dei task da implementare */
static void enc(void* pvParameters);
static void rt_task1(void* pvParameters); //rt1
static void rt_task2(void* pvParameters);  //rt2
static void scope(void* pvParameters);
static void diagnostic(void* pvParameters);

/*-----------------------------------------------------------*/

/* Array dei periodi */
static int periods[TASK_NUM];

//Array counter
static int counter[TASK_NUM];


// Definizione della struttura
struct enc_str {
    unsigned int slit;       // Valori oscillanti tra 0 e 1
    unsigned int home_slit;  // 1 se in home, 0 altrimenti
    SemaphoreHandle_t lock;  // Mutex per la sincronizzazione
};
static struct enc_str enc_data;

struct _rising_edge{
	unsigned int count;
	SemaphoreHandle_t lock;
};
static struct _rising_edge rising_edge;

struct _round_time{
	TickType_t time_diff;
	SemaphoreHandle_t lock;
};
static struct _round_time round_time;

struct _slack_rt1{
	TickType_t slack_time;
	SemaphoreHandle_t lock;
};
static struct _slack_rt1 slack_rt1;

struct _slack_rt2{
	TickType_t slack_time;
	SemaphoreHandle_t lock;
};
static struct _slack_rt2 slack_rt2;


/*-----------------------------------------------------------*/

void main_blinky(void)
{
    console_print("Main\n");
	/* Inizializzare i periodi dei task */

	// Creare i Mutex tramite le API freeRTOS 
    enc_data.lock = xSemaphoreCreateMutex();
    rising_edge.lock = xSemaphoreCreateMutex();
    round_time.lock = xSemaphoreCreateMutex();
    slack_rt1.lock = xSemaphoreCreateMutex();
    slack_rt2.lock = xSemaphoreCreateMutex();

    if((enc_data.lock != NULL) && (rising_edge.lock != NULL) && (round_time.lock != NULL) && (slack_rt1.lock != NULL) && (slack_rt2.lock != NULL))
    {
        console_print("Semaphores created \n");
        //creazione tasks
        xTaskCreate(enc,
                "EncoderTask",
                configMINIMAL_STACK_SIZE,
                NULL,
                (ENCODER_TASK_PRIORITY),
                NULL);
        console_print("Encoder Task created\n");

        xTaskCreate(rt_task1,
                "RT1 Task",
                configMINIMAL_STACK_SIZE,
                NULL,
                (RT1_PRIORITY),
                NULL);
        console_print("Rising edge Task created\n");

        xTaskCreate(rt_task2,
                "RT2 Task",
                configMINIMAL_STACK_SIZE,
                NULL,
                (RT2_PRIORITY),
                NULL);
        console_print("Round_time Task created\n");

        xTaskCreate(scope,
                "Scope Task",
                configMINIMAL_STACK_SIZE,
                NULL,
                (SCOPE_TASK_PRIORITY),
                NULL);
        console_print("Scope Task created\n");

        xTaskCreate(diagnostic,
                "Diagnostic Task",
                configMINIMAL_STACK_SIZE,
                NULL,
                (DIAGNOSTIC_TASK_PRIORITY),
                NULL);
        console_print("Diagnostic Task created\n");

        vTaskStartScheduler();
        }

	for (;; );    
}
/*-----------------------------------------------------------*/

static void enc(void* pvParameters)
{
	TickType_t xNextWakeTime;
    
    const TickType_t xBlockTime = pdMS_TO_TICKS(ENCODER_PERIOD_MS);

    xNextWakeTime = xTaskGetTickCount();

    console_print("Start Encoder Task \n");

    xSemaphoreTake(enc_data.lock , portMAX_DELAY);
	    enc_data.slit = 0;
	    enc_data.home_slit = 0;
	xSemaphoreGive(enc_data.lock);

	unsigned int count = 0;
	unsigned int slit_count = 0;		
	unsigned int prev_slit = 0;
	
	/* Randomized period (75-750 RPM) */
	srand(time(NULL));
	unsigned int semi_per = (rand() % 10) + 1;	
	//semi_per = 5;								//DEBUG
	

	for (;; )
	{
		vTaskDelayUntil(&xNextWakeTime, xBlockTime);

        xSemaphoreTake(enc_data.lock, portMAX_DELAY);
		
		prev_slit = enc_data.slit;
		if (count%semi_per == 0) {
			enc_data.slit++;
			enc_data.slit%=2;
		}

		if (prev_slit==0&&enc_data.slit==1) 					//fronte di salita
			slit_count=(++slit_count)%8;

		if (slit_count==0) enc_data.home_slit=enc_data.slit;
		else enc_data.home_slit=0;

		//console_print("%d:\t\t %d %d\n",count,enc_data.slit,enc_data.home_slit);	//DEBUG encoder
		count++;
		
		xSemaphoreGive(enc_data.lock);



	}
}
/*-----------------------------------------------------------*/

static void rt_task1(void* pvParameters)
{

	TickType_t xNextWakeTime;
    
    console_print("rt_task1: \n");
    const TickType_t xBlockTime = pdMS_TO_TICKS(ENCODER_PERIOD_MS/2);

    xSemaphoreTake(rising_edge.lock, portMAX_DELAY);
	rising_edge.count = 0;
	xSemaphoreGive(rising_edge.lock);

	int last_value=0, first_cycle=1;
	TickType_t finish_time;

    xNextWakeTime = xTaskGetTickCount();

    console_print("Start RT1 Task \n");
	for (;; )
	{
        vTaskDelayUntil(&xNextWakeTime, xBlockTime);

        if (first_cycle==1){
            first_cycle=0;
        }else{
            if(finish_time <= xNextWakeTime){
                xSemaphoreTake(slack_rt1.lock, portMAX_DELAY);
                slack_rt1.slack_time = (xNextWakeTime - finish_time);
                xSemaphoreGive(slack_rt1.lock);
            }
            else{
                console_print("DEADLINE MISS  finish time: %ld ms\t  deadline:%ld ms\n", finish_time, xNextWakeTime);
            }
        }

		xSemaphoreTake(enc_data.lock, portMAX_DELAY);
		if( last_value == 0 && enc_data.slit == 1){
			last_value = 1;
			
			xSemaphoreTake(rising_edge.lock, portMAX_DELAY);
			rising_edge.count++;
			xSemaphoreGive(rising_edge.lock);
			
		}
		else if(last_value == 1 && enc_data.slit == 0){
			last_value = 0;
		}
		xSemaphoreGive(enc_data.lock);
		
		/* Slack Time */
        finish_time = xTaskGetTickCount(); //to ms
        
	}
}

/*-----------------------------------------------------------*/

static void rt_task2(void* pvParameters)
{

	TickType_t xNextWakeTime;
    
    console_print("rt_task2: \n");
    const TickType_t xBlockTime = pdMS_TO_TICKS(ENCODER_PERIOD_MS/2);

    TickType_t time_home;
	TickType_t last_time_home;

    int first_measure = 1, first_cycle =1;
	int last_home_slit = 0;


	TickType_t finish_time;

    xNextWakeTime = xTaskGetTickCount();

    console_print("Start RT1 Task \n");
	for (;; )
	{
        vTaskDelayUntil(&xNextWakeTime, xBlockTime);

        if (first_cycle==1){
            first_cycle=0;
        }else{
            if(finish_time <= xNextWakeTime){
                xSemaphoreTake(slack_rt2.lock, portMAX_DELAY);
                slack_rt2.slack_time = (xNextWakeTime - finish_time);
                xSemaphoreGive(slack_rt2.lock);
            }
            else{
                console_print("DEADLINE MISS  finish time: %ld ms\t  deadline:%ld ms\n", finish_time, xNextWakeTime);
            }
        }

        xSemaphoreTake(enc_data.lock, portMAX_DELAY);
            if(enc_data.home_slit == 1 && last_home_slit == 0){
                last_home_slit = 1;
                if(first_measure){
                    last_time_home = xTaskGetTickCount();
                    first_measure = 0;
                }
                else{	
                    time_home = xTaskGetTickCount();
                                           
                    xSemaphoreTake(round_time.lock, portMAX_DELAY);
                        round_time.time_diff = time_home - last_time_home;
                    xSemaphoreGive(round_time.lock);

                    last_time_home = time_home;	
                }
            }
            else if(enc_data.home_slit == 0){
                last_home_slit = 0;
            } 
		xSemaphoreGive(enc_data.lock);
		/* Slack Time */
        finish_time = xTaskGetTickCount(); //to ms
	}
}


/*-----------------------------------------------------------*/

static void scope(void* pvParameters)
{

	TickType_t xNextWakeTime;
    
    console_print("scope: \n");
    const TickType_t xBlockTime = pdMS_TO_TICKS(ENCODER_PERIOD_MS*2);
    unsigned int count=0;
	float  diff = 0;
    float rpm = 0;

    xNextWakeTime = xTaskGetTickCount();

    console_print("Start Scope Task \n");
	for (;; )
	{
        vTaskDelayUntil(&xNextWakeTime, xBlockTime);

        xSemaphoreTake(rising_edge.lock, portMAX_DELAY);
		count = rising_edge.count;
		xSemaphoreGive(rising_edge.lock);
		
		console_print("Rising Edge Counter : %d\t",count);
		
		xSemaphoreTake(round_time.lock, portMAX_DELAY);
        //console_print("Tick difference : %ld\n",round_time.time_diff);		//debug	
		diff =(float) round_time.time_diff;
        
		xSemaphoreGive(round_time.lock);
		
		rpm = (60*1000/(diff*MS_PER_SEC/configTICK_RATE_HZ)); //per avere frequenza di rotazioni su milliseondi
	
		console_print("RPM : %f\n",rpm);	

	
	}
}

/*******************************************************************/

static void diagnostic(void* pvParameters){

    TickType_t xNextWakeTime;
    
    console_print("diagnostic: \n");
    const TickType_t xBlockTime = pdMS_TO_TICKS(ENCODER_PERIOD_MS*100);
    //const TickType_t xBlockTime = pdMS_TO_TICKS(ENCODER_PERIOD_MS*10); //per debug veloce

    unsigned long int avg_slack=0, slack_1 = 0, slack_2 = 0;
	int i = 0;
	int rounds = 100;
    //int rounds = 10; //per debug veloce

    xNextWakeTime = xTaskGetTickCount();

    console_print("Start Diagnostic Task \n");

	for(;; ) {
		vTaskDelayUntil(&xNextWakeTime, xBlockTime);
			
		xSemaphoreTake(slack_rt1.lock, portMAX_DELAY);
        xSemaphoreTake(slack_rt2.lock, portMAX_DELAY);

        slack_1 = (unsigned long int)slack_rt1.slack_time;
        slack_2 = (unsigned long int)slack_rt2.slack_time;
		
        xSemaphoreGive(slack_rt1.lock);
        xSemaphoreGive(slack_rt2.lock);

        //console_print("Slack time acquisiti.\n"); //debug

		avg_slack += (slack_1 + slack_2)/2; 	
		i++;
		if(i == rounds){
			avg_slack = (avg_slack/rounds)*MS_PER_SEC/configTICK_RATE_HZ;
			console_print("**********SLACK TIME MEDIO: %ld ms**********\n",avg_slack);
			i = 0;
		}
        
	}
}