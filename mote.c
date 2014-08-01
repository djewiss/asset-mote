#include "mist.h"
#include "websocket.h"
#include "stdio.h"
#include "dev/gpio.h"
#include "dev/button-sensor.h"
#include "lib/memb.h"
#include "lib/random.h"
#include "net/rime.h"
#include "lib/list.h"

/*-----------------websocket definitions--------------------*/
static struct websocket s;

#define WEBSOCKET_HTTP_CLIENT_TIMEOUT 200
//#define MAC_ADDR 00 12 4B 00 04 0E F3 88

static void callback(struct websocket *s, websocket_result r,
                     uint8_t *data, uint16_t datalen);

#define RECONNECT_INTERVAL 20 * CLOCK_SECOND
static struct ctimer reconnect_timer;

/*----------------multicast definitions---------------------*/
#define PORT 12345				//multicast port
static struct udp_socket u;
static uip_ipaddr_t addr;

#define SEND_INTERVAL	(30 * CLOCK_SECOND) // mesh multicast sender interval
static struct etimer periodic_timer, send_timer;

/*-----------------unicast definitions-----------------------*/

/* This is the structure of broadcast messages. */
struct broadcast_message {
  uint8_t seqno;
};

/* This is the structure of unicast ping messages. */
struct unicast_message {
  uint8_t type;
};

/* These are the types of unicast messages that we can send. */
enum {
  UNICAST_TYPE_PING,
  UNICAST_TYPE_PONG
};

/* This structure holds information about neighbors. */
struct neighbor {
  /* The ->next pointer is needed since we are placing these on a
Contiki list. */
  struct neighbor *next;

  /* The ->addr field holds the Rime address of the neighbor. */
  rimeaddr_t addr;

  /* The ->last_rssi and ->last_lqi fields */
  uint16_t last_rssi, last_lqi;

  /* Each broadcast packet contains a sequence number (seqno). */
  uint8_t last_seqno;

  /* The ->avg_gap contains the average seqno gap that we have seen
from this neighbor. */
  uint32_t avg_seqno_gap;

};

/* This #define defines the maximum amount of neighbors we can remember. */
#define MAX_NEIGHBORS 32

/* This MEMB() definition defines a memory pool from which we allocate
neighbor entries. */
MEMB(neighbors_memb, struct neighbor, MAX_NEIGHBORS);

/* The neighbors_list is a Contiki list that holds the neighbors we
have seen thus far. */
LIST(neighbors_list);

/* These hold the broadcast and unicast structures, respectively. */
static struct broadcast_conn broadcast;
static struct unicast_conn unicast;

/* These two defines are used for computing the moving average for the
broadcast sequence number gaps. */
#define SEQNO_EWMA_UNITY 0x100
#define SEQNO_EWMA_ALPHA 0x040

/*---------------------------------------------------------------------------*/
/* Declare the processes. */
PROCESS(blink_process, "Blink process");
PROCESS(websocket_example_process, "Websocket process");
PROCESS(sensor_input_process, "Sensor input");
PROCESS(multicast_example_process,"Link local multicast example process");
PROCESS(broadcast_process, "Broadcast process");
PROCESS(unicast_process, "Unicast process");
AUTOSTART_PROCESSES(&websocket_example_process, &blink_process, &sensor_input_process, &multicast_example_process, &broadcast_process, &unicast_process);
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
//  printf("websocket %p\n", (void *)s);
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

static void
broadcast_recv(struct broadcast_conn *c, const rimeaddr_t *from)
{
  struct neighbor *n;
  struct broadcast_message *m;
  uint8_t seqno_gap;

  /* The packetbuf_dataptr() returns a pointer to the first data byte
in the received packet. */
  m = packetbuf_dataptr();

  /* Check if we already know this neighbor. */
  for(n = list_head(neighbors_list); n != NULL; n = list_item_next(n)) {

    /* We break out of the loop if the address of the neighbor matches
the address of the neighbor from which we received this
broadcast message. */
    if(rimeaddr_cmp(&n->addr, from)) {
      break;
    }
  }

  /* If n is NULL, this neighbor was not found in our list, and we
allocate a new struct neighbor from the neighbors_memb memory
pool. */
  if(n == NULL) {
    n = memb_alloc(&neighbors_memb);

    /* If we could not allocate a new neighbor entry, we give up. We
could have reused an old neighbor entry, but we do not do this
for now. */
    if(n == NULL) {
      return;
    }

    /* Initialize the fields. */
    rimeaddr_copy(&n->addr, from);
    n->last_seqno = m->seqno - 1;
    n->avg_seqno_gap = SEQNO_EWMA_UNITY;

    /* Place the neighbor on the neighbor list. */
    list_add(neighbors_list, n);
  }

  /* We can now fill in the fields in our neighbor entry. */
  n->last_rssi = packetbuf_attr(PACKETBUF_ATTR_RSSI);
  n->last_lqi = packetbuf_attr(PACKETBUF_ATTR_LINK_QUALITY);

  /* Compute the average sequence number gap we have seen from this neighbor. */
  seqno_gap = m->seqno - n->last_seqno;
  n->avg_seqno_gap = (((uint32_t)seqno_gap * SEQNO_EWMA_UNITY) *
                      SEQNO_EWMA_ALPHA) / SEQNO_EWMA_UNITY +
                      ((uint32_t)n->avg_seqno_gap * (SEQNO_EWMA_UNITY -
                                                     SEQNO_EWMA_ALPHA)) /
    SEQNO_EWMA_UNITY;

  /* Remember last seqno we heard. */
  n->last_seqno = m->seqno;

  /* Print out a message. */
  printf("broadcast message received from %d.%d with seqno %d, RSSI %u, LQI %u, avg seqno gap %d.%02d\n",
         from->u8[0], from->u8[1],
         m->seqno,
         packetbuf_attr(PACKETBUF_ATTR_RSSI),
         packetbuf_attr(PACKETBUF_ATTR_LINK_QUALITY),
         (int)(n->avg_seqno_gap / SEQNO_EWMA_UNITY),
         (int)(((100UL * n->avg_seqno_gap) / SEQNO_EWMA_UNITY) % 100));
}
/* This is where we define what function to be called when a broadcast
is received. We pass a pointer to this structure in the
broadcast_open() call below. */
static const struct broadcast_callbacks broadcast_call = {broadcast_recv};
/*---------------------------------------------------------------------------*/
/* This function is called for every incoming unicast packet. */
static void
recv_uc(struct unicast_conn *c, const rimeaddr_t *from)
{
  struct unicast_message *msg;

  /* Grab the pointer to the incoming data. */
  msg = packetbuf_dataptr();

  /* We have two message types, UNICAST_TYPE_PING and
UNICAST_TYPE_PONG. If we receive a UNICAST_TYPE_PING message, we
print out a message and return a UNICAST_TYPE_PONG. */
  if(msg->type == UNICAST_TYPE_PING) {
    printf("unicast ping received from %d.%d\n",
           from->u8[0], from->u8[1]);
    msg->type = UNICAST_TYPE_PONG;
    packetbuf_copyfrom(msg, sizeof(struct unicast_message));
    /* Send it back to where it came from. */
    unicast_send(c, from);
  }
}
static const struct unicast_callbacks unicast_callbacks = {recv_uc};

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
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(broadcast_process, ev, data)
{
  static struct etimer et;
  static uint8_t seqno;
  struct broadcast_message msg;

  PROCESS_EXITHANDLER(broadcast_close(&broadcast);)

  PROCESS_BEGIN();

  broadcast_open(&broadcast, 129, &broadcast_call);

  while(1) {

    /* Send a broadcast every 16 - 32 seconds */
    etimer_set(&et, CLOCK_SECOND * 16 + random_rand() % (CLOCK_SECOND * 16));

    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));

    msg.seqno = seqno;
    packetbuf_copyfrom(&msg, sizeof(struct broadcast_message));
    broadcast_send(&broadcast);
    seqno++;
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(unicast_process, ev, data)
{
  PROCESS_EXITHANDLER(unicast_close(&unicast);)
    
  PROCESS_BEGIN();

  unicast_open(&unicast, 146, &unicast_callbacks);

  while(1) {
    static struct etimer et;
    struct unicast_message msg;
    struct neighbor *n;
    int randneighbor, i;
    
    etimer_set(&et, CLOCK_SECOND * 8 + random_rand() % (CLOCK_SECOND * 8));
    
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));

    /* Pick a random neighbor from our list and send a unicast message to it. */
    if(list_length(neighbors_list) > 0) {
      randneighbor = random_rand() % list_length(neighbors_list);
      n = list_head(neighbors_list);
      for(i = 0; i < randneighbor; i++) {
        n = list_item_next(n);
      }
      printf("sending unicast to %d.%d\n", n->addr.u8[0], n->addr.u8[1]);

      msg.type = UNICAST_TYPE_PING;
      packetbuf_copyfrom(&msg, sizeof(msg));
      unicast_send(&unicast, &n->addr);
    }
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
