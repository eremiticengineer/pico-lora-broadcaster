# Pico LoRa Broadcaster

System that accepts data over UART and broadcasts it over LoRa.

work in progress...

## Cloning the project

Clone the project with FreeRTOS and sensor submodules to get the pico functionality:

```
git clone https://github.com/eremiticengineer/pico-lora-broadcaster
cd pico-lora-broadcaster
git submodule update --init --progress --jobs 4
git -C lib/FreeRTOS-Kernel submodule update --init --recursive --progress
```

## UK regulations

In the UK, OFCOM state the 433Mhz band transmission requirements in the [Draft UK Interface
Requirement (IR) 2030](https://www.ofcom.org.uk/siteassets/resources/documents/consultations/category-2-6-weeks/consultation-notice-of-proposals-to-make-wireless-telegraphy-regulations-2025/main-docs/draft-ir-2030-2025.pdf?v=409289) (pdf). On Page 8 (accessed 5/10/26) it states the 433.05MHz - 434.79MHz band must have a duty cycle of <= 10% at 10mW effective radiated power (e.r.p.) (IR2030/1/10). A rough guide to **SX1278Config.txPowerDbm** is given below.

|    dBm |            mW |
| -----: | ------------: |
|  0 dBm |          1 mW |
|  3 dBm |          2 mW |
|  6 dBm |          4 mW |
| 10 dBm |         10 mW |
| 13 dBm |         20 mW |
| 17 dBm |         50 mW |
| 20 dBm |        100 mW |
| 30 dBm | 1000 mW = 1 W |

**SX1278Config.txPowerDbm** is set to **10** by default to comply with the ODCOM regulations in the UK.

A worked example for the default SX1278Config settings is:

```
Symbol airtime
  Tsym = (2^SF)/BW = (2^7) / 125000 = 0.001024 = 1.024ms

Preamble airtime
LoRa adds 4.25 symbols to the configured preamble length
  Npreamble = (preamble length + 4.25) x Tsym = (8 + 4.25) x 1.024 = 12.544ms

Work out how many payload symbols are required per broadcast
  PL = payload length = 56
  SF = spreading factor = 7
  CRC = 1 as crcEnabled = true
  IH = 0 as assuming explicit header
  DE = 0 as low-data-rate optimisation isn't needed at SF7/BW125
  CR = 1 for coding rate 4/5
  Npayload = 8 + ceil((8PL - 4SF + 28 + 16CRC - 20IH) / (4(SF - 2DE))) x (CR + 4)
    => 8 + ceil((448 - 28 + 28 + 16) / 28) x 5
    => 8 + ceil(464 / 28) x 5
    => 8 + ceil(16.57142857) x 5
    => 8 + (17 x 5)
    => 93 payload symbols required to transmit one weather station packet
    => Tsym = 1.024
    => Tpayload = 95.232ms
    => Tpreamble + Tpayload
    => 12.544 + 95.232 = 107.776ms
    => each broadcast occupies the air for about 108ms
    => 10% duty cycle
    -----------------
```

A 10% duty cycle means that over any representative period, the station may occupy the channel for at most 10% of the time. Over one hour:

```
3600 × (10 / 100) = 360 seconds
```

There is therefore a 360s window each hour the station can transmit, which means the maximum number of 56 byte payload packets per hour is:

```
360 / 0.108 ~= 3333
```

which equates to approximately **3333 broadcasts per hour** assuming an average broadcast airtime of 108ms.

3600 1s blocks in an hour, each one with a broadcast would give:

```
Duty Cycle % = (total transmit airtime / elapsed time) x 100
  => (0.108 x 3600) / 3600 x 100
  => 10.8%
```

Broadcasting every second each hour would be over the 10% limit so we need to reduce the broadcast rate to reduce the duty cycle.

If the station transmits every 10 seconds, with 360 x 10 second blocks in an hour, where during each 10s block there will be 108ms airtime:

```
Duty Cycle % = (total transmit airtime / elapsed time) x 100
  => (0.108 x 360) / 3600 x 100
  => 1.08%
```

Broadcasting 360 packets per hour would be 360 x 108ms ~ broadcasting for 39s per hour.

## FreeRTOS-Kernal setup for new projects

When creating a FreeRTOS project from scratch, clone the main branch into the project. The main branch at the moment has the necessary pico functionality:

```
git init
git submodule add https://github.com/FreeRTOS/FreeRTOS-Kernel.git lib/FreeRTOS-Kernel
git submodule update --init --recursive --progress
git submodule add https://github.com/eremiticengineer/pico-uart-comms lib/pico-uart-comms
git submodule add https://github.com/eremiticengineer/pico-lora lib/pico-lora
git add .
```

## FreeRTOSConfig.h

This file customises FreeRTOS for your project. The file:

```
include/FreeRTOSConfig.h
```

is this one from the pico-examples:

```
pico-examples/freertos/FreeRTOSConfig_examples_common.h
```

## References

* [Task priorites](https://www.freertos.org/Documentation/02-Kernel/02-Kernel-features/01-Tasks-and-co-routines/03-Task-priorities)
* [uxTaskGetStackHighWaterMark](https://www.freertos.org/Documentation/02-Kernel/04-API-references/03-Task-utilities/04-uxTaskGetStackHighWaterMark)
