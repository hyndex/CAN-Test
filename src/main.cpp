#include <Arduino.h>
#include <SPI.h>
#include <mcp2515.h>

// ESP32-S3 <-> MCP2515 pin mapping (8 MHz MCP2515 crystal)
#define CAN_CS_PIN   41  // SPI chip-select
#define CAN_INT_PIN  40  // Interrupt line from MCP2515 (currently polled)
#define CAN_SPI_SCK  48
#define CAN_SPI_MISO 21
#define CAN_SPI_MOSI 47

// CAN identifiers for the bidirectional ping-pong test
static constexpr uint32_t ESP_PING_ID = 0x123;  // ESP -> Pi
static constexpr uint32_t ESP_PONG_ID = 0x124;  // Pi -> ESP
static constexpr uint32_t PI_PING_ID  = 0x223;  // Pi -> ESP
static constexpr uint32_t PI_PONG_ID  = 0x224;  // ESP -> Pi

// Timing and robustness parameters
static constexpr uint32_t PING_PERIOD_MS       = 1000;  // ESP-initiated ping cadence
static constexpr uint32_t ACTIVITY_TIMEOUT_MS  = 5000;  // re-init if idle and errors accumulate
static constexpr uint8_t  ERROR_REINIT_LIMIT   = 5;     // consecutive send errors before re-init
static constexpr uint32_t HEALTH_CHECK_PERIOD_MS = 200; // controller health poll

static MCP2515 mcp2515(CAN_CS_PIN);

static struct can_frame espPingFrame;
static struct can_frame rxFrame;
static struct can_frame lastEspPingSent;
static bool             hasLastEspPing = false;
static volatile bool    canIntPending  = false;

static uint8_t  espPingCounter   = 0;
static uint32_t lastPingMillis   = 0;
static uint32_t lastActivityMs   = 0;
static uint8_t  consecutiveSendErrors = 0;
static uint8_t  consecutivePassiveErrors = 0;
static uint32_t lastHealthCheckMs = 0;

// MCP2515 EFLG bit masks (per datasheet)
static constexpr uint8_t EFLG_RX1OVR = 0x80;
static constexpr uint8_t EFLG_RX0OVR = 0x40;
static constexpr uint8_t EFLG_TXBO   = 0x20;
static constexpr uint8_t EFLG_TXEP   = 0x10;
static constexpr uint8_t EFLG_RXEP   = 0x08;
static constexpr uint8_t EFLG_TXWAR  = 0x04;
static constexpr uint8_t EFLG_RXWAR  = 0x02;
static constexpr uint8_t EFLG_EWARN  = 0x01;

static void IRAM_ATTR onCanInt()
{
    canIntPending = true;
}

// Build the fixed test payload with a simple counter for verification.
static void buildPattern(struct can_frame &frame, uint32_t id, uint8_t counter)
{
    frame.can_id  = id;
    frame.can_dlc = 8;

    frame.data[0] = counter;
    frame.data[1] = counter ^ 0xFF;
    frame.data[2] = 0x55;
    frame.data[3] = 0xAA;
    frame.data[4] = 0xC3;
    frame.data[5] = 0x3C;
    frame.data[6] = 0x5A;
    frame.data[7] = 0xA5;
}

static bool patternMatches(const struct can_frame &frame)
{
    if (frame.can_dlc != 8) {
        return false;
    }
    const uint8_t c = frame.data[0];

    return (frame.data[1] == static_cast<uint8_t>(c ^ 0xFF)) &&
           (frame.data[2] == 0x55) &&
           (frame.data[3] == 0xAA) &&
           (frame.data[4] == 0xC3) &&
           (frame.data[5] == 0x3C) &&
           (frame.data[6] == 0x5A) &&
           (frame.data[7] == 0xA5);
}

static bool framesEqual(const struct can_frame &a, const struct can_frame &b)
{
    if (a.can_dlc != b.can_dlc) {
        return false;
    }
    for (uint8_t i = 0; i < a.can_dlc; ++i) {
        if (a.data[i] != b.data[i]) {
            return false;
        }
    }
    return true;
}

static void logFrame(const char *prefix, const struct can_frame &frame)
{
    Serial.print(prefix);
    Serial.print(" ID=0x");
    Serial.print(frame.can_id, HEX);
    Serial.print(" DLC=");
    Serial.print(frame.can_dlc);
    Serial.print(" DATA=");
    for (uint8_t i = 0; i < frame.can_dlc; ++i) {
        if (frame.data[i] < 0x10) Serial.print('0');
        Serial.print(frame.data[i], HEX);
        Serial.print(' ');
    }
    Serial.println();
}

static bool initCan()
{
    mcp2515.reset();

    const auto bitrateErr = mcp2515.setBitrate(CAN_125KBPS, MCP_8MHZ);
    if (bitrateErr != MCP2515::ERROR_OK) {
        Serial.print("setBitrate failed: ");
        Serial.println(static_cast<int>(bitrateErr));
        return false;
    }

    const auto modeErr = mcp2515.setNormalMode();
    if (modeErr != MCP2515::ERROR_OK) {
        Serial.print("setNormalMode failed: ");
        Serial.println(static_cast<int>(modeErr));
        return false;
    }

    consecutiveSendErrors = 0;
    lastActivityMs = millis();
    hasLastEspPing = false;
    canIntPending  = false;
    consecutivePassiveErrors = 0;
    lastHealthCheckMs = millis();

    Serial.println("MCP2515 ready (125kbps, 8MHz).");
    return true;
}

static void recoverIfStalled(uint32_t now)
{
    if (consecutiveSendErrors >= ERROR_REINIT_LIMIT) {
        Serial.println("Too many send errors; reinitializing CAN...");
        if (initCan()) {
            consecutiveSendErrors = 0;
        }
        return;
    }

    if ((now - lastActivityMs) > ACTIVITY_TIMEOUT_MS && consecutiveSendErrors > 0) {
        Serial.println("Activity timeout with errors; attempting CAN reinit...");
        initCan();
    }
}

static void sendFrame(struct can_frame &frame)
{
    const auto err = mcp2515.sendMessage(&frame);
    if (err == MCP2515::ERROR_OK) {
        consecutiveSendErrors = 0;
        lastActivityMs = millis();
    } else {
        consecutiveSendErrors++;
        Serial.print("Send error: ");
        Serial.println(static_cast<int>(err));
    }
}

static void handleHealth(uint32_t now)
{
    if ((now - lastHealthCheckMs) < HEALTH_CHECK_PERIOD_MS) {
        return;
    }
    lastHealthCheckMs = now;

    const uint8_t flags = mcp2515.getErrorFlags();

    if (flags & (EFLG_RX0OVR | EFLG_RX1OVR)) {
        Serial.println("RX overflow detected; clearing.");
        mcp2515.clearRXnOVR();
    }

    if (flags & EFLG_TXBO) {
        Serial.println("Bus-off detected; reinitializing CAN...");
        initCan();
        return;
    }

    if (flags & (EFLG_TXEP | EFLG_RXEP)) {
        consecutivePassiveErrors++;
        if (consecutivePassiveErrors >= 3) {
            Serial.println("Error-passive persists; reinitializing CAN...");
            initCan();
            return;
        }
    } else {
        consecutivePassiveErrors = 0;
    }

    if (flags & EFLG_EWARN) {
        Serial.println("Warning: error warning flag set (EWARN).");
    }
}

static void processRxFrame(const struct can_frame &frame)
{
    // PONG for ESP-initiated PING
    if (frame.can_id == ESP_PONG_ID) {
        if (hasLastEspPing && framesEqual(lastEspPingSent, frame)) {
            Serial.println("MATCHED (ESP-initiated)");
        } else {
            Serial.println("MISMATCH (ESP-initiated)");
        }
    }
    // PING coming from Pi that ESP must echo
    else if (frame.can_id == PI_PING_ID) {
        if (patternMatches(frame)) {
            Serial.println("MATCHED (Pi->ESP PING)");
        } else {
            Serial.println("MISMATCH pattern from Pi");
        }

        struct can_frame pong = frame;
        pong.can_id = PI_PONG_ID;
        logFrame("TX PONG (ESP->Pi)", pong);
        sendFrame(pong);
    }
}

void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("ESP32-S3 MCP2515 CAN Ping-Pong (bidirectional, 8MHz MCP2515)");

    // Initialize SPI with explicit pins
    SPI.begin(CAN_SPI_SCK, CAN_SPI_MISO, CAN_SPI_MOSI, CAN_CS_PIN);
    pinMode(CAN_INT_PIN, INPUT);  // reserved for interrupt-driven reception
    attachInterrupt(digitalPinToInterrupt(CAN_INT_PIN), onCanInt, FALLING);

    if (!initCan()) {
        Serial.println("Fatal: cannot initialize MCP2515. Halting.");
        while (true) {
            delay(1000);
        }
    }
}

void loop()
{
    const uint32_t now = millis();

    // ESP-initiated PING towards Pi
    if (now - lastPingMillis >= PING_PERIOD_MS) {
        lastPingMillis = now;

        buildPattern(espPingFrame, ESP_PING_ID, espPingCounter);
        logFrame("TX PING (ESP->Pi)", espPingFrame);

        sendFrame(espPingFrame);

        lastEspPingSent = espPingFrame;
        hasLastEspPing  = true;

        espPingCounter++;
    }

    bool handledRx = false;

    // Drain all pending RX frames (interrupt-driven where available, with polling fallback).
    if (canIntPending || mcp2515.checkReceive() == MCP2515::ERROR_OK) {
        canIntPending = false;
        while (mcp2515.readMessage(&rxFrame) == MCP2515::ERROR_OK) {
            handledRx = true;
            lastActivityMs = millis();
            logFrame("RX", rxFrame);
            processRxFrame(rxFrame);
        }
    }

    handleHealth(now);
    recoverIfStalled(now);

    if (!handledRx) {
        delay(1);  // tiny backoff only when idle
    }
}
