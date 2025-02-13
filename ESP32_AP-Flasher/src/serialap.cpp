#include "serialap.h"

#include <Arduino.h>
#include <HardwareSerial.h>

#include "commstructs.h"
#include "flasher.h"
#include "leds.h"
#include "newproto.h"
#include "powermgt.h"
#include "settings.h"
#include "storage.h"
#include "web.h"
#include "zbs_interface.h"
#include "contentmanager.h"

QueueHandle_t rxCmdQueue;
SemaphoreHandle_t txActive;

// If a command is sent, it will wait for a reply here
#define CMD_REPLY_WAIT 0x00
#define CMD_REPLY_ACK 0x01
#define CMD_REPLY_NOK 0x02
#define CMD_REPLY_NOQ 0x03
volatile uint8_t cmdReplyValue = CMD_REPLY_WAIT;

#define AP_SERIAL_PORT Serial1

uint8_t channelList[6];
struct espSetChannelPower curChannel = {0, 11, 10};

#define RX_CMD_RQB 0x01
#define RX_CMD_ADR 0x02
#define RX_CMD_XFC 0x03
#define RX_CMD_XTO 0x04
#define RX_CMD_RDY 0x05
#define RX_CMD_RSET 0x06

#define AP_ACTIVITY_MAX_INTERVAL 30 * 1000
volatile uint32_t lastAPActivity = 0;
struct APInfoS apInfo;

extern uint8_t* getDataForFile(File* file);

struct rxCmd {
    uint8_t* data;
    uint8_t len;
    uint8_t type;
};

#define ZBS_RX_WAIT_HEADER 0
#define ZBS_RX_WAIT_PKT_LEN 1
#define ZBS_RX_WAIT_PKT_RX 2
#define ZBS_RX_WAIT_SEP1 3
#define ZBS_RX_WAIT_SEP2 4
#define ZBS_RX_WAIT_VER 6
#define ZBS_RX_BLOCK_REQUEST 7
#define ZBS_RX_WAIT_XFERCOMPLETE 8
#define ZBS_RX_WAIT_DATA_REQ 9
#define ZBS_RX_WAIT_JOINNETWORK 10
#define ZBS_RX_WAIT_XFERTIMEOUT 11
#define ZBS_RX_WAIT_MAC 12
#define ZBS_RX_WAIT_CHANNEL 13
#define ZBS_RX_WAIT_POWER 14
#define ZBS_RX_WAIT_PENDING 15
#define ZBS_RX_WAIT_NOP 16
#define ZBS_RX_WAIT_TYPE 17

bool txStart() {
    while (1) {
        if (xPortInIsrContext()) {
            if (xSemaphoreTakeFromISR(txActive, NULL) == pdTRUE) return true;
        } else {
            if (xSemaphoreTake(txActive, portTICK_PERIOD_MS)) return true;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
        Serial.println("wait... tx busy");
    }
    // this never happens. Should we make a timeout?
    return false;
}
void txEnd() {
    if (xPortInIsrContext()) {
        xSemaphoreGiveFromISR(txActive, NULL);
    } else {
        xSemaphoreGive(txActive);
    }
}
bool waitCmdReply() {
    uint32_t val = millis();
    while (millis() < val + 100) {
        switch (cmdReplyValue) {
            case CMD_REPLY_WAIT:
                break;
            case CMD_REPLY_ACK:
                lastAPActivity = millis();
                return true;
                break;
            case CMD_REPLY_NOK:
                lastAPActivity = millis();
                return false;
                break;
            case CMD_REPLY_NOQ:
                lastAPActivity = millis();
                return false;
                break;
        }
        vTaskDelay(1 / portTICK_RATE_MS);
    }
    return false;
}

#if (AP_PROCESS_PORT == FLASHER_AP_PORT)
int8_t APpowerPins[] = FLASHER_AP_POWER;
#define AP_RESET_PIN FLASHER_AP_RESET
#define AP_POWER_PIN FLASHER_AP_POWER
#endif
#ifdef OPENEPAPERLINK_PCB
#if (AP_PROCESS_PORT == FLASHER_EXT_PORT)
int8_t APpowerPins[] = FLASHER_EXT_POWER;
#define AP_RESET_PIN FLASHER_EXT_RESET
#define AP_POWER_PIN FLASHER_EXT_POWER
#endif
#if (AP_PROCESS_PORT == FLASHER_ALTRADIO_PORT)
int8_t APpowerPins[] = FLASHER_ALT_POWER;
#define AP_RESET_PIN FLASHER_ALT_RESET
#define AP_POWER_PIN FLASHER_ALT_POWER
#endif
#endif

void APEnterEarlyReset() {
    pinMode(AP_RESET_PIN, OUTPUT);
    digitalWrite(AP_RESET_PIN, LOW);
}

// Reset the tag
void APTagReset() {
    uint8_t powerPins = sizeof(APpowerPins);
    if (powerPins > 0 && APpowerPins[0] == -1)
        powerPins = 0;
    pinMode(AP_RESET_PIN, OUTPUT);
    digitalWrite(AP_RESET_PIN, LOW);
    vTaskDelay(50 / portTICK_PERIOD_MS);
    powerControl(false, (uint8_t*)APpowerPins, powerPins);
    vTaskDelay(300 / portTICK_PERIOD_MS);
    powerControl(true, (uint8_t*)APpowerPins, powerPins);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    digitalWrite(AP_RESET_PIN, HIGH);
    vTaskDelay(100 / portTICK_PERIOD_MS);
}

// Send data to the AP
uint16_t sendBlock(const void* data, const uint16_t len) {
    if (!apInfo.isOnline) return false;
    if (!txStart()) return 0;
    for (uint8_t attempt = 0; attempt < 5; attempt++) {
        cmdReplyValue = CMD_REPLY_WAIT;
        AP_SERIAL_PORT.print(">D>");
        if (waitCmdReply()) goto blksend;
        Serial.printf("block send failed in try %d\n", attempt);
    }
    Serial.print("Failed sending block...\n");
    txEnd();
    return 0;
blksend:
    uint8_t blockbuffer[sizeof(struct blockData)];
    struct blockData* bd = (struct blockData*)blockbuffer;
    bd->size = len;
    bd->checksum = 0;

    // calculate checksum
    for (uint16_t c = 0; c < len; c++) {
        bd->checksum += ((uint8_t*)data)[c];
    }

    // send blockData header
    for (uint8_t c = 0; c < sizeof(struct blockData); c++) {
        AP_SERIAL_PORT.write(0xAA ^ blockbuffer[c]);
    }

    // send an entire block of data
    uint16_t c;
    for (c = 0; c < len; c++) {
        AP_SERIAL_PORT.write(0xAA ^ ((uint8_t*)data)[c]);
    }

    // fill the rest of the block-length filled with something else (will end up as 0xFF in the buffer)
    for (; c < BLOCK_DATA_SIZE; c++) {
        AP_SERIAL_PORT.write(0x55);
    }

    // dummy bytes in case some bytes were missed, makes sure the AP gets kicked out of data-loading mode
    for (c = 0; c < 32; c++) {
        AP_SERIAL_PORT.write(0xF5);
    }
    delay(10);
    txEnd();
    return bd->checksum;
}
bool sendDataAvail(struct pendingData* pending) {
    if (!apInfo.isOnline) return false;
    if (!txStart()) return false;
    addCRC(pending, sizeof(struct pendingData));
    for (uint8_t attempt = 0; attempt < 5; attempt++) {
        cmdReplyValue = CMD_REPLY_WAIT;
        AP_SERIAL_PORT.print("SDA>");
        for (uint8_t c = 0; c < sizeof(struct pendingData); c++) {
            AP_SERIAL_PORT.write(((uint8_t*)pending)[c]);
        }
        if (waitCmdReply()) goto sdasend;
        Serial.printf("SDA send failed in try %d\n", attempt);
        delay(200);
    }
    Serial.print("SDA failed to send...\n");
    txEnd();
    return false;
sdasend:
    txEnd();
    return true;
}
bool sendCancelPending(struct pendingData* pending) {
    if (!apInfo.isOnline) return false;
    if (!txStart()) return false;
    addCRC(pending, sizeof(struct pendingData));
    for (uint8_t attempt = 0; attempt < 5; attempt++) {
        cmdReplyValue = CMD_REPLY_WAIT;
        AP_SERIAL_PORT.print("CXD>");
        for (uint8_t c = 0; c < sizeof(struct pendingData); c++) {
            AP_SERIAL_PORT.write(((uint8_t*)pending)[c]);
        }
        if (waitCmdReply()) goto cxdsent;
        Serial.printf("CXD send failed in try %d\n", attempt);
    }
    Serial.print("CXD failed to send...\n");
    txEnd();
    return false;
cxdsent:
    txEnd();
    return true;
}
bool sendChannelPower(struct espSetChannelPower* scp) {
    if ((apInfo.state != AP_STATE_ONLINE) && (apInfo.state != AP_STATE_COMING_ONLINE)) return false;
    if (!txStart()) return false;
    addCRC(scp, sizeof(struct espSetChannelPower));
    for (uint8_t attempt = 0; attempt < 5; attempt++) {
        cmdReplyValue = CMD_REPLY_WAIT;
        AP_SERIAL_PORT.print("SCP>");
        for (uint8_t c = 0; c < sizeof(struct espSetChannelPower); c++) {
            AP_SERIAL_PORT.write(((uint8_t*)scp)[c]);
        }
        if (waitCmdReply()) goto scpSent;
        Serial.printf("SCP send failed in try %d\n", attempt);
    }
    Serial.print("SCP failed to send...\n");
    txEnd();
    return false;
scpSent:
    txEnd();
    return true;
}
bool sendPing() {
    if (!txStart()) return false;
    for (uint8_t attempt = 0; attempt < 5; attempt++) {
        cmdReplyValue = CMD_REPLY_WAIT;
        AP_SERIAL_PORT.print("RDY?");
        if (waitCmdReply()) goto pingSent;
    }
    txEnd();
    return false;
pingSent:
    txEnd();
    return true;
}
bool sendGetInfo() {
    if (!txStart()) return false;
    for (uint8_t attempt = 0; attempt < 5; attempt++) {
        cmdReplyValue = CMD_REPLY_WAIT;
        AP_SERIAL_PORT.print("NFO?");
        if (waitCmdReply()) goto nfoRequested;
    }
    txEnd();
    return false;
nfoRequested:
    txEnd();
    return true;
}

// add RX'd request from the AP to the processor queue
void addRXQueue(uint8_t* data, uint8_t len, uint8_t type) {
    struct rxCmd* rxcmd = new struct rxCmd;
    rxcmd->data = data;
    rxcmd->len = len;
    rxcmd->type = type;
    BaseType_t queuestatus = xQueueSend(rxCmdQueue, &rxcmd, 0);
    if (queuestatus == pdFALSE) {
        if (data) free(data);
        free(rxcmd);
    }
}

// Asynchronous command processor
void rxCmdProcessor(void* parameter) {
    rxCmdQueue = xQueueCreate(30, sizeof(struct rxCmd*));
    txActive = xSemaphoreCreateBinary();
    xSemaphoreGive(txActive);
    while (1) {
        struct rxCmd* rxcmd = nullptr;
        BaseType_t q = xQueueReceive(rxCmdQueue, &rxcmd, 10);
        if (q == pdTRUE) {
            switch (rxcmd->type) {
                case RX_CMD_RQB:
                    processBlockRequest((struct espBlockRequest*)rxcmd->data);
#ifdef HAS_RGB_LED
                    shortBlink(CRGB::Blue);
#else
                    quickBlink(3);
#endif
                    break;
                case RX_CMD_ADR:
                    processDataReq((struct espAvailDataReq*)rxcmd->data, true);
#ifdef HAS_RGB_LED
                    shortBlink(CRGB::Aqua);
#else
                    quickBlink(1);
#endif
                    break;
                case RX_CMD_XFC:
                    processXferComplete((struct espXferComplete*)rxcmd->data, true);
#ifdef HAS_RGB_LED
                    shortBlink(CRGB::Purple);
#endif
                    break;
                case RX_CMD_XTO:
                    processXferTimeout((struct espXferComplete*)rxcmd->data, true);
                    break;
            }
            if (rxcmd->data) free(rxcmd->data);
            if (rxcmd) free(rxcmd);
        }
    }
}
void rxSerialTask(void* parameter) {
    static char cmdbuffer[4] = {0};
    static uint8_t* packetp = nullptr;
    //    static uint8_t pktlen = 0;
    static uint8_t pktindex = 0;  // length of the command
    static uint8_t RXState = ZBS_RX_WAIT_HEADER;
    static char lastchar = 0;
    static uint8_t charindex = 0;

    while (1) {
        while (AP_SERIAL_PORT.available()) {
            lastchar = AP_SERIAL_PORT.read();
            switch (RXState) {
                case ZBS_RX_WAIT_HEADER:
                    // Serial.write(lastchar);
                    //  shift characters in
                    for (uint8_t c = 0; c < 3; c++) {
                        cmdbuffer[c] = cmdbuffer[c + 1];
                    }
                    cmdbuffer[3] = lastchar;

                    if ((strncmp(cmdbuffer, "ACK>", 4) == 0)) cmdReplyValue = CMD_REPLY_ACK;
                    if ((strncmp(cmdbuffer, "NOK>", 4) == 0)) cmdReplyValue = CMD_REPLY_NOK;
                    if ((strncmp(cmdbuffer, "NOQ>", 4) == 0)) cmdReplyValue = CMD_REPLY_NOQ;

                    if ((strncmp(cmdbuffer, "VER>", 4) == 0)) {
                        pktindex = 0;
                        RXState = ZBS_RX_WAIT_VER;
                        charindex = 0;
                        memset(cmdbuffer, 0x00, 4);
                    }
                    if ((strncmp(cmdbuffer, "MAC>", 4) == 0)) {
                        RXState = ZBS_RX_WAIT_MAC;
                        charindex = 0;
                        memset(cmdbuffer, 0x00, 4);
                    }
                    if ((strncmp(cmdbuffer, "ZCH>", 4) == 0)) {
                        RXState = ZBS_RX_WAIT_CHANNEL;
                        charindex = 0;
                        memset(cmdbuffer, 0x00, 4);
                    }
                    if ((strncmp(cmdbuffer, "ZPW>", 4) == 0)) {
                        RXState = ZBS_RX_WAIT_POWER;
                        charindex = 0;
                        memset(cmdbuffer, 0x00, 4);
                    }
                    if ((strncmp(cmdbuffer, "PEN>", 4) == 0)) {
                        RXState = ZBS_RX_WAIT_PENDING;
                        charindex = 0;
                        memset(cmdbuffer, 0x00, 4);
                    }
                    if ((strncmp(cmdbuffer, "NOP>", 4) == 0)) {
                        RXState = ZBS_RX_WAIT_NOP;
                        charindex = 0;
                        memset(cmdbuffer, 0x00, 4);
                    }
                    if ((strncmp(cmdbuffer, "TYP>", 4) == 0)) {
                        RXState = ZBS_RX_WAIT_TYPE;
                        charindex = 0;
                        memset(cmdbuffer, 0x00, 4);
                    }
                    if (strncmp(cmdbuffer, "RES>", 4) == 0) {
                        addRXQueue(NULL, 0, RX_CMD_RSET);
                    }
                    if (strncmp(cmdbuffer, "RQB>", 4) == 0) {
                        RXState = ZBS_RX_BLOCK_REQUEST;
                        charindex = 0;
                        pktindex = 0;
                        packetp = (uint8_t*)calloc(sizeof(struct espBlockRequest) + 8, 1);
                        memset(cmdbuffer, 0x00, 4);
                    }
                    if (strncmp(cmdbuffer, "ADR>", 4) == 0) {
                        RXState = ZBS_RX_WAIT_DATA_REQ;
                        charindex = 0;
                        pktindex = 0;
                        packetp = (uint8_t*)calloc(sizeof(struct espAvailDataReq) + 8, 1);
                        memset(cmdbuffer, 0x00, 4);
                    }
                    if (strncmp(cmdbuffer, "XFC>", 4) == 0) {
                        RXState = ZBS_RX_WAIT_XFERCOMPLETE;
                        pktindex = 0;
                        packetp = (uint8_t*)calloc(sizeof(struct espXferComplete) + 8, 1);
                        memset(cmdbuffer, 0x00, 4);
                    }
                    if (strncmp(cmdbuffer, "XTO>", 4) == 0) {
                        RXState = ZBS_RX_WAIT_XFERTIMEOUT;
                        pktindex = 0;
                        packetp = (uint8_t*)calloc(sizeof(struct espXferComplete) + 8, 1);
                        memset(cmdbuffer, 0x00, 4);
                    }
                    if (strncmp(cmdbuffer, "RDY>", 4) == 0) {
                        addRXQueue(NULL, 0, RX_CMD_RDY);
                    }
                    break;
                case ZBS_RX_BLOCK_REQUEST:
                    packetp[pktindex] = lastchar;
                    pktindex++;
                    if (pktindex == sizeof(struct espBlockRequest)) {
                        addRXQueue(packetp, pktindex, RX_CMD_RQB);
                        RXState = ZBS_RX_WAIT_HEADER;
                    }
                    break;
                case ZBS_RX_WAIT_XFERCOMPLETE:
                    packetp[pktindex] = lastchar;
                    pktindex++;
                    if (pktindex == sizeof(struct espXferComplete)) {
                        addRXQueue(packetp, pktindex, RX_CMD_XFC);
                        RXState = ZBS_RX_WAIT_HEADER;
                    }
                    break;
                case ZBS_RX_WAIT_XFERTIMEOUT:
                    packetp[pktindex] = lastchar;
                    pktindex++;
                    if (pktindex == sizeof(struct espXferComplete)) {
                        addRXQueue(packetp, pktindex, RX_CMD_XTO);
                        RXState = ZBS_RX_WAIT_HEADER;
                    }
                    break;
                case ZBS_RX_WAIT_DATA_REQ:
                    packetp[pktindex] = lastchar;
                    pktindex++;
                    if (pktindex == sizeof(struct espAvailDataReq)) {
                        addRXQueue(packetp, pktindex, RX_CMD_ADR);
                        RXState = ZBS_RX_WAIT_HEADER;
                    }
                    break;
                case ZBS_RX_WAIT_VER:
                    cmdbuffer[charindex] = lastchar;
                    charindex++;
                    if (charindex == 4) {
                        charindex = 0;
                        apInfo.version = (uint16_t)strtoul(cmdbuffer, NULL, 16);
                        RXState = ZBS_RX_WAIT_HEADER;
                    }
                    break;
                case ZBS_RX_WAIT_MAC:
                    cmdbuffer[charindex] = lastchar;
                    charindex++;
                    if (charindex == 2) {
                        charindex = 0;
                        apInfo.mac[pktindex] = (uint8_t)strtoul(cmdbuffer, NULL, 16);
                        pktindex++;
                    }
                    if (pktindex == 8) {
                        RXState = ZBS_RX_WAIT_HEADER;
                    }
                    break;
                case ZBS_RX_WAIT_CHANNEL:
                    cmdbuffer[charindex] = lastchar;
                    charindex++;
                    if (charindex == 2) {
                        RXState = ZBS_RX_WAIT_HEADER;
                        apInfo.channel = (uint8_t)strtoul(cmdbuffer, NULL, 16);
                    }
                    break;
                case ZBS_RX_WAIT_POWER:
                    cmdbuffer[charindex] = lastchar;
                    charindex++;
                    if (charindex == 2) {
                        RXState = ZBS_RX_WAIT_HEADER;
                        apInfo.power = (uint8_t)strtoul(cmdbuffer, NULL, 16);
                    }
                    break;
                case ZBS_RX_WAIT_PENDING:
                    cmdbuffer[charindex] = lastchar;
                    charindex++;
                    if (charindex == 2) {
                        RXState = ZBS_RX_WAIT_HEADER;
                        apInfo.pending = (uint8_t)strtoul(cmdbuffer, NULL, 16);
                    }
                    break;
                case ZBS_RX_WAIT_NOP:
                    cmdbuffer[charindex] = lastchar;
                    charindex++;
                    if (charindex == 2) {
                        RXState = ZBS_RX_WAIT_HEADER;
                        apInfo.nop = (uint8_t)strtoul(cmdbuffer, NULL, 16);
                    }
                    break;
                case ZBS_RX_WAIT_TYPE:
                    cmdbuffer[charindex] = lastchar;
                    charindex++;
                    if (charindex == 2) {
                        RXState = ZBS_RX_WAIT_HEADER;
                        apInfo.type = (uint8_t)strtoul(cmdbuffer, NULL, 16);
                    }
                    break;
            }
        }
        vTaskDelay(1 / portTICK_PERIOD_MS);
    }  // end of while(1)
}

void ShowAPInfo() {
    Serial.printf("| AP Information - type %02X   |\n", apInfo.type);
    Serial.printf("| Channel |             0x%02X |\n", apInfo.channel);
    Serial.printf("| Power   |               %02X |\n", apInfo.power);
    Serial.printf("| MAC     | %02X%02X%02X%02X%02X%02X%02X%02X |\n", apInfo.mac[7], apInfo.mac[6], apInfo.mac[5], apInfo.mac[4], apInfo.mac[3], apInfo.mac[2], apInfo.mac[1], apInfo.mac[0]);
    Serial.printf("| Version |           0x%04X |\n", apInfo.version);

    if (apInfo.type == SOLUM_154_SSD1619 || apInfo.type == SOLUM_29_SSD1619 || apInfo.type == SOLUM_29_UC8151 || apInfo.type == SOLUM_42_SSD1619) {
        char macString[50];
        sprintf(macString, "%02X%02X%02X%02X%02X%02X%02X%02X", apInfo.mac[7], apInfo.mac[6], apInfo.mac[5], apInfo.mac[4], apInfo.mac[3], apInfo.mac[2], apInfo.mac[1], apInfo.mac[0]);
        showIpAddress(macString);
    }
}

void notifySegmentedFlash() {
    sendAPSegmentedData(apInfo.mac, (String) "Fl     ash", 0x0800, false, true);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
#ifdef POWER_NO_SOFT_POWER
    sendAPSegmentedData(apInfo.mac, (String) "If    done", 0x0800, false, true);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    sendAPSegmentedData(apInfo.mac, (String) "RE    boot", 0x0800, false, true);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
#endif
}
void checkWaitPowerCycle() {
    // check if we should wait for a power cycle. If we do, try to inform the user the best we can, and hang.
#ifdef POWER_NO_SOFT_POWER
    apInfo.isOnline = false;
    apInfo.state = AP_STATE_REQUIRED_POWER_CYCLE;
    // If we have no soft power control, we'll now wait until the device is power-cycled
    Serial.printf("Please power-cycle your AP/device\n");
#ifdef HAS_RGB_LED
    showColorPattern(CRGB::Aqua, CRGB::Aqua, CRGB::Red);
#endif
    while (1) {
        vTaskDelay(3000 / portTICK_PERIOD_MS);
    }
#endif
}
void segmentedShowIp() {
    IPAddress IP = WiFi.localIP();
    char temp[12];
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    sendAPSegmentedData(apInfo.mac, (String) "IP    Addr", 0x0200, true, true);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    sprintf(temp, "%03d IP %03d", IP[0], IP[1]);
    sendAPSegmentedData(apInfo.mac, (String)temp, 0x0200, true, true);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    sprintf(temp, "%03d IP %03d", IP[2], IP[3]);
    sendAPSegmentedData(apInfo.mac, (String)temp, 0x0200, true, true);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
}

bool bringAPOnline() {
    apInfo.isOnline = false;
    apInfo.state = AP_STATE_OFFLINE;
    APTagReset();
    vTaskDelay(500 / portTICK_PERIOD_MS);
    uint32_t bootTimeout = millis();
    bool APrdy = false;
    while ((!APrdy) && (millis() - bootTimeout < 10 * 1000)) {
        APrdy = sendPing();
        vTaskDelay(300 / portTICK_PERIOD_MS);
    }
    if (!APrdy) {
        return false;
    } else {
        apInfo.state = AP_STATE_COMING_ONLINE;
        sendChannelPower(&curChannel);
        vTaskDelay(200 / portTICK_PERIOD_MS);
        if (!sendGetInfo()) {
            apInfo.state = AP_STATE_OFFLINE;
            return false;
        }
        vTaskDelay(200 / portTICK_PERIOD_MS);
        apInfo.isOnline = true;
        apInfo.state = AP_STATE_ONLINE;
        return true;
    }
}

void APTask(void* parameter) {
    xTaskCreate(rxCmdProcessor, "rxCmdProcessor", 4000, NULL, configMAX_PRIORITIES - 10, NULL);
    xTaskCreate(rxSerialTask, "rxSerialTask", 1750, NULL, configMAX_PRIORITIES - 4, NULL);

#if (AP_PROCESS_PORT == FLASHER_AP_PORT)
    AP_SERIAL_PORT.begin(115200, SERIAL_8N1, FLASHER_AP_RXD, FLASHER_AP_TXD);
#endif
#ifdef OPENEPAPERLINK_PCB
#if (AP_PROCESS_PORT == FLASHER_EXT_PORT)
    AP_SERIAL_PORT.begin(115200, SERIAL_8N1, FLASHER_EXT_RXD, FLASHER_EXT_TXD);
#endif
#if (AP_PROCESS_PORT == FLASHER_ALTRADIO_PORT)
    AP_SERIAL_PORT.begin(115200, SERIAL_8N1, FLASHER_AP_RXD, FLASHER_AP_TXD);
#endif
#endif

    bringAPOnline();

    if (checkForcedAPFlash()) {
        if (apInfo.type == SOLUM_SEG_UK && apInfo.isOnline) {
            notifySegmentedFlash();
        }
        Serial.printf("We're going to try to perform an 'AP forced flash' in\n");
        flashCountDown(10);
        Serial.printf("\nPerforming force flash of the AP\n");
        apInfo.isOnline = false;
        apInfo.state = AP_STATE_FLASHING;
        doForcedAPFlash();
        checkWaitPowerCycle();
        bringAPOnline();
    }

    if (apInfo.isOnline) {
        // AP works!
        ShowAPInfo();

        if (apInfo.type == SOLUM_SEG_UK) {
            apInfo.state = AP_STATE_COMING_ONLINE;
            segmentedShowIp();
            showAPSegmentedInfo(apInfo.mac, true);
            apInfo.state = AP_STATE_ONLINE;
            updateContent(apInfo.mac);
        }

        uint16_t fsversion;
        fsversion = getAPUpdateVersion(apInfo.type);
        if ((fsversion) && (apInfo.version != fsversion)) {
            Serial.printf("Firmware version on LittleFS: %04X\n", fsversion);

            Serial.printf("We're going to try to update the AP's FW in\n");
            flashCountDown(30);
            Serial.printf("\n");
            notifySegmentedFlash();
            apInfo.isOnline = false;
            apInfo.state = AP_STATE_FLASHING;
            if (doAPUpdate(apInfo.type)) {
                checkWaitPowerCycle();
                Serial.printf("Flash completed, let's try to boot the AP!\n");
                if (bringAPOnline()) {
                    // AP works
                    ShowAPInfo();
                    setAPchannel();
                } else {
                    Serial.printf("Failed to bring up the AP after flashing seemed successful... That's not supposed to happen!\n");
                    Serial.printf("This can be caused by a bad AP firmware, failed or failing hardware, or the inability to fully power-cycle the AP\n");
                    apInfo.state = AP_STATE_FAILED;
#ifdef HAS_RGB_LED
                    showColorPattern(CRGB::Red, CRGB::Yellow, CRGB::Red);
#endif
                }
            } else {
                apInfo.state = AP_STATE_FAILED;
                checkWaitPowerCycle();
                Serial.println("Failed to update version on the AP :(\n");
#ifdef HAS_RGB_LED
                showColorPattern(CRGB::Red, CRGB::Red, CRGB::Red);
#endif
            }
        }
        refreshAllPending();
    } else {
#ifndef FLASH_TIMEOUT
#define FLASH_TIMEOUT 30
#endif

        // AP unavailable, maybe time to flash?
        apInfo.isOnline = false;
        apInfo.state = AP_STATE_OFFLINE;
        Serial.println("I wasn't able to connect to a ZBS (AP) tag.\n");
        Serial.printf("This could be the first time this AP is booted and the AP-tag may be unflashed. We'll try to flash it!\n");
        Serial.printf("If this tag was previously flashed succesfully but this message still shows up, there's probably something wrong with the serial connections.\n");
        Serial.printf("The build of this firmware expects an AP tag with TXD/RXD on ESP32 pins %d and %d, does this match with your wiring?\n", FLASHER_AP_RXD, FLASHER_AP_TXD);
        Serial.printf("Performing firmware flash in about %d seconds!\n", FLASH_TIMEOUT);
        flashCountDown(FLASH_TIMEOUT);
        if (doAPFlash()) {
            checkWaitPowerCycle();
            if (bringAPOnline()) {
                // AP works
                ShowAPInfo();
                if (apInfo.type == SOLUM_SEG_UK) {
                    segmentedShowIp();
                    showAPSegmentedInfo(apInfo.mac, true);
                }
                refreshAllPending();
            } else {
                Serial.printf("Failed to bring up the AP after successful flashing... That's not supposed to happen!\n");
                Serial.printf("This generally means that the flasher connections (MISO/MOSI/CLK/RESET/CS) are okay,\n");
                Serial.printf("but we can't (yet) talk to the AP over serial lines. Verify the pins mentioned above.\n\n");

#ifndef POWER_NO_SOFT_POWER
                Serial.printf("The firmware you're using expects soft power control over the AP tag; if it can't\n");
                Serial.printf("power-cycle the AP-tag using GPIO pin %d, this can cause this very same issue.\n", APpowerPins[0]);
#endif

#ifdef HAS_RGB_LED
                showColorPattern(CRGB::Red, CRGB::Yellow, CRGB::Red);
#endif
                apInfo.isOnline = false;
                apInfo.state = AP_STATE_FAILED;
            }
        } else {
            // failed to flash
#ifdef HAS_RGB_LED
            showColorPattern(CRGB::Red, CRGB::Red, CRGB::Red);
#endif
            apInfo.isOnline = false;
            apInfo.state = AP_STATE_FAILED;
            Serial.println("Failed to flash the AP :(");
            Serial.println("Seems like you're running into some issues with the wiring, or (very small chance) the tag itself");
            Serial.println("This ESP32-build expects the following pins connected to the ZBS243:");
            Serial.println("---  ZBS243 based tag              ESP32  ---");
            Serial.printf("       TXD     ----------------     %02d\n", FLASHER_AP_RXD);
            Serial.printf("       RXD     ----------------     %02d\n", FLASHER_AP_TXD);
            Serial.printf("       CS/SS   ----------------     %02d\n", FLASHER_AP_SS);
            Serial.printf("       MOSI    ----------------     %02d\n", FLASHER_AP_MOSI);
            Serial.printf("       MISO    ----------------     %02d\n", FLASHER_AP_MISO);
            Serial.printf("       CLK     ----------------     %02d\n", FLASHER_AP_CLK);
            Serial.printf("       RSET    ----------------     %02d\n", FLASHER_AP_RESET);
#ifdef POWER_NO_SOFT_POWER
            Serial.printf("Your firmware is configured without soft power control. This means you'll have to manually power-cycle the tag after flashing.\n");
#else
            Serial.printf("       POWER   ----------------     %02d\n", APpowerPins[0]);
#endif
            Serial.println("Please verify your wiring and try again!");
        }
#ifdef HAS_SDCARD
        if (SD_CARD_CLK == FLASHER_AP_CLK ||
            SD_CARD_MISO == FLASHER_AP_MISO ||
            SD_CARD_MOSI == FLASHER_AP_MOSI) {
            Serial.println("Reseting in 30 seconds to restore SPI state!\n");
            flashCountDown(30);
            ESP.restart();
        }
#endif
    }

    uint8_t attempts = 0;
    while (1) {
        if ((apInfo.isOnline) && (millis() - lastAPActivity > AP_ACTIVITY_MAX_INTERVAL)) {
            bool reply = sendPing();
            if (!reply) {
                attempts++;
            } else {
                attempts = 0;
            }
            if (attempts > 5) {
                apInfo.state = AP_STATE_WAIT_RESET;
                apInfo.isOnline = false;
                if (!bringAPOnline()) {
                    // tried to reset the AP, but we failed... Maybe the AP-Tag died?
                    apInfo.state = AP_STATE_FAILED;
#ifdef HAS_RGB_LED
                    showColorPattern(CRGB::Yellow, CRGB::Yellow, CRGB::Red);
#endif
                } else {
                    apInfo.state = AP_STATE_ONLINE;
                    apInfo.isOnline = true;
                    attempts = 0;
                    refreshAllPending();
                }
            }
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}