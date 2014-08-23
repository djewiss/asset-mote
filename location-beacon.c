/**
 * file
 *         Oscap_mote
 * author
 *         Darryl Jewiss <darryl.jewiss@daanontek.co.nz>
 */
 
/**
 * Includes
 */
 
#include "mist.h"
#include "websocket.h"
#include "stdio.h"
#include "dev/gpio.h"
#include "dev/button-sensor.h"
#include "net/rime.h"
#include "myneighbors.h"
#include "packets.h"
#include "node-id.h"

#include <stdio.h>
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
struct myneighbor {
  rimeaddr_t addr;
  int rssi;
};

#define MAX_NEIGHBORS 20
static struct myneighbor neighbors[MAX_NEIGHBORS];
/*---------------------------------------------------------------------------*/

/**
 * Process declarations
 */
PROCESS(blink_process, "Blink process");
PROCESS(websocket_example_process, "Websocket process");
PROCESS(sensor_input_process, "Sensor input");
PROCESS(neighbor_process, "Neighbor process");
AUTOSTART_PROCESSES(&websocket_example_process, &blink_process, &sensor_input_process, &neighbor_process);
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
    websocket_send_str(s, "Connected");
    websocket_send_str(s, "{\"TagNumberFrom\":\"00 12 4B 00 04 0E F3 88\",\"TagNumberTarget\":\"\",\"Status\":\"Started\",\"Location\":\"\",\"EquipmentSerialNumber\":\"\"}");
  } else if(r == WEBSOCKET_DATA) {
    printf("websocket: Received data '%.*s' (len %d)\n", datalen,
           data, datalen);
  }
}
/*---------------------------------------------------------------------------*/
void
myneighbors_init(void)
{
  int i;
  for (i=0; i < MAX_NEIGHBORS; i++) {
    rimeaddr_copy(&neighbors[i].addr, &rimeaddr_null);
  }
}
/*---------------------------------------------------------------------------*/
void
myneighbors_update(rimeaddr_t *neighbor, int rssi)
{
  int i;

  /* Locate neighbor */
  for (i=0; i < MAX_NEIGHBORS; i++) {
    if (rimeaddr_cmp(&neighbors[i].addr, neighbor)
        || rimeaddr_cmp(&neighbors[i].addr, &rimeaddr_null)) {
      break;
    }
  }
  if (i>=MAX_NEIGHBORS) {
    /* Neighbor list is full! */
    return;
  }

  if (rimeaddr_cmp(&neighbors[i].addr, neighbor)) {
    /* Update existing neighbor */
    neighbors[i].rssi = rssi;
    printf("neighbors[%i] = { %d.%d, %i }\n", i,
        neighbors[i].addr.u8[0], neighbors[i].addr.u8[1],
        neighbors[i].rssi);
    return;
  }

  /* Add new neighbor */
  rimeaddr_copy(&neighbors[i].addr, neighbor);
  neighbors[i].rssi = rssi;
  printf("neighbors[%i] = { %d.%d, %i } (ADDED)\n", i,
        neighbors[i].addr.u8[0], neighbors[i].addr.u8[1],
        neighbors[i].rssi);
}
/*---------------------------------------------------------------------------*/
static int
get_neighbor_rssi(int rank, rimeaddr_t *neighbor)
{
  int i, idx_1=-1, idx_2=-1, idx_3=-1;

  /* Extract top 3 neighbors */
  for (i=0; i < MAX_NEIGHBORS; i++) {
    if (rimeaddr_cmp(&neighbors[i].addr, &rimeaddr_null)) {
      /* End of list */
      break;
    }

    /* Sort */
    if (idx_1 < 0
        || neighbors[i].rssi > neighbors[idx_1].rssi) {
      idx_3 = idx_2;
      idx_2 = idx_1;
      idx_1 = i;
    } else if (idx_2 < 0
        || neighbors[i].rssi > neighbors[idx_2].rssi) {
      idx_3 = idx_2;
      idx_2 = i;
    } else if (idx_3 < 0
        || neighbors[i].rssi > neighbors[idx_3].rssi) {
      idx_3 = i;
    }
  }

  /* Rank: [1-3] */
  if (rank == 1) {
    i = idx_1;
  } else if (rank == 2) {
    i = idx_2;
  } else if (rank == 3) {
    i = idx_3;
  } else {
    /*printf("error: bad rank: %i\n", rank);*/
    rimeaddr_copy(neighbor, &rimeaddr_null);
    return 0;
  }
  if (i < 0) {
    rimeaddr_copy(neighbor, &rimeaddr_null);
    return 0;
  }
  rimeaddr_copy(neighbor, &neighbors[i].addr);
  return neighbors[i].rssi;
}
/*---------------------------------------------------------------------------*/
int
myneighbors_best(rimeaddr_t *neighbor)
{
  return get_neighbor_rssi(1, neighbor);
}
/*---------------------------------------------------------------------------*/
int
myneighbors_second_best(rimeaddr_t *neighbor)
{
  return get_neighbor_rssi(2, neighbor);
}
/*---------------------------------------------------------------------------*/
int
myneighbors_third_best(rimeaddr_t *neighbor)
{
  return get_neighbor_rssi(3, neighbor);
}

/*---------------------------------------------------------------------------*/

/**
 * Process threads
 */ 
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(neighbor_process, ev, data)
{
  static struct etimer et;

  PROCESS_BEGIN();

  /* Init neighbor library */
  myneighbors_init();

  while(1) {
    struct report_msg msg;
    rimeaddr_t addr;
    int rssi;

    /* Delay 1 second */
    etimer_set(&et, CLOCK_SECOND);
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));

    /* Randomly update a neighbor entry */
    addr.u8[0] = 1 + (random_rand() % 6); /* [1-6] */
    addr.u8[1] = 0;
    rssi = (int) (random_rand() & 0xFF);
    myneighbors_update(&addr, rssi);
    printf("updated neighbor=%u.%u with rssi=%i\n", addr.u8[0], addr.u8[1], rssi);

    /* Delay 1 more second */
    etimer_set(&et, CLOCK_SECOND);
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));

    /* Create report msg */
    msg.type = TYPE_REPORT;

    /* Extract top neighbors */
    msg.neighbor1_rssi = myneighbors_best(&msg.neighbor1);
    msg.neighbor2_rssi = myneighbors_second_best(&msg.neighbor2);
    msg.neighbor3_rssi = myneighbors_third_best(&msg.neighbor3);

    /* Print report */
    printf("TOP 3 NEIGHBORS:\n");
    printf("neighbor1=%d.%d %i\n",
        msg.neighbor1.u8[0], msg.neighbor1.u8[1], msg.neighbor1_rssi);
    printf("neighbor2=%d.%d %i\n",
        msg.neighbor2.u8[0], msg.neighbor2.u8[1], msg.neighbor2_rssi);
    printf("neighbor3=%d.%d %i\n",
        msg.neighbor3.u8[0], msg.neighbor3.u8[1], msg.neighbor3_rssi);
    leds_toggle(LEDS_BLUE);
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/  
 PROCESS_THREAD(websocket_example_process, ev, data)
{
  static struct etimer et;
  PROCESS_BEGIN();

  ctimer_set(&reconnect_timer, RECONNECT_INTERVAL, reconnect_callback, &s);

  while(1) {
    etimer_set(&et, CLOCK_SECOND * 256);
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
    printf(&s, "connected\n");
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

  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(ev == sensors_event &&
data == &button_sensor);
    leds_toggle(LEDS_ALL);
    printf("tamper activated\n");
    websocket_send_str(&s,"{\"TagNumberFrom\":\"00 12 4B 00 04 0E F3 88\",\"TagNumberTarget\":\"00 12 4B 00 04 0E F3 88\",\"Status\":\"Detached \",\"Location\":\"\", \"EquipmentSerialNumber\":\"\"}");
    active ^= 1;
    leds_toggle(LEDS_ALL);
  }
  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
  
