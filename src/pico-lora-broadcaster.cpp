#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/spi.h"
#include "hardware/structs/rosc.h"

#include <string>
#include <cstring>
#include <vector>
#include <cstdlib>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "UartComms.hpp"
#include "LoRaPacket.hpp"
#include "SX1278.hpp"

#define UART_RECEIVE_TASK_PRIORITY (tskIDLE_PRIORITY + 2UL)
#define LORA_SEND_TASK_PRIORITY (tskIDLE_PRIORITY + 2UL)

struct LoRaMessage {
    char data[256];
};

QueueHandle_t lora_send_queue;

#ifdef DEVICE_seeed_xiao_rp2040
// Seeed Xaio rp2040
namespace lora_config {
    inline spi_inst_t* SPI = spi0;
    inline constexpr uint SCK   = 2;  // P2
    inline constexpr uint MOSI  = 3;  // P3
    inline constexpr uint MISO  = 4;  // P4
    inline constexpr uint CS    = 26; // P26
    inline constexpr uint RESET = 22; // not used
}
namespace uart_config {
    inline uart_inst_t* const UART_NUM = uart0;
    inline constexpr uint BAUD = 115200;
    inline constexpr uint TX = 0;
    inline constexpr uint RX = 1;
}
#elif defined(DEVICE_pico2)
namespace lora_config {
    inline spi_inst_t* SPI = spi0;
    inline constexpr uint SCK   = 18;
    inline constexpr uint MOSI  = 19;
    inline constexpr uint MISO  = 16;
    inline constexpr uint CS    = 17;
    inline constexpr uint RESET = 20;
}
#else
#error "No supported DEVICE defined"
#endif

/*
 * Create a fairly unique id that can be used as a session id for lora packets.
 * current microsecond timer
 * RP2040 ring-oscillator random bits
 * a RAM address
 * a small xorshift mix
 */
uint32_t create_lora_session_id() {
    uint32_t value = time_us_32();

    value ^= rosc_hw->randombit << 0;
    value ^= rosc_hw->randombit << 7;
    value ^= rosc_hw->randombit << 13;
    value ^= rosc_hw->randombit << 21;
    value ^= rosc_hw->randombit << 29;

    value ^= static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&value));

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;

    return value;
}

void lora_send_weather_data_task(void* params) {
    SX1278* pLora = static_cast<SX1278*>(params);

    uint32_t sequence = 0;

    // Session id to prevent duplicate packet warnings at the receiver
    uint32_t sessionId = create_lora_session_id();

    printf("LoRa session=%lu\n", static_cast<unsigned long>(sessionId));

    LoRaMessage queuedMessage;

    while (true) {
        if (xQueueReceive(lora_send_queue, &queuedMessage, portMAX_DELAY) == pdTRUE) {
            uint32_t currentSequence = sequence++;

            PacketHeader header {
                .sequence = currentSequence,
                .sessionId = sessionId,
                .version = 1,
                .type = PacketType::Weather
            };

            char* p = queuedMessage.data;
            char* end;

            WeatherPayload weather_payload {};

            // Leapfrog the commas to parse the csv:
            // p
            // 947483372,669596416,15.0,100.0,1015.6,7.5,16.9,SW,225,14,2720.4,0.00
            // strtoul(p, &end, 10)
            // p        e
            // 947483372,669596416,15.0,100.0,1015.6,7.5,16.9,SW,225,14,2720.4,0.00
            // p = end + 1;
            //          ep
            // 947483372,669596416,15.0,100.0,1015.6,7.5,16.9,SW,225,14,2720.4,0.00
            // and so on

            weather_payload.timestamp = static_cast<decltype(weather_payload.timestamp)>
                (strtoul(p, &end, 10));
            p = end + 1;

            weather_payload.bootId = static_cast<decltype(weather_payload.bootId)>
                (strtoul(p, &end, 10));
            p = end + 1;
            
            weather_payload.temperature = strtof(p, &end);
            p = end + 1;

            weather_payload.pressure = strtof(p, &end);
            p = end + 1;

            weather_payload.humidity = strtof(p, &end);
            p = end + 1;

            weather_payload.windSpeed = strtof(p, &end);
            p = end + 1;

            weather_payload.windGust = strtof(p, &end);
            p = end + 1;

            // Wind direction name, find the comma first and copy only that field
            // currently:
            // ep
            // ,SW,225,14,2720.4,0.00
            end = strchr(p, ',');
            // now:
            //  p e
            // ,SW,225,14,2720.4,0.00
            // ensure we don't copy more than 3 chars into windDirectionName
            // e.g. NNW is the max we support
            size_t safe_len = std::min(static_cast<size_t>(end - p), // 2 or 3 depending on direction
                sizeof(weather_payload.windDirectionName) - 1);      // 3, always space for \0
            memcpy(weather_payload.windDirectionName, p, safe_len);
            weather_payload.windDirectionName[safe_len] = '\0';
            p = end + 1;

            weather_payload.windDirectionDegrees =
                static_cast<decltype(weather_payload.windDirectionDegrees)>
                (strtoul(p, &end, 10));
            p = end + 1;

            weather_payload.lux = strtof(p, &end);
            p = end + 1;

            weather_payload.rainTipsSinceBoot = strtof(p, &end);
            p = end + 1;

            weather_payload.batteryVoltage = strtof(p, &end);
            p = end + 1;

            std::vector<uint8_t> packet(sizeof(PacketHeader) + sizeof(WeatherPayload));

            std::memcpy(packet.data(), &header, sizeof(PacketHeader));

            std::memcpy(packet.data() + sizeof(PacketHeader), &weather_payload,
                sizeof(WeatherPayload));

            if (pLora->send(packet.data(), packet.size())) {
                printf(
                    "TX seq=%lu "
                    "timestamp=%lu "
                    "bootId=%d "
                    "temp=%.1f pressure=%.1f humidity=%.1f "
                    "wind=%.1f gust=%.1f "
                    "dir=%s deg=%.1f "
                    "lux=%.1f "
                    "rain=%d "
                    "battery=%.2f\n",

                    static_cast<unsigned long>(currentSequence),
                    static_cast<unsigned long>(weather_payload.timestamp),
                    weather_payload.bootId,
                    weather_payload.temperature,
                    weather_payload.pressure,
                    weather_payload.humidity,
                    weather_payload.windSpeed,
                    weather_payload.windGust,
                    weather_payload.windDirectionName,
                    static_cast<unsigned>(weather_payload.windDirectionDegrees),
                    weather_payload.lux,
                    weather_payload.rainTipsSinceBoot,
                    weather_payload.batteryVoltage
                );
            }
            else {
                printf("TX failed seq=%lu\n", static_cast<unsigned long>(currentSequence));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void uart_receive_task(void* params) {
    UartComms* pUartComms = static_cast<UartComms*>(params);

    while (true) {
        std::string received_message;
        while (pUartComms->receive(received_message)) {
            gpio_put(PICO_DEFAULT_LED_PIN, 1);
            sleep_ms(500);
            gpio_put(PICO_DEFAULT_LED_PIN, 0);
            sleep_ms(500);
            gpio_put(PICO_DEFAULT_LED_PIN, 1);
            sleep_ms(500);
            gpio_put(PICO_DEFAULT_LED_PIN, 0);
            printf("received: '%s'\n", received_message.c_str());
            LoRaMessage lora_message {};
            std::strncpy(lora_message.data, received_message.c_str(), sizeof(lora_message.data) - 1);
            xQueueSend(lora_send_queue, &lora_message, portMAX_DELAY);
        }

        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

int main( void )
{
    stdio_init_all();

    sleep_ms(2000);

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    UartComms uartComms(
        uart_config::UART_NUM,
        uart_config::BAUD,
        uart_config::TX,
        uart_config::RX
    );
    uartComms.init();
    
    SX1278Config config;
    config.frequencyHz = 433920000;
    config.bandwidth = LoRaBandwidth::BW_125_KHZ;
    config.codingRate = LoRaCodingRate::CR_4_5;
    config.spreadingFactor = 7;
    config.crcEnabled = true;
    config.preambleLength = 8;
    config.syncWord = 0x12;
    config.txPowerDbm = 10;

    SX1278 lora(
        lora_config::SPI,
        lora_config::CS,
        lora_config::RESET,
        lora_config::SCK,
        lora_config::MOSI,
        lora_config::MISO
    );

    if (lora.init(config)) {
        printf("SX1278 detected, version: 0x%02X\n", lora.getVersion());

        lora_send_queue = xQueueCreate(8, sizeof(LoRaMessage));
        if (lora_send_queue == nullptr) {
            printf("Failed to create LoRa queue ... not starting\n");
            while (true) {
                tight_loop_contents();
            }
        }

        xTaskCreate(uart_receive_task, "UartReceiveTask", 512, (void*)&uartComms, UART_RECEIVE_TASK_PRIORITY, nullptr);
        
        xTaskCreate(lora_send_weather_data_task, "LoRaSendWeatherDataTask", 2048, (void*)&lora, LORA_SEND_TASK_PRIORITY, nullptr);
        
        vTaskStartScheduler();
    }
    else {
        printf("SX1278 not detected ... not starting\n");
    }

    while (true) { tight_loop_contents(); }
}
