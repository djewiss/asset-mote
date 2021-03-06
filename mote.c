/**
* file
* Oscap_mote
* author
* Darryl Jewiss <darryl.jewiss@daanontek.co.nz>
*/
 
/**
* Includes
*/
#include "mist.h"
#include "websocket.h"
#include "stdio.h"
#include "dev/gpio.h"
#include "dev/button-sensor.h"
#include "contiki.h"
#include "lib/random.h"
#include "sys/ctimer.h"
#include "net/uip.h"
#include "net/uip-ds6.h"
#include "net/uip-udp-packet.h"
#include "sys/ctimer.h"
#include <stdio.h>
#include <string.h>
/*---------------------------------------------------------------------------*/
  
/**
* definitions
*/
/* Websockets */
 
static struct websocket s;
#define WEBSOCKET_HTTP_CLIENT_TIMEOUT 200
#define RECONNECT_INTERVAL 20 * CLOCK_SECOND

static void callback(struct websocket *s, websocket_result r, uint8_t *data, uint16_t datalen);
static struct ctimer reconnect_timer;

/* Json string */

#define MACADDR "00:12:4b:00:04:0e:f1:f2"


/*---------------------------------------------------------------------------*/

/**
* Process declarations
*/
PROCESS(blink_process, "Blink process");
PROCESS(websocket_example_process, "Websocket process");
PROCESS(sensor_input_process, "Sensor input");
AUTOSTART_PROCESSES(&websocket_example_process, &blink_process, &sensor_input_process);
/*---------------------------------------------------------------------------*/

/**
* Initialise
*/
/* Websocket */
static void
reconnect_callback(void *ptr)
{
  websocket_open(&s, "ws://124.149.167.204:8081/",
                 "tagid=1", callback);
    printf("websocket - starting\n");
}
/*---------------------------------------------------------------------------*/
static void
callback(struct websocket *s, websocket_result r,
         uint8_t *data, uint16_t datalen)
{
// printf("websocket %p\n", (void *)s);
  if(r == WEBSOCKET_CLOSED ||
     r == WEBSOCKET_RESET ||
     r == WEBSOCKET_HOSTNAME_NOT_FOUND ||
     r == WEBSOCKET_TIMEDOUT) {
         printf(" Websocket- '%d'\n", r);
    ctimer_set(&reconnect_timer, RECONNECT_INTERVAL, reconnect_callback, s);
  } else if(r == WEBSOCKET_CONNECTED) {
    
    char buf [100];			/* buffer for Json string */
    
    printf("Connected\n");
    sprintf(buf, "\{\"TagNumberFrom\":%s,\"TagNumberTarget\":\"\",\"Status\":\"Started\",\"Location\":\"\",\"EquipmentSerialNumber\":\"\"}", MACADDR);

    printf(buf);
    websocket_send_str(s, buf);
  } else if(r == WEBSOCKET_DATA) {
    printf("websocket: Received data '%.*s' (len %d)\n", datalen,
           data, datalen);
  } else if(r == WEBSOCKET_DATA) {
    leds_on(LEDS_ALL);
    printf("websocket Received data '%.*s' (len %d)\n", datalen,
           data, datalen);
    leds_off(LEDS_ALL);
  }
}

/*---------------------------------------------------------------------------*/
/**
* Process threads
*/
  
 PROCESS_THREAD(websocket_example_process, ev, data)
{
  static struct etimer et;
  PROCESS_BEGIN();

  ctimer_set(&reconnect_timer, RECONNECT_INTERVAL, reconnect_callback, &s);

  while(1) {
    etimer_set(&et, CLOCK_SECOND * 256);
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
    printf("connected\n");
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/

PROCESS_THREAD(blink_process, ev, data)
{
    /* The event timer must be declared as static. */
    static struct etimer e;

    /* Processes always begins with PROCESS_BEGIN(). */
    PROCESS_BEGIN();

    /* We set up the timer to fire once per second */
    etimer_set(&e, CLOCK_SECOND*10);

    /* Run for ever. */
    while(1) {

        /* Wait until the timer expires, the reset the timer and turn on
all LEDs. */
        PROCESS_WAIT_UNTIL(etimer_expired(&e));
        etimer_reset(&e);
        leds_toggle(LEDS_RED);
    }
    leds_off(LEDS_RED);
    /* Processes must end with PROCESS_END(). */
    PROCESS_END();
}
/*---------------------------------------------------------------------------*/

static uint8_t active;
PROCESS_THREAD(sensor_input_process, ev, data)
{
  PROCESS_BEGIN();
  active = 0;
  SENSORS_ACTIVATE(button_sensor);
  char sndTamper[200];

  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(ev == sensors_event &&
data == &button_sensor);
    leds_on(LEDS_ALL);
    printf("tamper activated\n");
    sprintf(sndTamper, "\{\"TagNumberFrom\":\"%s\",\"TagNumberTarget\":\"\",\"Status\":\"Detached\",\"Location\":\"\",\"EquipmentSerialNumber\":\"\"}", MACADDR);
    printf(sndTamper);
    printf("\n");
    
    websocket_send_str(&s,sndTamper);
    active ^= 1;
    leds_off(LEDS_ALL);

  }
  PROCESS_END();
}
/*---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
  
