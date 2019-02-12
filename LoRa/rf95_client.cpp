// rf95_client.cpp
//
// Example program showing how to use RH_RF95 on Raspberry Pi
// Uses the bcm2835 library to access the GPIO pins to drive the RFM95 module
// Requires bcm2835 library to be already installed
// http://www.airspayce.com/mikem/bcm2835/
// Use the Makefile in this directory:
// cd example/raspi/rf95
// make
// sudo ./rf95_client
//
// Contributed by Charles-Henri Hallard based on sample RH_NRF24 by Mike Poublon

#include <bcm2835.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <mosquitto.h>
#include <openssl/aes.h>
#include <openssl/evp.h>

#include <RH_RF69.h>
#include <RH_RF95.h>

#define BLOCK_SIZE 16
// define hardware used change to fit your need
// Uncomment the board you have, if not listed 
// uncommment custom board and set wiring tin custom section

// LoRasPi board 
// see https://github.com/hallard/LoRasPI
#define BOARD_DRAGINO_PIHAT

// iC880A and LinkLab Lora Gateway Shield (if RF module plugged into)
// see https://github.com/ch2i/iC880A-Raspberry-PI
//#define BOARD_IC880A_PLATE

// Raspberri PI Lora Gateway for multiple modules 
// see https://github.com/hallard/RPI-Lora-Gateway
//#define BOARD_PI_LORA_GATEWAY

// Dragino Raspberry PI hat
// see https://github.com/dragino/Lora
//#define BOARD_DRAGINO_PIHAT

// Now we include RasPi_Boards.h so this will expose defined 
// constants with CS/IRQ/RESET/on board LED pins definition
#include "../RasPiBoards.h"

// Our RFM95 Configuration 
#define RF_FREQUENCY  868.00
#define RF_GATEWAY_ID 1 
#define RF_NODE_ID    10

// Create an instance of a driver
RH_RF95 rf95(RF_CS_PIN, RF_IRQ_PIN);
//RH_RF95 rf95(RF_CS_PIN);

//Flag for Ctrl-C
volatile sig_atomic_t force_exit = false;

struct mosquitto *mosq = NULL;

static void hex_print(const void* pv, size_t len)
{
    const unsigned char * p = (const unsigned char*)pv;
    if (NULL == pv)
        printf("NULL");
    else
    {
        size_t i = 0;
        for (; i<len;++i)
            printf("%02X ", *p++);
    }
    printf("\n");
}




void sig_handler(int sig)
{
  printf("\n%s Break received, exiting!\n", __BASEFILE__);
  mosquitto_destroy(mosq);
  mosquitto_lib_cleanup();
  exit(0) ;
}

// MQTT listener for recieved messages
void my_message_callback(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *message)
{	
	if(message->payloadlen){
		// when recieving a message through MQTT, we cipher it using the hardcoded AES key
		// then we send the message with LoRa
		printf("%s %s\n", message->topic, message->payload);
		uint8_t* data = (uint8_t*) message->payload;
		const unsigned char* dataLen = (const unsigned char*) message->payload;


		const size_t encslength = ((message->payloadlen + BLOCK_SIZE) / BLOCK_SIZE) * BLOCK_SIZE;

		// hardcoded AES key
		char* userkey = "the_cake_is_alie";

		unsigned char IV[BLOCK_SIZE];
		memset(IV, 0, BLOCK_SIZE);

		AES_KEY enc_key;

		int ret = AES_set_encrypt_key((unsigned char*) userkey, BLOCK_SIZE*8, &enc_key);

		if(ret<0){
			printf("Error setting up key\n");
			exit(-1);
		}

		unsigned char dataEnc[encslength];
		AES_cbc_encrypt(dataLen, dataEnc, BLOCK_SIZE, &enc_key, IV, AES_ENCRYPT);

   		uint8_t len = sizeof(dataEnc);
    
		printf("Sending %02d bytes to node #%d => ", len, RF_GATEWAY_ID );
		printbuffer((uint8_t*) dataEnc, len);
		printf("\n" );
		rf95.send(dataEnc, len);
		rf95.waitPacketSent();


	}else{
		printf("%s (null)\n", message->topic);
	}
	fflush(stdout);
}

void my_connect_callback(struct mosquitto *mosq, void *userdata, int result)
{
	int i;
	if(!result){
		/* Subscribe to broker information topics on successful connect. */
		//mosquitto_subscribe(mosq, NULL, "$SYS/#", 2);
	}else{
		fprintf(stderr, "Connect failed\n");
	}
}

void my_subscribe_callback(struct mosquitto *mosq, void *userdata, int mid, int qos_count, const int *granted_qos)
{
	int i;

	printf("Subscribed (mid: %d): %d", mid, granted_qos[0]);
	for(i=1; i<qos_count; i++){
		printf(", %d", granted_qos[i]);
	}
	printf("\n");
}

void my_log_callback(struct mosquitto *mosq, void *userdata, int level, const char *str)
{
	/* Pring all log messages regardless of level. */
	printf("%s\n", str);
}

//Main Function
int main (int argc, const char* argv[] )
{
  static unsigned long last_millis;
  static unsigned long led_blink = 0;
  
  signal(SIGINT, sig_handler);
  printf( "%s\n", __BASEFILE__);

  if (!bcm2835_init()) {
    fprintf( stderr, "%s bcm2835_init() Failed\n\n", __BASEFILE__ );
    return 1;
  }
  
  printf( "RF95 CS=GPIO%d", RF_CS_PIN);

#ifdef RF_LED_PIN
  pinMode(RF_LED_PIN, OUTPUT);
  digitalWrite(RF_LED_PIN, HIGH );
#endif

#ifdef RF_IRQ_PIN
  printf( ", IRQ=GPIO%d", RF_IRQ_PIN );
  // IRQ Pin input/pull down 
  pinMode(RF_IRQ_PIN, INPUT);
  bcm2835_gpio_set_pud(RF_IRQ_PIN, BCM2835_GPIO_PUD_DOWN);
#endif
  
#ifdef RF_RST_PIN
  printf( ", RST=GPIO%d", RF_RST_PIN );
  // Pulse a reset on module
  pinMode(RF_RST_PIN, OUTPUT);
  digitalWrite(RF_RST_PIN, LOW );
  bcm2835_delay(150);
  digitalWrite(RF_RST_PIN, HIGH );
  bcm2835_delay(100);
#endif

#ifdef RF_LED_PIN
  printf( ", LED=GPIO%d", RF_LED_PIN );
  digitalWrite(RF_LED_PIN, LOW );
#endif

  if (!rf95.init()) {
    fprintf( stderr, "\nRF95 module init failed, Please verify wiring/module\n" );
  } else {
    printf( "\nRF95 module seen OK!\r\n");

#ifdef RF_IRQ_PIN
    // Since we may check IRQ line with bcm_2835 Rising edge detection
    // In case radio already have a packet, IRQ is high and will never
    // go to low so never fire again 
    // Except if we clear IRQ flags and discard one if any by checking
    rf95.available();

    // Now we can enable Rising edge detection
    bcm2835_gpio_ren(RF_IRQ_PIN);
#endif

    // Defaults after init are 434.0MHz, 13dBm, Bw = 125 kHz, Cr = 4/5, Sf = 128chips/symbol, CRC on

    // The default transmitter power is 13dBm, using PA_BOOST.
    // If you are using RFM95/96/97/98 modules which uses the PA_BOOST transmitter pin, then 
    // you can set transmitter powers from 5 to 23 dBm:
    //rf95.setTxPower(23, false); 
    // If you are using Modtronix inAir4 or inAir9,or any other module which uses the
    // transmitter RFO pins and not the PA_BOOST pins
    // then you can configure the power transmitter power for -1 to 14 dBm and with useRFO true. 
    // Failure to do that will result in extremely low transmit powers.
    //rf95.setTxPower(14, true);

    rf95.setTxPower(14, false); 

    // You can optionally require this module to wait until Channel Activity
    // Detection shows no activity on the channel before transmitting by setting
    // the CAD timeout to non-zero:
    //rf95.setCADTimeout(10000);

    // Adjust Frequency
    rf95.setFrequency( RF_FREQUENCY );

    // This is our Node ID
    rf95.setThisAddress(RF_NODE_ID);
    rf95.setHeaderFrom(RF_NODE_ID);
    
    // Where we're sending packet
    rf95.setHeaderTo(RF_GATEWAY_ID);  

    printf("RF95 node #%d init OK @ %3.2fMHz\n", RF_NODE_ID, RF_FREQUENCY );

    last_millis = millis();

    //Begin the main body of code

    // Send a message to rf95_server with MQTT callbacks
    //********************************************************************************************************************************************************************

	int i;
	const char *host = "mqtt.server";
	int port = 8883;
	int keepalive = 60;
	bool clean_session = true;

	const char* cafile = "/etc/mosquitto/ca_certificates/ecc.ca.cert.pem";
	const char* certfile = "/etc/mosquitto/certs/server.crt";
	const char* keyfile = "/etc/mosquitto/certs/server.key";

	const char* username = "esp8266";
	const char* pwd = "esp8266";

	mosquitto_lib_init();
	mosq = mosquitto_new(NULL, clean_session, NULL);

	if(!mosq){
		fprintf(stderr, "Error: Out of memory.\n");
		return 1;
	}

	int auth_mqtt = mosquitto_username_pw_set(mosq,username,pwd);

	if(auth_mqtt){
		fprintf(stderr, "Error: Authentification MQTT.\n");
		printf("Code : %d\n",auth_mqtt);
		return 1;
	}

	int result_TLS = mosquitto_tls_set(mosq,cafile,NULL,certfile,keyfile,NULL);
	if(result_TLS){
		fprintf(stderr, "Error: TLS connection.\n");
		printf("Code : %d\n",result_TLS);
		return 1;
	}


	mosquitto_log_callback_set(mosq, my_log_callback);
	mosquitto_connect_callback_set(mosq, my_connect_callback);
	mosquitto_message_callback_set(mosq, my_message_callback);
	mosquitto_subscribe_callback_set(mosq, my_subscribe_callback);

	if(mosquitto_connect(mosq, host, port, keepalive)){
		fprintf(stderr, "Unable to connect.\n");
		return 1;
	}


	int result_subscribe = mosquitto_subscribe(mosq,NULL,"/esp8266",0);
	if(result_subscribe){
		fprintf(stderr, "Error: Subscribe.\n");
		printf("Code : %d\n",result_subscribe);
		return 1;
	}
	mosquitto_loop_forever(mosq, -1, 1);


	// Infinite loop
	printf("Reaching infinite loop\n");

	while (!force_exit) {

      // Send every 5 seconds
      if ( millis() - last_millis > 5000 ) {
        last_millis = millis();

#ifdef RF_LED_PIN
        led_blink = millis();
        digitalWrite(RF_LED_PIN, HIGH);
#endif
                
      }

#ifdef RF_LED_PIN
      // Led blink timer expiration ?
      if (led_blink && millis()-led_blink>200) {
        led_blink = 0;
        digitalWrite(RF_LED_PIN, LOW);
      }
#endif
      
      // Let OS doing other tasks
      // Since we do nothing until each 5 sec
      bcm2835_delay(100);
    }
}

#ifdef RF_LED_PIN
  digitalWrite(RF_LED_PIN, LOW );
#endif
  printf( "\n%s Ending\n", __BASEFILE__ );
  bcm2835_close();
  return 0;
}
