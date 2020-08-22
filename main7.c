﻿/* ----------------------------------------------------------------------------------------------
 * real GPS car tracker on ATMEGA328P + SIM7000 module  - version 2.0 
 * by Adam Loboda - adam.loboda@wp.pl
 * baudrate UART is 9600 as most stable using RC internal clock 1MHz (8MHz with division by 8)
 * MCU clock is lowered to 1MHz to reduce power consumption
 *
 * SIM7000 default serial port speed is 115200 bps
 * please configure SIM7000 to fixed 9600 first by AT+IPR=9600 command 
 * and disable echo by ATE0 command
 * to ensure stability, and then save config via AT&W command
 * this version is suitable for the SIM7000 boards that have DTR pin exposed, utilizes sleepmode on SIM7000
 *  
 * connections to be made :
 * SIM7000 RXD to ATMEGA328 TXD PIN #3,
 * SIM7000 TXD to ATMEGA328 RXD PIN #2
 * SIM7000 DTR/SLEEP to ATMEGA PC5 PIN  #28 
 * VCC :
 * ATMEGA328 VCC (PIN #7) must be connected to lower voltage VCC ~3.3V than 5V of SIM7000 board
 * you may use 3x 1N4007 diodes in serial to drop voltage from 5V to ~3.3V
 * GND :
 * ATMEGA328 GND (PIN #8 and PIN #22) to SIM7000 GND and 0V 
 * other info :
 * the prototype board was built using www.and-global.com BK-808 breadboard
 * SIM7000 breadboard which is powered from 5V DC but uses 3.3V TTL logic on RXD/TXD
 *
 * this version has more accurate positioning from GPS module and SMS command support
 * following commands :
 * ACTIVATE : validates sending MSISDN as a voice caller
 * SINGLE   : gives single position result 
 * MULTI    : gives 5 positions with 5 min intervals
 * GUARD    : enables GUARD MODE - notifies when GPS position changes over SMS
 * STOP     : disables GUARD MODE if active
 * ----------------------------------------------------------------------------------------------
 */

#include <inttypes.h>
#include <stdio.h>
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <util/delay.h>
#include <avr/sleep.h>
#include <avr/wdt.h>
#include <string.h>
#include <avr/power.h>
#include <avr/eeprom.h>


#define UART_NO_DATA 0x0100
// internal RC oscillator 8MHz with divison by 8 and U2X0 = 1, gives 0.2% error rate for 9600 bps UART speed
// and lower current consumption
// for 1MHz : -U lfuse:w:0x62:m     on ATMEGA328P
#define F_CPU 1000000UL

// EEPROM address to store phonenumber for messages
#define EEADDR 0       
 
#define BAUD 9600
// formula for 1MHz clock and U2X0 = 1 double UART speed 
#define MYUBBR ((F_CPU / (BAUD * 8L)) - 1)


// SIM and GSM related commands
const char AT[] PROGMEM = { "AT\r" }; 
const char ISATECHO[] PROGMEM = { "AT" }; 
const char ISOK[] PROGMEM = { "OK" };
const char ISREG1[] PROGMEM = { "+CREG: 0,1" };            // SIM registered in HPLMN 
const char ISREG2[] PROGMEM = { "+CREG: 0,5" };            // SIM registered in ROAMING NETWORK 
const char SHOW_REGISTRATION[] PROGMEM = {"AT+CREG?\r"};
const char DISREGREPORT[] PROGMEM = {"AT+CREG=0\r"};  //  disable reporting URC of losing 2G coverage by +CREG=0
const char PIN_IS_READY[] PROGMEM = {"+CPIN: READY"};
const char PIN_MUST_BE_ENTERED[] PROGMEM = {"+CPIN: SIM PIN"};


const char SHOW_PIN[] PROGMEM = {"AT+CPIN?\r"};
const char ECHO_OFF[] PROGMEM = {"ATE0\r"};
const char ENTER_PIN[] PROGMEM = {"AT+CPIN=\"1111\"\r"};
const char CFGRIPIN[] PROGMEM = {"AT+CFGRI=1\r"};

const char SMS1[] PROGMEM = {"AT+CMGF=1\r"};                 // select txt format of SMS
const char SMS2[] PROGMEM = {"AT+CMGS=\""};                    // begin of sending SMS in text format
const char DELSMS[] PROGMEM = {"AT+CMGD=4\r"};    // delete all stored SMS just in case
const char SHOWSMS[] PROGMEM = {"AT+CNMI=1,2,0,0,0\r"};      // display automatically SMS when arrives
const char ISSMS[] PROGMEM = {"CMT:"};                         // beginning of mobile terminated SMS identification

// SMS commands to be interpreted
const char ISMULTI[] PROGMEM = {"MULTI"};                      // MULTI option gives 5 continous measurements
const char ISSINGLE[] PROGMEM = {"SINGLE"};                    // SINGLE option gives single measurement GPS or GSM 
const char ISACTIVATE[] PROGMEM = {"ACTIVATE"};                // ACTIVATE activates sending number as allowed to call 
const char ISGUARD[] PROGMEM = {"GUARD"};                      // GUARD mode to automatically alert of GPS position change 
const char ISSTOP[] PROGMEM = {"STOP"};                        // STOP GUARD mode 

const char COMMANDACK[] PROGMEM = {"COMMAND ACCEPTED\n"};               // Acknowledge that the SMS command was accepted
const char COMMANDSINGLEACK[] PROGMEM = {"SINGLE MEASUREMENT IN PROGRESS... PLEASE WAIT 7-8 MINUTES BEFORE NEXT COMMAND\n"};         // Acknowledge that the SINGLE command was accepted
const char COMMANDMULTIACK[] PROGMEM = {"MULTIPLE MEASUREMENTS IN PROGRESS.. PLEASE WAIT 25 MINUTES BEFORE NEXT COMMAND\n"};          // Acknowledge that the MULTI command was accepted
const char ACTIVATED[] PROGMEM =  {"ACTIVATED CALLS FROM "};            // Ack activated sending number as allowed to call 
const char GUARD[] PROGMEM =      {"GUARD MODE ACTIVATED.. PLEASE WAIT 5 MINUTES BEFORE NEXT COMMAND\n"};            // Confirmation of guard mode activated 
const char ALERT[] PROGMEM =      {"ALERT, POSITION CHANGED TO :  "};   // Alert, that guard detected position change
const char STOP[] PROGMEM =       {"GUARD MODE STOPPED"};               // Guard mode deactivated

const char CRLF[] PROGMEM = {"\"\n\r"};


// Flightmode ON OFF - for saving battery while in underground garage with no GSM signal
// tracker will check 2G network availability in 30 minutes intervals
// meanwhile radio will be switched off for power saving
const char FLIGHTON[] PROGMEM = { "AT+CFUN=4\r" };
const char FLIGHTOFF[] PROGMEM = { "AT+CFUN=1\r" };

// Sleepmode ON OFF - mode #1 requires DTR pin manipulation, 
// to get out of SIM7000 sleepmode DTR must be LOW for at least 50 miliseconds
const char SLEEPON[] PROGMEM = { "AT+CSCLK=1\r" };
const char SLEEPOFF[] PROGMEM = { "AT+CSCLK=0\r" };

// Fix UART speed to 9600 bps
const char SET9600[] PROGMEM = { "AT+IPR=9600\r" };

// Save settings to SIM7000
const char SAVECNF[] PROGMEM = { "AT&W\r" };

// Disable SIM7000 LED for further reduction of power consumption
const char DISABLELED[] PROGMEM = { "AT+CNETLIGHT=0\r" };

// for sending SMS predefined text 
const char GOOGLELOC1[] PROGMEM = {"\r\n http://maps.google.com/maps?q="};
const char GOOGLELOC2[] PROGMEM = {","};
const char GOOGLELOC3[] PROGMEM = {"\r\n"};
const char LONG[] PROGMEM = {" LONGTITUDE="};
const char LATT[] PROGMEM = {" LATITUDE="};
const char BATT[] PROGMEM = {"\nBATTERY[mV]="};


// check statuses & cells
const char CHECKBATT[] PROGMEM = {"AT+CBC\r"};           // check battery voltage 

// GPS SIM7000 only related AT commands and responses
const char GPSPWRON[] PROGMEM = {"AT+CGNSPWR=1\r"};                 // enable GPS inside SIM7000 
const char GPSISFIXED[] PROGMEM = {"+CGNSINF: 1,1,"};                 // GPS in now fixed 
const char GPSINFO[] PROGMEM = {"AT+CGNSINF\r"};                    // Read GPS LAT & LONG for SMS
const char GPSCLDSTART[] PROGMEM = {"AT+CGNSCOLD\r"};               // GPS cold restart
const char GPSHOTSTART[] PROGMEM = {"AT+CGNSHOT\r"};                // GPS hot restart
const char GPSPWROFF[] PROGMEM = {"AT+CGNSPWR=0\r"};                // disable GPS inside SIM7000 
const char INITLOC[] PROGMEM = {"0000000000000000000\x00"};           // to clear location at the beginning 



// buffers for number of phone, responses from modem
// must fit SMS message with details
#define BUFFER_SIZE 170
volatile static uint8_t response[BUFFER_SIZE] = "12345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890";
volatile static uint8_t response_pos = 0;
volatile static uint8_t phonenumber[20] = "12345678901234567890";
volatile static uint8_t phonenumber_pos = 0;
volatile static uint8_t smsphonenumber[20] = "12345678901234567890";
volatile static uint8_t smsphonenumber_pos = 0;
volatile static uint8_t smstext[BUFFER_SIZE] = "12345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890";
volatile static uint8_t smstext_pos = 0;


// buffers for Google GPS API data -  longtitude & latitude data
volatile static uint8_t latitude[20] = "00000000000000000000";
volatile static uint8_t latitude_pos = 0;
volatile static uint8_t longtitude[20] = "00000000000000000000";
volatile static uint8_t longtitude_pos = 0;

// buffers for SIM7000 GPS data -  longtitude & latitude data
volatile static uint8_t latitudegps[20] = "00000000000000000000";
volatile static uint8_t longtitudegps[20] = "00000000000000000000";
volatile static uint8_t latitudegpsold[20] = "00000000000000000000";
volatile static uint8_t longtitudegpsold[20] = "00000000000000000000";

// other buffers
volatile static uint8_t buf[40];  // buffer to copy string from PROGMEM for modem output comparision
volatile static uint8_t battery[10] = "0000000000";  // for battery voltage checking
volatile static uint8_t battery_pos = 0;

// other flags and counters
volatile static uint8_t continousgps = 0;


// ----------------------------------------------------------------------------------------------
// init_uart
// ----------------------------------------------------------------------------------------------
void init_uart(void) {
  // double speed by U2X0 flag = 1 to have 0.2% error rate on 9600 baud
 UCSR0A = (1<<U2X0);
  // set baud rate from PRESCALER
 UBRR0H = (uint8_t)(MYUBBR>>8);
 UBRR0L = (uint8_t)(MYUBBR);
 UCSR0B|=(1<<TXEN0); //enable TX
 UCSR0B|=(1<<RXEN0); //enable RX
  // set frame format for SIM7000 communication
 UCSR0C|=(1<<UCSZ00)|(1<<UCSZ01); // no parity, 1 stop bit, 8-bit data 
}



// ----------------------------------------------------------------------------------------------
// send_uart
// Sends a single char to UART without ISR
// ----------------------------------------------------------------------------------------------
void send_uart(uint8_t c) {
  // wait for empty data register
  while (!(UCSR0A & (1<<UDRE0)));
  // set data into data register
  UDR0 = c;
}



// ----------------------------------------------------------------------------------------------
// receive_uart
// Receives a single char without ISR
// ----------------------------------------------------------------------------------------------
uint8_t receive_uart() {
  while ( !(UCSR0A & (1<<RXC0)) ) 
    ; 
  return UDR0; 
}


// ----------------------------------------------------------------------------------------------
// function to search RX buffer for response  SUB IN RX_BUFFER STR
// ----------------------------------------------------------------------------------------------
uint8_t is_in_rx_buffer(char *str, char *sub, uint8_t buffersize) {
   uint8_t i, j=0, k;
    for(i=0; i<buffersize; i++)
    {
      if(str[i] == sub[j])
     {
       for(k=i, j=0; str[k] && sub[j]; j++, k++)  // if NOT NULL on each of strings
            if(str[k]!=sub[j])   break; // if different - start comparision with next char
       // if(!sub[j]) return 1; 
        if(j == strlen(sub)) return 1;  // full substring has been found        
      }
     }
     // substring not found
    return 0;
}

 

// ----------------------------------------------------------------------------------------------
// uart_puts
// Sends a string.
// ----------------------------------------------------------------------------------------------
void uart_puts(const char *s) {
  while (*s) {
    send_uart(*s);
    s++;
  }
}



// ----------------------------------------------------------------------------------------------
// uart_puts_P
// Sends a PROGMEM string.
// ----------------------------------------------------------------------------------------------
void uart_puts_P(const char *s) {
  while (pgm_read_byte(s) != 0x00) {
    send_uart(pgm_read_byte(s++));
  }
}


// ------------------------------------------------------------------------------------------------------------
// READLINE from serial port that starts with CRLF and ends with CRLF and put to 'response' buffer what read
// ------------------------------------------------------------------------------------------------------------
uint8_t readline()
{
  uint16_t char1, wholeline ;
  uint8_t i;

  // wait for first CR-LF or exit after timeout 'i' cycles - safe fuse
   i = 0;
   wholeline = 0;
   response_pos = 0;
  //
   do {
      // read single char
      char1 = receive_uart();
      // if CR-LF combination detected start to copy the response right after
      if   (  (char1 != 0x0a) && (char1 != 0x0d) && (i<150) ) 
         { response[response_pos] = char1; 
           response_pos++;
         };
      // if CR or LF detected - end of line
      if    (  char1 == 0x0a || char1 == 0x0d )              
         {  
           // if the line was already received & copied and this is only CR/LF ending :
           if (response_pos > 0)     // this is EoL
               { response[response_pos] = NULL;
                 response_pos = 0;
                  wholeline = 1;  };
         };
      // increment number of read chars and check against overflow
      i++;
      } while ( (wholeline == 0) && (i<150) );

return(1);
}


// ------------------------------------------------------------------------------------------------------------
// Read content of SMS message right after CMT URC and put to 'smstext' buffer what read
// ------------------------------------------------------------------------------------------------------------
uint8_t readsmstxt()
{
  uint16_t char1, wholeline ;
  uint8_t  i;

  // wait for first CR-LF or exit after timeout i cycles
   i = 0;
   wholeline = 0;
   smstext_pos = 0;
  //
   do {
      // read single char and check it
      char1 = receive_uart();
      // if CR-LF combination detected start to copy the response
      if   (  (char1 != 0x0a) && (char1 != 0x0d) && (i<150) ) 
         { smstext[smstext_pos] = char1; 
           smstext_pos++;
         };
      // if CR or LF detected - end of line
      if    (  char1 == 0x0a || char1 == 0x0d )              
         {  
           // if the line was already received & copied and this is only CR/LF ending :
           if (smstext_pos > 0)     // this is EoL
               { smstext[smstext_pos] = NULL;
                 smstext_pos = 0;
                 wholeline = 1;  };
         };
      // increment number of read chars and check for overflow
      i++;
      } while ( (wholeline == 0) && (i<150) );

return(1);
}



// ----------------------------------------------------------------------------------------------------------------------------
// read SMS message PHONE NUMBER from +CMT: output and response buffer and copy it to buffer 'phonenumber' for SMS sending
// ----------------------------------------------------------------------------------------------------------------------------
uint8_t readsmsphonenumber()
{
  // need 8bit ascii 
  uint8_t char1;
  uint8_t i;

  // 'i' is a safe fuse not to get out of response buffer
  i = 0;
  phonenumber_pos = 0;

  // reading from RESPONSE buffer of modem output, rewind to beginning of buffer
  response_pos = 0;

  // wait for "+CMT:" and space
      do { 
           char1 = response[response_pos];
           response_pos++;
           i++;
         } while ( (char1 != ':') && (i<150) );

      // wait for first quotation sign - there will be MSISDN number of sender
      do { 
           char1 = response[response_pos];
           response_pos++;
           i++;
         } while ( (char1 != '\"') && (i<150) );
      // if quotation detected start to copy the response - phonenumber 
      do  { 
           char1 = response[response_pos];
           response_pos++;
           phonenumber[phonenumber_pos] = char1; 
           phonenumber_pos++;
           i++;
         } while ( (char1 != '\"') && (i<150) );    // until end of quotation
     // put NULL to end the string phonenumber
           phonenumber[phonenumber_pos-1] = NULL; 
           phonenumber_pos=0;
           response_pos = 0;

 return (1);
}



// ----------------------------------------------------------------------------------------------
// Read BATTERY VOLTAGE in milivolts from AT+CBC output and put results to 'battery' buffer
// ----------------------------------------------------------------------------------------------

uint8_t readbattery()
{
  uint16_t char1;
  uint8_t i;

   // 'i' is a safe fuse not to get deadlocked in code
   i = 0;
   battery_pos = 0;

      // wait for first COMMA sign
      do { 
           char1 = receive_uart();
           i++;
         } while ( (char1 != ',') && (i<70) );
      // check if deadlocked
      if (i == 70) return(0);

      // wait for second COMMA sign
      do { 
           char1 = receive_uart();
           i++;
         } while ( (char1 != ',') && (i<70) );

      // if 2 COMMA detected start to copy battery voltage to buffer 
      do  { 
           char1 = receive_uart();
           battery[battery_pos] = char1; 
           battery_pos++;
           i++;
         } while ( (char1 != 0x0a)  && (char1 != 0x0d)  && (i<70)   );

          battery[battery_pos-1] = NULL; 
          battery_pos=0;

return (1);
}


// -----------------------------------------------------------------------------------------------------
// READ SIM7000 GPS from AT+CGPSINF output and put output to 'lattitude' and 'longtitude' buffers
// -----------------------------------------------------------------------------------------------------
uint8_t readSIM7000gps()
{
  uint16_t char1;
  uint8_t i;
  
  // 'i' is a safe fuse not to get deadlock on serial port reading
  i = 0;

  // set positions to start of each buffer
   longtitude_pos = 0;
   latitude_pos = 0;
 
   uart_puts_P(GPSINFO);      // try to retrieve GPS position

      // wait for "+CGNSINF:" last sign
      do { 
           char1 = receive_uart();
           i++;
         } while ( (char1 != ':') && (i<20) );
      // check if deadlocked and buffer overrun, there should be +CGPSINF: within first 20 chars
         if (i == 20) return(0);


      // wait for FIRST COMMA sign - we omit GNSPWR info
      do { 
           char1 = receive_uart();
           i++;
         } while ( (char1 != ',') && (i<150) );

      // wait for SECOND COMMA sign - we omit GNS fixation info
      do { 
           char1 = receive_uart();
           i++;
         } while ( (char1 != ',') && (i<150) );

     // wait for THIRD COMMA sign - we omit GNS UTC date-time info
      do { 
           char1 = receive_uart();
           i++;
         } while ( (char1 != ',') && (i<150) );


      // if COMMA detected start to copy the response - LATITUDE comes first
      do  { 
           char1 = receive_uart();
           latitude[latitude_pos] = char1; 
           latitude_pos++;
           i++;
         } while ( (char1 != ',') && (i<150) );
           // put end of string to latitude
           latitude[latitude_pos-1] = NULL; 
           latitude_pos=0;

          // if COMMA detected start to copy the response - LONGTITUDE is second
      do  { 
           char1 = receive_uart();
           longtitude[longtitude_pos] = char1; 
           longtitude_pos++;
           i++;
         } while ( (char1 != ',') && (i<150) );
           longtitude[longtitude_pos-1] = NULL; 
           longtitude_pos=0;

          // now comes ATTITUDE - bypassing
      do  { 
           char1 = receive_uart();
           i++;
         } while ( (char1 != ',') && (i<150) );

 
return (1);
}



// --------------------------------------------------------------------------------------------------------------------
// Power on GPS and retrieve position from GPS and put it to LOC and LATT buffers by calling 'readSIM7000gps' function
// --------------------------------------------------------------------------------------------------------------------
uint8_t readgpsinfo()
{
  uint8_t gpsattempts, gpsfixed;   // counter on attempts to get proper GPS position from SIM7000 - for indoor scenarios
  gpsattempts = 0;
  gpsfixed = 0;



  // if begining/last cycle of continous GPS mode we need to turn on and restart GPS module 
  if ( (continousgps == 1) || (continousgps == 5) )
      {   
           delay_sec(1);
           uart_puts_P(GPSPWRON);      // enable SIM7000 GPS power
           delay_sec(2);  
           uart_puts_P(GPSCLDSTART);   // cold start of SIM7000 GPS
       }
  else  // during continous mode cycle - we dont need to turn on and restart GPS module
       {
           delay_sec(1); 
           // hot start of SIM7000 GPS if needed, otherwise simply poll GPS data
           // uart_puts_P(GPSHOTSTART);   
       }; 
      


  // now we have to wait some time until SIM7000 gets GPS signal, assume we are waiting 10 minutes 
  // check the status in 30 sec intervals and then quit
  do 
  {
          if ( continousgps != 255 )   delay_sec(15);      // in NOT in GUARD mode - wait 15 sec for first fix checking

          uart_puts_P(GPSINFO);   // check GPS status

          if (readline()>0)       // check GPS status response if NOT FIXED
           {
                    gpsfixed = 0;

                    // check if already fixed for GPS coordinates ?
                    memcpy_P(buf, GPSISFIXED, sizeof(GPSISFIXED));   
                    if (is_in_rx_buffer(response, buf, BUFFER_SIZE ) == 1) 
                          {
                           // when CGNSINF: 1,1 - stop checking GPS anymore
                            gpsfixed = 1; 
                          }; 

                      
                    // special treatment for GUARD MODE - if not fix quit immediatelly, because there will be repeatition in 1 min
                    if ( (gpsfixed == 0) && (continousgps == 255) )  return (0);       
			             			  
                    // there was some fix - 3D or 2D, poll the data from GPS
                    if ( gpsfixed == 1)        
                      { 

                       // initialize buffers with zero characters first before reading new values from SIM7000 GNSS
                        memcpy_P(latitudegps, INITLOC, sizeof(INITLOC));
                        memcpy_P(longtitudegps, INITLOC, sizeof(INITLOC));

                        readSIM7000gps();         // poll GPS position and parse to buffers LAT & LONG
                        delay_sec(2); 

                        if (continousgps == 1)         // ... if last GPS cycle or single sequence
                           {  uart_puts_P(GPSPWROFF);  // disable SIM7000 GPS power to save battery
                              delay_sec(1);  
                      }; 
                        return(1);               // succesful GPS position decoding from SIM7000  
                       }                         // end of second IF               
                    else   gpsattempts++;        // if not fixed just increase attempts counter
            };                                   // end of first IF       
     
    } while (gpsattempts<20); // end of DO loop - only 20 attempts in 15 sec intervals to get GPS fixation - 5 minutes of searching

    // GPS position retrieval not succesful - we are disabling GPS/GNSS power 
    delay_sec(2); 
    if (continousgps == 1)         // ... if last GPS cycle or single sequence
       {  uart_puts_P(GPSPWROFF);  // disable SIM7000 GPS power to save battery
          delay_sec(1);  
        }; 

return(0); // 0 means that GPS was unable to fix on sattelites at all
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////
// delay procedure ASM based because _delay_ms() is working bad for 1 MHz clock MCU
/////////////////////////////////////////////////////////////////////////////////////////////////////////////

// delay partucular number of seconds 

void delay_sec(uint8_t i)
{
while(i > 0)
{
// Delay 1 000 000 cycles
// 1s at 1 MHz

asm volatile (
    "    ldi  r18, 6"           "\n"
    "    ldi  r19, 19"           "\n"
    "    ldi  r20, 174"           "\n"
    "1:  dec  r20"           "\n"
    "    brne 1b"           "\n"
    "    dec  r19"           "\n"
    "    brne 1b"           "\n"
    "    dec  r18"           "\n"
    "    brne 1b"           "\n"
    "    rjmp 1f"           "\n"
    "1:"           "\n"
);

i--;  // decrease another second

};    // repeat until i not zero

}

// 
// delay multiplication of 50 microseconds
//

void delay_50usec()
{
// Generated by delay loop calculator
// at http://www.bretmulvey.com/avrdelay.html
// Delay 50 cycles
// 50us at 1 MHz

asm volatile (
    "    ldi  r18, 16"	"\n"
    "1:  dec  r18"	"\n"
    "    brne 1b"	"\n"
    "    rjmp 1f"	"\n"
    "1:"	"\n"
);


}





//////////////////////////////////////////
// SIM7000 initialization procedures
//////////////////////////////////////////

// -------------------------------------------------------------------------------
// wait for first AT in case SIM7000 is starting up
// -------------------------------------------------------------------------------
uint8_t checkat()
{
  uint8_t initialized2;

// wait for first OK while sending AT - autosensing speed on SIM7000, but we are working 9600 bps
// SIM 800L can be set by AT+IPR=9600  to fix this speed
// which I do recommend by connecting SIM7000 to PC using putty and FTD232 cable

                 initialized2 = 0;
              do { 
               uart_puts_P(AT);
                if (readline()>0)
                   {  // check if OK was received
                    memcpy_P(buf, ISOK, sizeof(ISOK));                     
                   if (is_in_rx_buffer(response, buf, BUFFER_SIZE) == 1)  initialized2 = 1;                  
                   }
                else
                   { // maybe ECHO is ON and first line was AT
                    memcpy_P(buf, ISATECHO, sizeof(ISATECHO));                     
                   if (is_in_rx_buffer(response, buf, BUFFER_SIZE) == 1)  initialized2 = 1;                  
                   };

               delay_sec(1);
               } while (initialized2 == 0);

        // send ECHO OFF
                delay_sec(1);
                uart_puts_P(ECHO_OFF);
                delay_sec(1);

             return (1);
}

// -------------------------------------------------------------------------------
// check if PIN is needed and enter PIN 1111 
// -------------------------------------------------------------------------------
uint8_t checkpin()
{

  uint8_t initialized2;
     // readline and wait for PIN CODE STATUS if needed send PIN 1111 to SIM card if required
                  initialized2 = 0;
              do { 
                      delay_sec(2);
                uart_puts_P(SHOW_PIN);
                if (readline()>0)
                   {
                    memcpy_P(buf, PIN_IS_READY, sizeof(PIN_IS_READY));
                  if (is_in_rx_buffer(response, buf, BUFFER_SIZE) == 1)       initialized2 = 1;                                         
                    memcpy_P(buf, PIN_MUST_BE_ENTERED, sizeof(PIN_MUST_BE_ENTERED));
                  if (is_in_rx_buffer(response, buf, BUFFER_SIZE) == 1)     
                        {  
                           delay_sec(1);
                           uart_puts_P(ENTER_PIN);   // ENTER PIN 1111
                           delay_sec(1);
                        };                  
                    };
                  
              } while (initialized2 == 0);

   return (1);
}


// -------------------------------------------------------------------------------
// check if registered to the network
// -------------------------------------------------------------------------------
uint8_t checkregistration()
{
  uint8_t initialized2, attempt2, nbrminutes;
     // readline and wait for STATUS NETWORK REGISTRATION from SIM7000
     // first 2 networks preferred from SIM list are OK
     initialized2 = 0;
     nbrminutes = 0;
     attempt2 = 0;

     // check if already registered first and quit immediately if true
     delay_sec(1);
     uart_puts_P(SHOW_REGISTRATION);
     if (readline()>0)
        {                                    
         memcpy_P(buf, ISREG1, sizeof(ISREG1));
         if (is_in_rx_buffer(response, buf, BUFFER_SIZE) == 1)  return(1); 
         memcpy_P(buf, ISREG2, sizeof(ISREG2));
         if (is_in_rx_buffer(response, buf, BUFFER_SIZE) == 1)  return(1); 
        } 

     // if not registered enable radio.. just in case
                 // TURN ON RADIO IF WAS NOT BEFORE
                 delay_sec(1);
                 uart_puts_P(FLIGHTOFF);  // disable airplane mode - turn on radio and start to search for networks
              do { 
                 delay_sec(120);          // first searching 1 min in case of instability

                 // now after searching check if already registered to 2G GSM
                 uart_puts_P(SHOW_REGISTRATION);

                 if (readline()>0)
                   {                                    
                    memcpy_P(buf, ISREG1, sizeof(ISREG1));
                   if (is_in_rx_buffer(response, buf, BUFFER_SIZE) == 1)  initialized2 = 1; 
                    memcpy_P(buf, ISREG2, sizeof(ISREG2));
                   if (is_in_rx_buffer(response, buf, BUFFER_SIZE) == 1)  initialized2 = 1; 

                  
                  // if not registered do a backoff for 1 hour, maybe in underground garage or something
                   
                   if (initialized2 == 0)
                     {  
                      // if not registered or something wrong turn off RADIO for  minutes 
                      // this is not to drain battery in underground garage 
                      delay_sec(1);
                      uart_puts_P(FLIGHTON);    // enable airplane mode - turn off radio
                      delay_sec(1);
                      uart_puts_P(GPSPWROFF); 
                    // enter SLEEP MODE of SIM800L for power saving when no coverage 
                      delay_sec(1);
                      uart_puts_P(SLEEPON); 
                     // now wait XX min before turning on radio again, here XX = 30 min
                       for (nbrminutes = 0; nbrminutes<30; nbrminutes++) 
                          { 
                          delay_sec(60); 
                          };  
   
                      // Wake up SIM800L and search for network again                  

                        // disable SLEEPMODE 
                        PORTC &= ~_BV(PC5);  // Toggle LOW the DTR pin of SIM7000
                        // send first dummy AT command
                        uart_puts_P(AT);
                        delay_sec(1); 
                        uart_puts_P(SLEEPOFF);  // switch off to SLEEPMODE = 0
                        delay_sec(1); 
                        PORTC |= _BV(PC5);   // Toggles again HIGH the DTR pin
                        delay_sec(1); 
                        uart_puts_P(FLIGHTOFF);  // disable airplane mode - turn on radio and start to search for networks
                      }; // end of no-coverage IF

                     
                   }

                attempt2++; // increase number of attempts - max 24 attempts = 12 hours of no signal 2G network

                // end of DO loop
                } while ( (initialized2 == 0) && (attempt2 < 24) );

      return initialized2;
}
 


//////////////////////////////////////////////////////////////////////////////////
// POWER SAVING mode on ATMEGA 328P handling to reduce the battery consumption
// this part of code can be used ONLY if you have SIM7000 RI/RING PIN connected
// to ATMEGA D2/INT0 interrupt input - otherwise program will hang
//////////////////////////////////////////////////////////////////////////////////

void sleepnow(void)
{

    set_sleep_mode(SLEEP_MODE_PWR_DOWN);

    sleep_enable();

    DDRD &= ~(1 << DDD2);     // Clear the PD2 pin
    // PD2 (PCINT0 pin) is now an input

    PORTD |= (1 << PORTD2);    // turn On the Pull-up
    // PD2 is now an input with pull-up enabled

    // stop interrupts for configuration period
    cli(); 

    // update again INT0 conditions
    EICRA &= ~(1 << ISC01);    // set INT0 to trigger on low level
    EICRA &= ~(1 << ISC00);    // set INT0 to trigger on low level
    EIMSK |= (1 << INT0);     // Turns on INT0 (set bit)

    sei();                         //ensure interrupts enabled so we can wake up again

    sleep_cpu();                   //go to sleep

    // MCU ATTMEGA328P sleeps here until INT0 interrupt

    sleep_disable();               //wake up here

}

// when interrupt from INT0 disable next interrupts from RING pin of SIM7000 and go back to main code
ISR(INT0_vect)
{

   EIMSK &= ~(1 << INT0);     // Turns off INT0 (clear bit)
}




// *********************************************************************************************************
//
//                                                    MAIN PROGRAM
//
// *********************************************************************************************************

int main(void) {

  uint8_t initialized, ringrcvd,  attempt, gpsdataavailable,  char1;
  double   latdiff, longdiff;
  uint32_t nbr50useconds;
  uint32_t nbrseconds;

  attempt = 0;           // number of attempts for GPRS connections
  initialized = 0;       // flag for getting in-out of loops
  ringrcvd = 0;          // flag if there was anything valuable received
 
  latdiff = 0;           // calculation of position change for GUARD MODE
  longdiff = 0;          // calculation of position change for GUARD MODE  
 
  nbr50useconds = 0;     // timer for checking serial port activities or SIM7000 chip sanity
  nbrseconds = 0;        // used for delay function 

  continousgps = 0;      // distinguish between modes : 1 = single, 5 = multi, 255 = guard
  gpsdataavailable = 0;  // flag if real GPS data from SIM7000 were OK
 
 
  // enable INPUT on INT0 / PD2 (ATMEGA PIN #4) to connect RI/RING from SIM7000 if available
  DDRD &= ~(1 << DDD2);     // Clear the PD2 pin -  PD2 (PCINT0 pin) is now an input
  PORTD |= (1 << PORTD2);    // turn On the Pull-up - PD2 is now an input with pull-up enabled

  // enable output to control DTR signal of SIM7000 
  // ATMEGA pin PC5 must be connected to DTR/SLEEP signal of SIM7000 board
  DDRC |= (1<<5) ; // set OUTPUT mode for PC5
  // PORTC &= ~_BV(PC5);  // Toggle LOW the DTR pin of SIM7000
  PORTC |= _BV(PC5);   // Toggles HIGH the DTR pin
 
  // pull up RXD input on ATMEGA
  //DDRD &= ~(1 << DDD0);     // Clear the PD2 pin
  // PD2 (PCINT0 pin) is now an input
  //PORTD |= (1 << PORTD0);    // turn On the Pull-up
  // PD2 is now an input with pull-up enabled

  // initialize variables
  memcpy_P(latitudegpsold, INITLOC, sizeof(INITLOC));
  memcpy_P(latitudegps, INITLOC, sizeof(INITLOC));
  memcpy_P(longtitudegpsold, INITLOC, sizeof(INITLOC));
  memcpy_P(longtitudegps, INITLOC, sizeof(INITLOC));
   

  // initialize 9600 baud 8N1 RS232
  init_uart();

  // delay 10 seconds for safe SIM7000 startup and network registration
  delay_sec(10);

  // turno of ECHO proactively
  uart_puts_P(ECHO_OFF);
  delay_sec(1);
      
  // try to communicate with SIM7000 over AT
  checkat();
  delay_sec(1);

  // Fix UART speed to 9600 bps to disable autosensing
  uart_puts_P(SET9600); 
  delay_sec(1);

  // TURN ON RADIO IF WAS NOT BEFORE
  uart_puts_P(FLIGHTOFF);
  delay_sec(1);

  // if you have connected SIM7000 board RI/RING pint to ATMEGA328P INT0 pin
  // configure RI PIN activity for URC ( unsolicited messages like restart of the modem or battery low)
  // uart_puts_P(CFGRIPIN);
  // delay_sec(2);

  // disable reporting URC of losing 2G coverage by +CREG=0
  uart_puts_P(DISREGREPORT);
  delay_sec(1);


  // Save settings to SIM7000
  uart_puts_P(SAVECNF);
  delay_sec(3);

  // check PIN status 
  checkpin();
  delay_sec(1); 

  // delete all previous SMSes and SMS confirmation to keep SIM7000 memory empty   
  uart_puts_P(SMS1);
  delay_sec(1); 
  uart_puts_P(DELSMS);

  // delay another 90 sec to search for GSM network
  delay_sec(90);

  // check registration status 
  checkregistration();
 
  // neverending LOOP

    while (1) {

        do { 
                // WAIT FOR RING message - incoming voice call and send SMS or restart RADIO module if no signal
                   initialized = 0;
                   ringrcvd = 0;
                   continousgps = 0;
                   nbr50useconds = 0UL;
                   nbrseconds = 0;
                
                // marker for SIM7000 GPS data availability  
                   gpsdataavailable = 0;
                   delay_sec(1);

                // set mode to display incoming SMS, will be needed for retrieval of originating MSISDN 
                   uart_puts_P(SMS1);
                   delay_sec(1); 
                   uart_puts_P(SHOWSMS); 
                   delay_sec(1);

                // OPTIONAL
                // Disable LED blinking on  SIM7000
                //   uart_puts_P(DISABLELED);
                //   delay_sec(2);

               // enter SLEEP MODE #1 of SIM7000 for power saving 
               // ( will be interrupted by incoming voice call or SMS ) and RING URC
                    uart_puts_P(GPSPWROFF); 
                    delay_sec(1);
                    uart_puts_P(SLEEPON); 
                    delay_sec(2);
     
               // ONLY if you have connected SIM7000 RI/RING pin to ATMEGA328P INT0 pin
               // otherwise program will HANG here
               // enter SLEEP MODE on ATMEGA328P for power saving  
               //
               //  
               //  if using sleepnow() do not use periodic check of RI/RIN below
               //
               //    sleepnow(); // sleep function called here 


               // if NOT USING sleepnow() and if RI/RING pin is available on SIM7000 board and connected to INT0 ATMEGA
               // we can periodically check RI/RING pin status  in 1 sec intervals
               // RI is held LOW by SIM7000 until call is completely answered / disconnected
               // because there is 1sec sampling we will probably not be able to detect 120ms RI change for incoming SMS / other URC
               // The module SIM7000 will be periodically woken up to see coverage status - once per 1 hour

               /* OPTIONAL CODE ONLY WHEN SIM7000 RI/RING IS CONNECTED TO INT0 ATMEGA328P

                   while(initialized == 0)
                            {
                              // check if RING/INT0/D2 PIN IS LOW - if yes exit WHILE LOOP and proceed witch Call/SMS
                              if ((PIND & (1 << PD2)) == 0)      
                                 {initialized = 1; }
                              else
                                 {    // RING signal is HIGH
                                     nbrseconds++;               // increase number of seconds waited
                                     delay_sec(1);               // wait another second
                                     // if 1 hour passed we need to check if there is need to turn off 2G for longer time
                                     if (nbrseconds == 3600)
                                          { // if 3600sec passed, we need to check 2G network coverage
                                            nbrseconds = 0;
                                            // disable SLEEPMODE 
                                            PORTC &= ~_BV(PC5);  // Toggle LOW the DTR pin of SIM7000
                                            // send first dummy AT command
                                            uart_puts_P(AT);
                                            delay_sec(1); 
                                            uart_puts_P(SLEEPOFF);  // switch off to SLEEPMODE = 0
                                            delay_sec(1); 
                                            PORTC |= _BV(PC5);   // Toggles again HIGH the DTR pin
                                            delay_sec(1);
                                            //  check 2G coverage
                                            checkregistration();
                                            // enter SLEEP MODE of SIM7000 again 
                                            delay_sec(1);
                                            uart_puts_P(SLEEPON); 
                                            delay_sec(1);
                                            // clear the flag that there was no RING
                                            initialized = 0;
                                          };
                                  };  // end of checking PIN D2 (INT0)
                             };  // end of WHILE for checking 2G coverage

                    END OF OPTIONAL CODE WHEN SIM 808 RI/RING CONNECTED WITH INT0 ATMEGA */

               // THERE WAS RI / INT0 INTERRUPT (when RI/RING connected to INT0) 
               // OR SOMETHING WAS RECEIVED OVER SERIAL (URC) 
               // WE NEED TO GET OFF SLEEPMODE AND READ SERIAL PORT


             // If RI/RING is unavailable then we are waiting for something from uart
                nbr50useconds = 0UL;
             // first empty RX buffer just in case
                while (UCSR0A & (1<<RXC0)) char1 = UDR0; 

             // then wait for something valuable and increase timer  
                while(initialized == 0)
                            {
                              // check if something from serial port received 
                              // - if yes exit WHILE LOOP and proceed witch Call/SMS
                              // we are probing serial port every ~50-100 useconds not to lose any character
                              if (UCSR0A & (1<<RXC0))      
                                 {initialized = 1;
                                  ringrcvd = 0; }
                              else
                                 {    // there was NOTHING received over serial port so we are still waiting...
                                      //
                                     nbr50useconds++;           // increase number of 50useconds waited
                                     delay_50usec();            // wait another 50usec
                                     // if something like 15min ~ 30min passed 
                                     // we need to check if there is need to turn off 2G for longer time
                                     if (nbr50useconds == 18000000UL)
                                          { // if specified amount of second passed, we need to check 2G network coverage
                                            nbr50useconds = 0UL;
                                            // disable SLEEPMODE 
                                            PORTC &= ~_BV(PC5);  // Toggle LOW the DTR pin of SIM7000
                                            // send first dummy AT command
                                            uart_puts_P(AT);
                                            delay_sec(1); 
                                            uart_puts_P(SLEEPOFF);  // switch off to SLEEPMODE = 0
                                            delay_sec(1); 
                                            PORTC |= _BV(PC5);   // Toggles again HIGH the DTR pin
                                            delay_sec(1);
                                            //  check 2G coverage
                                            checkregistration();
                                            // enter SLEEP MODE of SIM7000 again
                                            delay_sec(1);
                                            uart_puts_P(SLEEPON); 
                                            delay_sec(1);
                                            // clear the flag that there was no RING
                                            initialized = 0;
                                            ringrcvd = 0;
                                            continousgps = 0;
                                            // empty RX buffer just in case
                                            while (UCSR0A & (1<<RXC0)) char1 = UDR0; 
                                          };
                                  };  // end of checking USART status
                             };  // end of WHILE for checking 2G coverage



                if (readline()>0)
                    {
                    // check if this is an SMS message first

                    memcpy_P(buf, ISSMS, sizeof(ISSMS));  
                    if  ( (is_in_rx_buffer(response, buf, BUFFER_SIZE) == 1) && (ringrcvd == 0) )  
                            { 
                                 // extract TXT SMS content first not to lose incoming serial port characters... timing issue...
                                 readsmstxt();
                                 // then we need to extract phone number from SMS message RESPONSE buffer
                                 readsmsphonenumber(); 

                                 // clear the flags first
                                 initialized = 0;
                                 ringrcvd = 0; 
                                 continousgps = 0; 

                                 // checking if there is "MULTI" word in SMS content buffer
                                 memcpy_P(buf, ISMULTI, sizeof(ISMULTI));
                                 // convert to upper char  
                                 if   (is_in_rx_buffer(strupr(smstext), buf, BUFFER_SIZE) == 1)  
                                     {
                                        // disable SLEEPMODE 
                                        PORTC &= ~_BV(PC5);  // Toggle LOW the DTR pin of SIM7000
                                        // send first dummy AT command
                                        uart_puts_P(AT);
                                        delay_sec(1); 
                                        uart_puts_P(SLEEPOFF);  // switch off to SLEEPMODE = 0
                                        delay_sec(1); 
                                        PORTC |= _BV(PC5);   // Toggles again HIGH the DTR pin
                                        delay_sec(1);

                                       // send a SMS configrmation of the command
                                        uart_puts_P(SMS1);
                                        delay_sec(1); 
                                        // compose an SMS from fragments - interactive mode CTRL Z at the end
                                        uart_puts_P(SMS2);
                                        uart_puts(phonenumber);  // send phone number received from CLIP
                                        uart_puts_P(CRLF);                                       
                                        delay_sec(1); 
                                        uart_puts_P(COMMANDMULTIACK);
                                       // end the SMS message
                                        send_uart(26);   // ctrl Z to end SMS
                                        delay_sec(10); 
 
                                        // delete all previous SMSes and SMS confirmation to keep SIM7000 memory empty   
                                        uart_puts_P(SMS1);
                                        delay_sec(1); 
                                        uart_puts_P(DELSMS);
                                        delay_sec(2);

                                        // mark 'initialized' flag to further proceed outside do-while loop
                                        // send number of GPS polling to 5
                                        initialized = 1; 
                                        ringrcvd = 1;
                                        continousgps = 5; 
                                     };


                                 // checking if there is "SINGLE" word in SMS content buffer
                                 memcpy_P(buf, ISSINGLE, sizeof(ISSINGLE));  
                                 if   (is_in_rx_buffer(strupr(smstext), buf, BUFFER_SIZE) == 1)  
                                     {
                                        // disable SLEEPMODE 
                                        PORTC &= ~_BV(PC5);  // Toggle LOW the DTR pin of SIM7000
                                        // send first dummy AT command
                                        uart_puts_P(AT);
                                        delay_sec(1); 
                                        uart_puts_P(SLEEPOFF);  // switch off to SLEEPMODE = 0
                                        delay_sec(1); 
                                        PORTC |= _BV(PC5);   // Toggles again HIGH the DTR pin
                                        delay_sec(1);

                                       // send a SMS configrmation of the command
                                        uart_puts_P(SMS1);
                                        delay_sec(1); 
                                        // compose an SMS from fragments - interactive mode CTRL Z at the end
                                        uart_puts_P(SMS2);
                                        uart_puts(phonenumber);  // send phone number received from CLIP
                                        uart_puts_P(CRLF);                                       
                                        delay_sec(1); 
                                        uart_puts_P(COMMANDSINGLEACK);
                                       // end the SMS message
                                        send_uart(26);   // ctrl Z to end SMS
                                        delay_sec(10); 
 
                                        // delete all previous SMSes and SMS confirmation to keep SIM7000 memory empty   
                                        uart_puts_P(SMS1);
                                        delay_sec(1); 
                                        uart_puts_P(DELSMS);
                                        delay_sec(2);

                                        // mark 'initialized' flag to further proceed outside do-while loop
                                        // send number of GPS polling to 5
                                        initialized = 1; 
                                        ringrcvd = 1;
                                        continousgps = 1; 
                                     };

                                   
                                 // checking if there is "ACTIVATE" word in SMS content buffer
                                 memcpy_P(buf, ISACTIVATE, sizeof(ISACTIVATE));  
                                 if   (is_in_rx_buffer(strupr(smstext), buf, BUFFER_SIZE) == 1)  
                                     {
                                        // disable SLEEPMODE 
                                        PORTC &= ~_BV(PC5);  // Toggle LOW the DTR pin of SIM7000
                                        // send first dummy AT command
                                        uart_puts_P(AT);
                                        delay_sec(1); 
                                        uart_puts_P(SLEEPOFF);  // switch off to SLEEPMODE = 0
                                        delay_sec(1); 
                                        PORTC |= _BV(PC5);   // Toggles again HIGH the DTR pin
                                        delay_sec(1);

                                        // write 20 bytes of phonenumber to EEPROM at address EEADDR
                                        eeprom_write_block((const void *)phonenumber, (void *)EEADDR, 20);   

                                       // send a SMS configrmation of the command
                                        uart_puts_P(SMS1);
                                        delay_sec(1); 
                                        // compose an SMS from fragments - interactive mode CTRL Z at the end
                                        uart_puts_P(SMS2);
                                        uart_puts(phonenumber);  // send phone number received from CLIP
                                        uart_puts_P(CRLF);                                       
                                        delay_sec(1); 
                                        uart_puts_P(ACTIVATED);  // send string Activated calls from...
                                        uart_puts(phonenumber);  // number copied from SMS sender

                                       // end the SMS message
                                        send_uart(26);   // ctrl Z to end SMS
                                        delay_sec(10); 
 
                                        // delete all previous SMSes and SMS confirmation to keep SIM7000 memory empty   
                                        uart_puts_P(SMS1);
                                        delay_sec(1); 
                                        uart_puts_P(DELSMS);
                                        delay_sec(2);

                                        // mark 'initialized' flag to further proceed outside do-while loop
                                        // send number of GPS polling to 5
                                        initialized = 0; 
                                        ringrcvd = 1;
                                        continousgps = 0; 
                                     };


                                 // checking if there is "GUARD" word in SMS content buffer
                                 memcpy_P(buf, ISGUARD, sizeof(ISGUARD));  
                                 if   (is_in_rx_buffer(strupr(smstext), buf, BUFFER_SIZE) == 1)  
                                    {
                                        // disable SLEEPMODE 
                                        PORTC &= ~_BV(PC5);  // Toggle LOW the DTR pin of SIM7000
                                        // send first dummy AT command
                                        uart_puts_P(AT);
                                        delay_sec(1); 
                                        uart_puts_P(SLEEPOFF);  // switch off to SLEEPMODE = 0
                                        delay_sec(1); 
                                        PORTC |= _BV(PC5);   // Toggles again HIGH the DTR pin
                                        delay_sec(1);

                                       // send a SMS configrmation of the command
                                        uart_puts_P(SMS1);
                                        delay_sec(1); 
                                        // compose an SMS from fragments - interactive mode CTRL Z at the end
                                        uart_puts_P(SMS2);
                                        uart_puts(phonenumber);  // send phone number received from CLIP
                                        uart_puts_P(CRLF);                                       
                                        delay_sec(1); 
                                        uart_puts_P(GUARD);  // send string GUARD MODE
 
                                       // end the SMS message
                                        send_uart(26);   // ctrl Z to end SMS
                                        delay_sec(10); 
 
                                        // delete all previous SMSes and SMS confirmation to keep SIM7000 memory empty   
                                        uart_puts_P(SMS1);
                                        delay_sec(1); 
                                        uart_puts_P(DELSMS);
                                        delay_sec(2);

                                        // mark 'initialized' flag to further proceed outside do-while loop
                                        // send number of GPS polling to 255 which means this is GUARD MODE
                                        initialized = 1; 
                                        ringrcvd = 1;
                                        continousgps = 255; 

                                        // enable GPS to poll data during GUARD MODE
                                        uart_puts_P(GPSPWRON);      // enable SIM7000 GPS power
                                        delay_sec(2);  
                                        uart_puts_P(GPSCLDSTART);   // cold start of SIM7000 GPS
                                        delay_sec(1);
                                    };  // end of GUARD IF

                            };  // end of ISSMS IF  


 
                    // if some other message than "RING" or SMS check if network is avaialble and SIM7000 is operational  
                    // maybe power loss or something
                    if  (ringrcvd == 0) 
                      {
                        // disable SLEEPMODE                  
                        // first pull LOW DTR pin for AT LEAST 50 miliseconds
                        PORTC &= ~_BV(PC5);  // Toggle LOW the DTR pin of SIM7000

                        // send first dummy AT command
                        uart_puts_P(AT);
                        delay_sec(1); 

                        uart_puts_P(SLEEPOFF);  // switch off to SLEEPMODE = 0
                        delay_sec(1); 

                        PORTC |= _BV(PC5);   // Toggles again HIGH the DTR pin

                        delay_sec(1);
 
                        // check status of all functions, just in case the SIM7000 restarted itself 
                        checkpin();
                        // check if network was lost and we have to search for it
                        checkregistration();
                        delay_sec(1);
                        // delete all SMSes and SMS confirmation to keep SIM7000 memory empty   
                        uart_puts_P(SMS1);
                        delay_sec(1); 
                        uart_puts_P(DELSMS);
                        delay_sec(2);


                        // there was something different than RING so we need to go back to the beginning - clear the flag
                        initialized = 0;
                        ringrcvd = 0; 
                        continousgps = 0; 
                      }; // end of LAST IF

                }; // END of READLINE IF
                 
        } while ( initialized == 0);    // end od DO-WHILE, go to begging and enter SLEEPMODE again 




               // GPS POLLLING & SMS SENDING LOOP - WE WILL DO AS MANY TIMES AS
               // "gpscontinous" VARIABLE IS SET 
         
        do  {
                   
               // clear the 'initialized' and 'gpsdataavailable' flags 
               initialized = 0;
               gpsdataavailable = 0; 

               // call polling from GPS SIM7000, if it was successful mark it by flag 'gpsdataavailable'

               if (readgpsinfo()==1)
                  { 
                   // mark flag that GPS data are available 
                   gpsdataavailable = 1; 

                   // THERE IS NO NEED TO CONVERT ANY GPS DATA FROM SIM7000 AS IT WAS FOR SIM808 MODULE !
                   // WE SIMPLY COPY LATITUDE & LONGITUDE TO TEXT MESSAGE DIRECTLY 
                   // the longtitude is written as [-180.000000,180.000000] - 11 characters
                   // the latitude is written as [-90.000000,90.000000] - 10 characters
   

                       // copy longtitude for text message
                       memcpy(longtitudegps, longtitude, 11);
                       longtitudegps[11] = 0x00;
                       // put NULL at the end
                                               
                       // we will be counting from the last digit
                       memcpy(latitudegps, latitude, 10);  
                       latitudegps[10] = 0x00;
                 
                   };  // END OF GPSINFO IF




                 // if CELL GPS data availbale or real SIM7000 GPS data available and NOT GUARD MODE 
                 // then proceed with sending SMS                   
                 // avoid sending SMS with no usable information
 
                if (  (  ( gpsdataavailable == 1) ) && (continousgps != 255) )
                   {
                          // check battery voltage
                          delay_sec(1);
                          uart_puts_P(CHECKBATT);
                          readbattery();

                          // send a SMS in plain text format
                          delay_sec(1); 
                          uart_puts_P(SMS1);
                          delay_sec(1); 
                          // compose an SMS from fragments - interactive mode CTRL Z at the end
                          uart_puts_P(SMS2);
                          uart_puts(phonenumber);        // send phone number received from SMS
                          uart_puts_P(CRLF);                                       
                          delay_sec(1); 
                          // send LONGTITUDE
                          uart_puts_P(LONG);                      // send longtitude string
                          uart_puts(longtitudegps);               // send REAL GPS info about LAONGTITUDE
                          // send LATTITUDE
                          uart_puts_P(LATT);                      // send lattitude string
                          uart_puts(latitudegps);                 // send REAL GPS info about LATTITUDE
                          // put battery info
                          uart_puts_P(BATT);                      // send BATTERY VOLTAGE in milivolts
                          uart_puts(battery);                     // from buffer
                          // put link to GOOGLE MAPS
                          uart_puts_P(GOOGLELOC1);                // send http ****
                          uart_puts(latitudegps);                 // send REAL GPS info about LATTITUDE
                          uart_puts_P(GOOGLELOC2);                // send comma
                          uart_puts(longtitude);                  // send Google API LONGTITUDE;
                          uart_puts_P(GOOGLELOC3);                // send CRLF
                          delay_sec(1); 
                          // end the SMS message
                          send_uart(26);                          // ctrl Z to end SMS and send it over the air

                   };   // end of IF for gpsavailable available
                 


                    // Checking if this is first pass of guard and there is any previous GPS position
                    // if first GPS checking then previous position is '0'
                         latdiff = atof(latitudegpsold);
                         longdiff = atof(longtitudegpsold);


                 // if CELL GPS data available or real SIM7000 GPS data available and GUARD MODE enabled
                 // check if GPS position has changed
                 // avoid sending SMS with no usable information
                if (  ( gpsdataavailable == 1) && (continousgps == 255) && (latdiff != 0) && (longdiff !=0) )
                   {
                    // Now comparing numbers of Latitude and Longtitude to calculate position difference
                         latdiff = atof(latitudegpsold) - atof(latitudegps);
                         longdiff = atof(longtitudegpsold) - atof(longtitudegps);

 
                     // if necessary get rid of 'minus' sign no to get false positives
                     if (latdiff < 0)  latdiff = 0 - latdiff;
                     if (longdiff < 0)  longdiff = 0 - longdiff;

                    // if difference greater than 3 hundread of meters... this value can be modified for SENSIVITY
                    if ( ( longdiff > 0.0027 )  ||  ( latdiff > 0.0027 ) ) 
                          // if GPS movement detected send ALERT
                        {
                          // send a SMS in plain text format
                          delay_sec(1); 
                          uart_puts_P(SMS1);
                          delay_sec(1); 
                          // compose an SMS from fragments - interactive mode CTRL Z at the end
                          uart_puts_P(SMS2);
                          uart_puts(phonenumber);  // send phone number received from CLIP
                          uart_puts_P(CRLF);                                       
                          delay_sec(1); 

                          uart_puts_P(ALERT);                                  // send ALERT info

                          // put link to GOOGLE MAPS
                          uart_puts_P(GOOGLELOC1);                            // send http ****
                          uart_puts(latitudegps);                             // send REAL GPS info about LATITUDE
                          uart_puts_P(GOOGLELOC2);                            // send comma
                          uart_puts(longtitudegps);                           // send REAL GPS info about LONGTITUDE
                          uart_puts_P(GOOGLELOC3);                            // send CRLF
                          delay_sec(1); 
                          // end the SMS message
                          send_uart(26);                                      // ctrl Z to end SMS and send it over the air

                        // clear continousgps flag by setting to 1 - get out of GUARD MODE
                          delay_sec(10);
                          continousgps = 1;      
                        // disable GPS to conserve power after quitting GUARD MODE
                          uart_puts_P(GPSPWROFF);  // disable SIM7000 GPS after quiting GUARD MODE
                          delay_sec(1);  
                        // delete all SMSes and SMS confirmation to keep SIM7000 memory empty   
                          uart_puts_P(SMS1);
                          delay_sec(1); 
                          uart_puts_P(DELSMS);
                          delay_sec(2);
                       };  // end of IF 
 
                  }; // end of IF for continous GPS = 255

                // copy current GPS position as old GPS position for comparision during GUARD mode
                   memcpy(latitudegpsold, latitudegps, 10);  
                   memcpy(longtitudegpsold, longtitudegps, 11);  
                   longtitudegpsold[11] = 0x00;
                   latitudegpsold[10] = 0x00;


                // decrease continousgps attempt number, this is global variable also checked in GPS procedures
                   if ( continousgps != 255) continousgps-- ;

                // wait ten seconds or longer (if continous mode)
                // just in case SMS confirmation was received not to trigger anything
                   if ( (continousgps > 0) && (continousgps<255) )
                        {                            
                          // if in GPS continous mode delay XX minutes before next attempt
                          delay_sec(60);
                          delay_sec(60);
                          delay_sec(60);
                         };
			
                // if ending sequence of GPS checking simply wait until SMS is delivered
                   if ( continousgps == 0)   delay_sec(10);  


                // if GUARD MODE is active check if incoming STOP text message
                   if ( continousgps == 255)     
                          {   // wait 1 minute for SMS to stop GUARD MODE

                                delay_sec(1);    // delay to empty serial buff
            					   // first empty serial buffer - read all unnecessary chars
                                while (UCSR0A & (1<<RXC0)) char1 = UDR0; 

                                nbr50useconds = 0UL;           // clear time counter first
                                initialized = 0;               // flag for something on serial port

                                 // wait until something received from serial port or 1 minute passed
                                do 
                                    {
                                     nbr50useconds++;               // increase number of 50useconds waited
                                     delay_50usec();                // wait another 50usec
                                     if ( UCSR0A & (1<<RXC0))          initialized = 1;     // if something on serial port quit waiting
                                     if ( nbr50useconds == 800000UL )  initialized = 2;     // if ~60 sec reached
                                    }  while ( initialized == 0);   // end of WHILE for SMS and delaying 1 sec


                                if ( initialized == 1 )      // if something on serial port we need to check it
                                   {  
                                      initialized = 0;       // clear serial port  flag 
                                     if (readline()>0)       // there is something on serial port...
                                      {
                                         // check if this is an SMS message first
                                          memcpy_P(buf, ISSMS, sizeof(ISSMS));  
                                          if  ( is_in_rx_buffer(response, buf, BUFFER_SIZE) == 1)  
                                              { 
                                              // extract TXT SMS content first not to lose incoming serial port characters... timing issue...
                                              readsmstxt();
                                              // then we need to extract phone number from SMS message RESPONSE buffer
                                              readsmsphonenumber(); 

                                              // checking if there is "STOP" word in SMS content buffer
                                              memcpy_P(buf, ISSTOP, sizeof(ISSTOP));
                                              // convert to upper char  
                                              if   (is_in_rx_buffer(strupr(smstext), buf, BUFFER_SIZE) == 1)  
                                                   {
                                                    // send a SMS configrmation of the command
                                                    uart_puts_P(SMS1);
                                                    delay_sec(1); 
                                                    // compose an SMS from fragments - interactive mode CTRL Z at the end
                                                    uart_puts_P(SMS2);
                                                    uart_puts(phonenumber);  // send phone number received from CLIP
                                                    uart_puts_P(CRLF);                                       
                                                    delay_sec(1); 
                                                    uart_puts_P(STOP);
                                                    // end the SMS message
                                                    send_uart(26);   // ctrl Z to end SMS
                                                    delay_sec(10); 
                                                    // delete all previous SMSes and SMS confirmation to keep SIM7000 memory empty   
                                                    uart_puts_P(SMS1);
                                                    delay_sec(1); 
                                                    uart_puts_P(DELSMS);
                                                    delay_sec(2);
                                                    // disable GPS to conserve power after quitting GUARD MODE
                                                    uart_puts_P(GPSPWROFF);  // disable SIM7000 GPS after quiting GUARD MODE
                                                    delay_sec(1);   
                                                    // send number of GPS polling to 1 to disable GUARD MODE
                                                    continousgps = 0; 
                                                   };  // end of STOP IF

                                               }; // end of IS SMS IF
 
                                         }; // END of READLINEIF

                                    };  // END of IF INITIALIZED

                    };  // end of IF CONTINOUSGPS

                          
 
        // END OF CONTINOUS GPS LOOP
        } while ( continousgps > 0);
  
        // go to the beginning and enter sleepmode on SIM7000 and ATMEGA328P again for power saving

        // end of neverending loop
      };

 
    // end of MAIN code 
}
