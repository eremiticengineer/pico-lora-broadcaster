#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"

#include <string>
#include <cstring>
#include <vector>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "UartComms.hpp"
#include "SX1278.hpp"
#include "LoRaPacket.hpp"

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

namespace uart_config {
    inline uart_inst_t* const UART_NUM = uart0;
    inline constexpr uint BAUD = 115200;
    inline constexpr uint TX = 0;
    inline constexpr uint RX = 1;
}
#else
#error "No supported DEVICE defined"
#endif

void lora_send_task(void* params) {
    SX1278* pLora = static_cast<SX1278*>(params);

    uint32_t sequence = 0;

    LoRaMessage queuedMessage;

    while (true) {
        if (xQueueReceive(lora_send_queue, &queuedMessage, portMAX_DELAY) == pdTRUE) {
            uint32_t currentSequence = sequence++;

            PacketHeader header {
                .sequence = currentSequence,
                .version = 1,
                .type = PacketType::Status
            };

            std::string message(queuedMessage.data);
            std::vector<uint8_t> packet(sizeof(PacketHeader) + message.size());

            /*
            * Packet layout:
            *
            * [ PacketHeader ][ payload bytes ]
            */

            std::memcpy(packet.data(), &header, sizeof(PacketHeader));

            std::memcpy(packet.data() + sizeof(PacketHeader), message.data(), message.size());

            if (pLora->send(packet.data(), packet.size())) {
                printf("TX seq=%lu version=%u type=%u: %s\n",
                    static_cast<unsigned long>(currentSequence),
                    static_cast<unsigned>(header.version),
                    static_cast<unsigned>(header.type),
                    message.c_str());
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

    lora_send_queue = xQueueCreate(8, sizeof(LoRaMessage));

    UartComms uartComms(
        uart_config::UART_NUM,
        uart_config::BAUD,
        uart_config::TX,
        uart_config::RX
    );
    uartComms.init();
    xTaskCreate(uart_receive_task, "UartReceiveTask", 512, (void*)&uartComms, UART_RECEIVE_TASK_PRIORITY, nullptr);

    SX1278Config config;
    config.frequencyHz = 433000000;
    config.bandwidth = LoRaBandwidth::BW_125_KHZ;
    config.codingRate = LoRaCodingRate::CR_4_5;
    config.spreadingFactor = 7;
    config.crcEnabled = true;
    config.preambleLength = 8;
    config.syncWord = 0x12;
    config.txPowerDbm = 17;

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
        xTaskCreate(lora_send_task, "LoRaSendTask", 512, (void*)&lora, LORA_SEND_TASK_PRIORITY, nullptr);
    }
    else {
        printf("SX1278 not detected\n");
    }

    vTaskStartScheduler();

    while (true) { tight_loop_contents(); }
}