#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>

#include <xbee.h>
#include <MQTTClient.h>

#define XBEE2MQTT_VERSION "v0.2"
#define TRUE 1
#define FALSE 0

#define ADDRESS		"tcp://localhost:1883"
#define CLIENTID	"bhp-xbee"
#define TOPIC		"xbee"
#define QOS		1
#define TIMEOUT		10000L
#define XBEE_TYPE	"xbeeZB"
#define XBEE_DEVICE	"/dev/xbee"
#define XBEE_BAUDRATE	57600

typedef struct stXBeeClient {
    char name[21];
    unsigned char addr16[2];
    unsigned char addr64[8];
} stXBeeClient;

struct xbee *xbee;
struct xbee_conAddress xbee_address;
struct xbee_conSettings xbee_settings;
struct xbee_con *con;
struct xbee_con *con_at;
xbee_err ret;
unsigned char txRet;
unsigned char lastNDFrame;
unsigned char cntClients;
char localName[21];
stXBeeClient xbeeclients[16];

MQTTClient client;
MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
MQTTClient_message pubmsg = MQTTClient_message_initializer;
MQTTClient_deliveryToken token;
char payload[160];
int rc;

char* getName(struct xbee_conAddress address) {
    char *temp = malloc(21);
    int i;

    memset(temp, '\0', strlen(temp));
    for (i=0;i<cntClients;i++) {
	if (address.addr64_enabled) {
	    if ((xbeeclients[i].addr64[0] == address.addr64[0]) &&
		(xbeeclients[i].addr64[1] == address.addr64[1]) &&
		(xbeeclients[i].addr64[2] == address.addr64[2]) &&
		(xbeeclients[i].addr64[3] == address.addr64[3])) {
		strcpy(temp, &(xbeeclients[i].name[0]));
		return temp;
	    }
	} else if (address.addr16_enabled) {
	    if ((xbeeclients[i].addr16[0] == address.addr16[0]) &&
		(xbeeclients[i].addr16[1] == address.addr16[1])) {
		strcpy(temp, &(xbeeclients[i].name[0]));
		return temp;
	    }
	} else return NULL;
    }

    return NULL;
}

void xbee_packetreceived(struct xbee *xbee, struct xbee_con *con, struct xbee_pkt **pkt, void **data) {
    if ((*pkt)->dataLen == 0) {
	printf("Received xbee packet was too short\n");
	return;
    }
    printf("rx [%s]: [%s] (%d)\n", getName((*pkt)->address), ((*pkt)->data), (*pkt)->dataLen);

    sprintf(payload, "%s: %s\n", localName, ((*pkt)->data));
    pubmsg.payload = payload;
    pubmsg.payloadlen = strlen(payload);
    pubmsg.qos = QOS;
    pubmsg.retained = 0;
    MQTTClient_publishMessage(client, TOPIC, &pubmsg, &token);
    if ((rc = MQTTClient_waitForCompletion(client, token, TIMEOUT)) != MQTTCLIENT_SUCCESS) {
	printf("Couldn't send MQTT-Message: %d", rc);
	exit(4);
    }
}

void xbee_atpacketreceived(struct xbee *xbee, struct xbee_con *con, struct xbee_pkt **pkt, void **data) {
    int i;

    if ((*pkt)->dataLen == 0) {
	printf("Received xbee packet was too short\n");
	return;
    }

    if (!strncasecmp((*pkt)->atCommand, "NI", 2)) {
	strcpy(&(localName[0]), &((*pkt)->data[0]));
    } else if (!strncasecmp((*pkt)->atCommand, "ND", 2)) {
	if ((*pkt)->frameId != lastNDFrame) {
	    memset(&xbeeclients, 0, sizeof(xbeeclients));
	    lastNDFrame = (*pkt)->frameId;
	    cntClients = 0;
	}
	strcpy(&(xbeeclients[cntClients].name[0]), &((*pkt)->data[10]));
	memcpy(&(xbeeclients[cntClients].addr16[0]), &((*pkt)->data[0]), 2);
	memcpy(&(xbeeclients[cntClients].addr64[0]), &((*pkt)->data[2]), 8);
	cntClients++;

	printf("Clientname: %s 0x", xbeeclients[cntClients-1].name);
	for (i=0; i<2; i++) {
	    printf("%02X", xbeeclients[cntClients-1].addr16[i]);
	}
	printf("  0x");
	for (i=0; i<7; i++) {
	    printf("%02X", xbeeclients[cntClients-1].addr64[i]);
	    if (i==3) printf(" 0x");
	}
	printf("\n");
    }

    printf("rx [%s]: [%s] (%d) (AT: %s - Clients: %d)\n", localName, ((*pkt)->data), (*pkt)->dataLen, (*pkt)->atCommand, cntClients);
}

int mqtt_msgarrvd(void *context, char *topicName, int topicLen, MQTTClient_message *message) {
    int i;
    char* payloadptr;
    char temp[100];

//    strcpy(temp, "hpn54l:");
    sprintf(temp, "%s:", localName);
    if (strncmp(message->payload, temp, 7) != 0) {
	printf("###NOTSELF###\n");

	printf("MQTT-Message Topic: %s\n", topicName);
	printf("           Message: ");

	payloadptr = message->payload;
	for (i=0; i<message->payloadlen; i++) {
	    putchar(*payloadptr++);
	}
	putchar('\n');

	xbee_conTx(con, NULL, topicName);

	xbee_conTx(con_at, NULL, "ND");
    } else printf("---SELF---\n");

    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);

    return 1;
}

int init_system() {
    strcpy(localName, "NoName");

    if ((ret = xbee_setup(&xbee, XBEE_TYPE, XBEE_DEVICE, XBEE_BAUDRATE)) != XBEE_ENONE) {
	printf("Couldn't connect to XBee: %d (%s)\n", ret, xbee_errorToStr(ret));
	exit(1);
    }

    xbee_logTargetSet(xbee, stderr);
//    xbee_logLevelSet(xbee, 100);
    xbee_log(xbee, -1, "Start logging...");

    memset(&xbee_address, 0, sizeof(xbee_address));
    xbee_address.addr64_enabled = 1;
    xbee_address.addr64[0] = 0x00;
    xbee_address.addr64[1] = 0x00;
    xbee_address.addr64[2] = 0x00;
    xbee_address.addr64[3] = 0x00;
    xbee_address.addr64[4] = 0x00;
    xbee_address.addr64[5] = 0x00;
    xbee_address.addr64[6] = 0xFF;
    xbee_address.addr64[7] = 0xFF;

    if ((ret = xbee_conNew(xbee, &con, "Data", &xbee_address)) != XBEE_ENONE) {
	printf("Couldn't create a xbee connection: %d (%s)\n", ret, xbee_errorToStr(ret));
	exit(2);
    }

    if ((ret = xbee_conNew(xbee, &con_at, "Local AT", NULL)) != XBEE_ENONE) {
	printf("Couldn't create a xbee control connection: %d (%s)\n", ret, xbee_errorToStr(ret));
	exit(2);
    }

    xbee_conSettings(con, NULL, &xbee_settings);
    xbee_settings.catchAll = 1;
    xbee_conSettings(con, &xbee_settings, NULL);

    if ((ret = xbee_conCallbackSet(con, xbee_packetreceived, NULL)) != XBEE_ENONE) {
	printf("Couldn't setup the xbee callback function: %d (%s)\n", ret, xbee_errorToStr(ret));
	exit(3);
    }

    if ((ret = xbee_conCallbackSet(con_at, xbee_atpacketreceived, NULL)) != XBEE_ENONE) {
	printf("Couldn't setup the xbee control callback function: %d (%s)\n", ret, xbee_errorToStr(ret));
	exit(3);
    }

    MQTTClient_create(&client, ADDRESS, CLIENTID, MQTTCLIENT_PERSISTENCE_NONE, NULL);
    conn_opts.keepAliveInterval = 20;
    conn_opts.cleansession = 1;

    MQTTClient_setCallbacks(client, NULL, NULL, mqtt_msgarrvd, NULL);

    if ((rc = MQTTClient_connect(client, &conn_opts)) != MQTTCLIENT_SUCCESS) {
	printf("Couldn't connect to MQTT-Broker: %d\n", rc);
	exit(4);
    }

    MQTTClient_subscribe(client, TOPIC, QOS);

//    xbee_conTx(con, NULL, "Broadcast!");
    xbee_conTx(con_at, &txRet, "NI");
    printf("txRet: %d\n", txRet);
    xbee_conTx(con_at, &txRet, "ND");
    printf("txRet: %d\n", txRet);

    return TRUE;
}

int free_system() {
    if ((ret = xbee_conEnd(con)) != XBEE_ENONE) {
	printf("Couldn't end the xbee connection: %d (%s)\n", ret, xbee_errorToStr(ret));
	return ret;
    }
    if ((ret = xbee_conEnd(con_at)) != XBEE_ENONE) {
	printf("Couldn't end the xbee control connection: %d (%s)\n", ret, xbee_errorToStr(ret));
	return ret;
    }
    xbee_shutdown(xbee);

    MQTTClient_unsubscribe(client, TOPIC);
    MQTTClient_disconnect(client, TIMEOUT);
    MQTTClient_destroy(&client);

    return TRUE;
}

int kbhit(void) {
    struct termios oldt, newt;
    int ch;
    int oldf;

    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO | ISIG);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);

    ch = getchar();

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    fcntl(STDIN_FILENO, F_SETFL, oldf);

    if (ch != EOF) {
	ungetc(ch, stdin);
	return 1;
    }
    return 0;
}

void listen_loop() {
    while (!kbhit()) {
	usleep(5000);
	MQTTClient_yield();
    }

    free_system();
}

int main(int argc, char *argv[]) {
    init_system();
    listen_loop();

    return 0;
}
