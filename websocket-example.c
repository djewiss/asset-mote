#include "mist.h"
#include "websocket.h"
#include "stdio.h"
#include "dev/gpio.h"
#include "dev/button-sensor.h"

static struct websocket s;

#define WEBSOCKET_HTTP_CLIENT_TIMEOUT 200
//#define MAC_ADDR 00 12 4B 00 04 0E F3 88

static void callback(struct websocket *s, websocket_result r,
                     uint8_t *data, uint16_t datalen);

#define RECONNECT_INTERVAL 20 * CLOCK_SECOND
static struct ctimer reconnect_timer;

#define PORT 12345 //multicast port
static struct udp_socket u;
static uip_ipaddr_t addr;

#define SEND_INTERVAL (30 * CLOCK_SECOND) // mesh multicast sender interval
static struct etimer periodic_timer, send_timer;

/*---------------------------------------------------------------------------*/
/* Declare the Blink process. */
PROCESS(blink_process, "Blink process");
PROCESS(websocket_example_process, "Websocket process");
PROCESS(sensor_input_process, "Sensor input");
PROCESS(multicast_example_process,"Link local multicast example process");
AUTOSTART_PROCESSES(&websocket_example_process, &blink_process, &sensor_input_process, &multicast_example_process);
/*---------------------------------------------------------------------------*/

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
static void
route_callback(int event, uip_ipaddr_t *route, uip_ipaddr_t *ipaddr,
               int numroutes)
{
  if(event == UIP_DS6_NOTIFICATION_DEFRT_ADD) {
    leds_off(LEDS_ALL);
    printf("Got a RPL route\n");
  }
}
/*---------------------------------------------------------------------------*/
static void
receiver(struct udp_socket *c,
         void *ptr,
         const uip_ipaddr_t *sender_addr,
         uint16_t sender_port,
         const uip_ipaddr_t *receiver_addr,
         uint16_t receiver_port,
         const uint8_t *data,
         uint16_t datalen)
{
  printf("Data received on port %d from port %d with length %d, '%s'\n",
         receiver_port, sender_port, datalen, data);
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
PROCESS_THREAD(multicast_example_process, ev, data)
{
  PROCESS_BEGIN();

  /* Create a linkl-local multicast addresses. */
  uip_ip6addr(&addr, 0xff02, 0, 0, 0, 0, 0, 0x1337, 0x0001);

  /* Join local group. */
  if(uip_ds6_maddr_add(&addr) == NULL) {
    printf("Error: could not join local multicast group.\n");
  }

  /* Register UDP socket callback */
  udp_socket_register(&u, NULL, receiver);

  /* Bind UDP socket to local port */
  udp_socket_bind(&u, PORT);

  /* Connect UDP socket to remote port */
  udp_socket_connect(&u, NULL, PORT);

  while(1) {

    /* Set up two timers, one for keeping track of the send interval,
which is periodic, and one for setting up a randomized send time
within that interval. */
    etimer_set(&periodic_timer, SEND_INTERVAL);
    etimer_set(&send_timer, (random_rand() % SEND_INTERVAL));

    PROCESS_WAIT_UNTIL(etimer_expired(&send_timer));

    printf("Sending multicast\n");
    udp_socket_sendto(&u,
                      "00 12 4B 00 04 0E F3 88", 23,
                      &addr, PORT);

    PROCESS_WAIT_UNTIL(etimer_expired(&periodic_timer));
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
