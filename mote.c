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
#include "contiki.h"
#include "lib/random.h"
#include "sys/ctimer.h"
#include "net/uip.h"
#include "net/uip-ds6.h"
#include "net/uip-udp-packet.h"
#include "sys/ctimer.h"
#include "powertrace.h"
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

/* UDP Client */
#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678

#define UDP_EXAMPLE_ID  190
#define START_INTERVAL		(15 * CLOCK_SECOND)
#define SEND_INTERVAL		(PERIOD * CLOCK_SECOND)
#define SEND_TIME		(random_rand() % (SEND_INTERVAL))
#define MAX_PAYLOAD_LEN		30
#define PERIOD 60
static struct uip_udp_conn *client_conn;
static uip_ipaddr_t server_ipaddr;
/*---------------------------------------------------------------------------*/

/**
 * Process declarations
 */
PROCESS(blink_process, "Blink process");
PROCESS(websocket_example_process, "Websocket process");
PROCESS(sensor_input_process, "Sensor input");
PROCESS(udp_client_process, "UDP client process");
AUTOSTART_PROCESSES(&websocket_example_process, &blink_process, &sensor_input_process, &udp_client_process);
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
/* UDP Client */
static void
tcpip_handler(void)
{
  char *str;

  if(uip_newdata()) {
    str = uip_appdata;
    str[uip_datalen()] = '\0';
    printf("DATA recv '%s'\n", str);
  }
}
static void
send_packet(void *ptr)
{
  static int seq_id;
  char buf[MAX_PAYLOAD_LEN];

  seq_id++;
  PRINTF("DATA send to %d 'Hello %d'\n",
         server_ipaddr.u8[sizeof(server_ipaddr.u8) - 1], seq_id);
  sprintf(buf, "Hello %d from the client", seq_id);
  uip_udp_packet_sendto(client_conn, buf, strlen(buf),
                        &server_ipaddr, UIP_HTONS(UDP_SERVER_PORT));
}
/*---------------------------------------------------------------------------*/
static void
print_local_addresses(void)
{
  int i;
  uint8_t state;

  PRINTF("Client IPv6 addresses: ");
  for(i = 0; i < UIP_DS6_ADDR_NB; i++) {
    state = uip_ds6_if.addr_list[i].state;
    if(uip_ds6_if.addr_list[i].isused &&
       (state == ADDR_TENTATIVE || state == ADDR_PREFERRED)) {
      PRINT6ADDR(&uip_ds6_if.addr_list[i].ipaddr);
      PRINTF("\n");
      /* hack to make address "final" */
      if (state == ADDR_TENTATIVE) {
	uip_ds6_if.addr_list[i].state = ADDR_PREFERRED;
      }
    }
  }
}
/*---------------------------------------------------------------------------*/
static void
set_global_address(void)
{
  uip_ipaddr_t ipaddr;

  uip_ip6addr(&ipaddr, 0xaaaa, 0, 0, 0, 0, 0, 0, 0);
  uip_ds6_set_addr_iid(&ipaddr, &uip_lladdr);
  uip_ds6_addr_add(&ipaddr, 0, ADDR_AUTOCONF);

/* The choice of server address determines its 6LoPAN header compression.
 * (Our address will be compressed Mode 3 since it is derived from our link-local address)
 * Obviously the choice made here must also be selected in udp-server.c.
 *
 * For correct Wireshark decoding using a sniffer, add the /64 prefix to the 6LowPAN protocol preferences,
 * e.g. set Context 0 to aaaa::.  At present Wireshark copies Context/128 and then overwrites it.
 * (Setting Context 0 to aaaa::1111:2222:3333:4444 will report a 16 bit compressed address of aaaa::1111:22ff:fe33:xxxx)
 *
 * Note the IPCMV6 checksum verification depends on the correct uncompressed addresses.
 */
 
#if 0
/* Mode 1 - 64 bits inline */
   uip_ip6addr(&server_ipaddr, 0xaaaa, 0, 0, 0, 0, 0, 0, 1);
#elif 1
/* Mode 2 - 16 bits inline */
  uip_ip6addr(&server_ipaddr, 0xaaaa, 0, 0, 0, 0, 0x00ff, 0xfe00, 1);
#else
/* Mode 3 - derived from server link-local (MAC) address */
  uip_ip6addr(&server_ipaddr, 0xaaaa, 0, 0, 0, 0x0250, 0xc2ff, 0xfea8, 0xcd1a); //redbee-econotag
#endif
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
PROCESS_THREAD(udp_client_process, ev, data)
{
  static struct etimer periodic;
  static struct ctimer backoff_timer;
#if WITH_COMPOWER
  static int print = 0;
#endif

  PROCESS_BEGIN();

  PROCESS_PAUSE();

  set_global_address();
  
  PRINTF("UDP client process started\n");

  print_local_addresses();

  /* new connection with remote host */
  client_conn = udp_new(NULL, UIP_HTONS(UDP_SERVER_PORT), NULL); 
  if(client_conn == NULL) {
    PRINTF("No UDP connection available, exiting the process!\n");
    PROCESS_EXIT();
  }
  udp_bind(client_conn, UIP_HTONS(UDP_CLIENT_PORT)); 

  PRINTF("Created a connection with the server ");
  PRINT6ADDR(&client_conn->ripaddr);
  PRINTF(" local/remote port %u/%u\n",
	UIP_HTONS(client_conn->lport), UIP_HTONS(client_conn->rport));

  powertrace_sniff(POWERTRACE_ON);

  etimer_set(&periodic, SEND_INTERVAL);
  while(1) {
    PROCESS_YIELD();
    if(ev == tcpip_event) {
      tcpip_handler();
    }
    
    if(etimer_expired(&periodic)) {
      etimer_reset(&periodic);
      ctimer_set(&backoff_timer, SEND_TIME, send_packet, NULL);

      if (print == 0) {
	powertrace_print("#P");
      }
      if (++print == 3) {
	print = 0;
      }
    }
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
  
