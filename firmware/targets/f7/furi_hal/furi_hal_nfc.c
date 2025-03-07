#include <limits.h>
#include "furi_hal_nfc.h"
#include <st25r3916.h>
#include <st25r3916_irq.h>
#include <rfal_rf.h>
#include <furi.h>
#include <m-string.h>

#include <lib/digital_signal/digital_signal.h>
#include <furi_hal_delay.h>

#define TAG "FuriHalNfc"

static const uint32_t clocks_in_ms = 64 * 1000;

osEventFlagsId_t event = NULL;
#define EVENT_FLAG_INTERRUPT (1UL << 0)
#define EVENT_FLAG_STATE_CHANGED (1UL << 1)
#define EVENT_FLAG_STOP (1UL << 2)
#define EVENT_FLAG_ALL (EVENT_FLAG_INTERRUPT | EVENT_FLAG_STATE_CHANGED | EVENT_FLAG_STOP)

void furi_hal_nfc_init() {
    ReturnCode ret = rfalNfcInitialize();
    if(ret == ERR_NONE) {
        furi_hal_nfc_start_sleep();
        event = osEventFlagsNew(NULL);
        FURI_LOG_I(TAG, "Init OK");
    } else {
        FURI_LOG_W(TAG, "Initialization failed, RFAL returned: %d", ret);
    }
}

bool furi_hal_nfc_is_busy() {
    return rfalNfcGetState() != RFAL_NFC_STATE_IDLE;
}

void furi_hal_nfc_field_on() {
    furi_hal_nfc_exit_sleep();
    st25r3916TxRxOn();
}

void furi_hal_nfc_field_off() {
    st25r3916TxRxOff();
    furi_hal_nfc_start_sleep();
}

void furi_hal_nfc_start_sleep() {
    rfalLowPowerModeStart();
}

void furi_hal_nfc_exit_sleep() {
    rfalLowPowerModeStop();
}

bool furi_hal_nfc_detect(FuriHalNfcDevData* nfc_data, uint32_t timeout) {
    furi_assert(nfc_data);

    rfalNfcDevice* dev_list = NULL;
    uint8_t dev_cnt = 0;
    bool detected = false;

    rfalLowPowerModeStop();
    rfalNfcState state = rfalNfcGetState();
    rfalNfcState state_old = 0;
    if(state == RFAL_NFC_STATE_NOTINIT) {
        rfalNfcInitialize();
    }
    rfalNfcDiscoverParam params;
    params.compMode = RFAL_COMPLIANCE_MODE_EMV;
    params.techs2Find = RFAL_NFC_POLL_TECH_A | RFAL_NFC_POLL_TECH_B | RFAL_NFC_POLL_TECH_F |
                        RFAL_NFC_POLL_TECH_V | RFAL_NFC_POLL_TECH_AP2P | RFAL_NFC_POLL_TECH_ST25TB;
    params.totalDuration = 1000;
    params.devLimit = 3;
    params.wakeupEnabled = false;
    params.wakeupConfigDefault = true;
    params.nfcfBR = RFAL_BR_212;
    params.ap2pBR = RFAL_BR_424;
    params.maxBR = RFAL_BR_KEEP;
    params.GBLen = RFAL_NFCDEP_GB_MAX_LEN;
    params.notifyCb = NULL;

    uint32_t start = DWT->CYCCNT;
    rfalNfcDiscover(&params);
    while(true) {
        rfalNfcWorker();
        state = rfalNfcGetState();
        if(state != state_old) {
            FURI_LOG_T(TAG, "State change %d -> %d", state_old, state);
        }
        state_old = state;
        if(state == RFAL_NFC_STATE_ACTIVATED) {
            detected = true;
            break;
        }
        if(state == RFAL_NFC_STATE_POLL_ACTIVATION) {
            start = DWT->CYCCNT;
            continue;
        }
        if(state == RFAL_NFC_STATE_POLL_SELECT) {
            rfalNfcSelect(0);
        }
        if(DWT->CYCCNT - start > timeout * clocks_in_ms) {
            rfalNfcDeactivate(true);
            FURI_LOG_T(TAG, "Timeout");
            break;
        }
        osDelay(1);
    }
    rfalNfcGetDevicesFound(&dev_list, &dev_cnt);
    if(detected) {
        if(dev_list[0].type == RFAL_NFC_LISTEN_TYPE_NFCA) {
            nfc_data->type = FuriHalNfcTypeA;
            nfc_data->atqa[0] = dev_list[0].dev.nfca.sensRes.anticollisionInfo;
            nfc_data->atqa[1] = dev_list[0].dev.nfca.sensRes.platformInfo;
            nfc_data->sak = dev_list[0].dev.nfca.selRes.sak;
            uint8_t* cuid_start = dev_list[0].nfcid;
            if(dev_list[0].nfcidLen == 7) {
                cuid_start = &dev_list[0].nfcid[3];
            }
            nfc_data->cuid = (cuid_start[0] << 24) | (cuid_start[1] << 16) | (cuid_start[2] << 8) |
                             (cuid_start[3]);
        } else if(dev_list[0].type == RFAL_NFC_LISTEN_TYPE_NFCB) {
            nfc_data->type = FuriHalNfcTypeB;
        } else if(dev_list[0].type == RFAL_NFC_LISTEN_TYPE_NFCF) {
            nfc_data->type = FuriHalNfcTypeF;
        } else if(dev_list[0].type == RFAL_NFC_LISTEN_TYPE_NFCV) {
            nfc_data->type = FuriHalNfcTypeV;
        }
        if(dev_list[0].rfInterface == RFAL_NFC_INTERFACE_RF) {
            nfc_data->interface = FuriHalNfcInterfaceRf;
        } else if(dev_list[0].rfInterface == RFAL_NFC_INTERFACE_ISODEP) {
            nfc_data->interface = FuriHalNfcInterfaceIsoDep;
        } else if(dev_list[0].rfInterface == RFAL_NFC_INTERFACE_NFCDEP) {
            nfc_data->interface = FuriHalNfcInterfaceNfcDep;
        }
        nfc_data->uid_len = dev_list[0].nfcidLen;
        memcpy(nfc_data->uid, dev_list[0].nfcid, nfc_data->uid_len);
    }

    return detected;
}

bool furi_hal_nfc_activate_nfca(uint32_t timeout, uint32_t* cuid) {
    rfalNfcDevice* dev_list;
    uint8_t dev_cnt = 0;
    rfalLowPowerModeStop();
    rfalNfcState state = rfalNfcGetState();
    if(state == RFAL_NFC_STATE_NOTINIT) {
        rfalNfcInitialize();
    }
    rfalNfcDiscoverParam params = {
        .compMode = RFAL_COMPLIANCE_MODE_NFC,
        .techs2Find = RFAL_NFC_POLL_TECH_A,
        .totalDuration = 1000,
        .devLimit = 3,
        .wakeupEnabled = false,
        .wakeupConfigDefault = true,
        .nfcfBR = RFAL_BR_212,
        .ap2pBR = RFAL_BR_424,
        .maxBR = RFAL_BR_KEEP,
        .GBLen = RFAL_NFCDEP_GB_MAX_LEN,
        .notifyCb = NULL,
    };
    uint32_t start = DWT->CYCCNT;
    rfalNfcDiscover(&params);
    while(state != RFAL_NFC_STATE_ACTIVATED) {
        rfalNfcWorker();
        state = rfalNfcGetState();
        FURI_LOG_T(TAG, "Current state %d", state);
        if(state == RFAL_NFC_STATE_POLL_ACTIVATION) {
            start = DWT->CYCCNT;
            continue;
        }
        if(state == RFAL_NFC_STATE_POLL_SELECT) {
            rfalNfcSelect(0);
        }
        if(DWT->CYCCNT - start > timeout * clocks_in_ms) {
            rfalNfcDeactivate(true);
            FURI_LOG_T(TAG, "Timeout");
            return false;
        }
        osThreadYield();
    }
    rfalNfcGetDevicesFound(&dev_list, &dev_cnt);
    // Take first device and set cuid
    if(cuid) {
        uint8_t* cuid_start = dev_list[0].nfcid;
        if(dev_list[0].nfcidLen == 7) {
            cuid_start = &dev_list[0].nfcid[3];
        }
        *cuid = (cuid_start[0] << 24) | (cuid_start[1] << 16) | (cuid_start[2] << 8) |
                (cuid_start[3]);
        FURI_LOG_T(TAG, "Activated tag with cuid: %lX", *cuid);
    }
    return true;
}

bool furi_hal_nfc_listen(
    uint8_t* uid,
    uint8_t uid_len,
    uint8_t* atqa,
    uint8_t sak,
    bool activate_after_sak,
    uint32_t timeout) {
    rfalNfcState state = rfalNfcGetState();
    if(state == RFAL_NFC_STATE_NOTINIT) {
        rfalNfcInitialize();
    } else if(state >= RFAL_NFC_STATE_ACTIVATED) {
        rfalNfcDeactivate(false);
    }
    rfalLowPowerModeStop();
    rfalNfcDiscoverParam params = {
        .compMode = RFAL_COMPLIANCE_MODE_NFC,
        .techs2Find = RFAL_NFC_LISTEN_TECH_A,
        .totalDuration = 1000,
        .devLimit = 1,
        .wakeupEnabled = false,
        .wakeupConfigDefault = true,
        .nfcfBR = RFAL_BR_212,
        .ap2pBR = RFAL_BR_424,
        .maxBR = RFAL_BR_KEEP,
        .GBLen = RFAL_NFCDEP_GB_MAX_LEN,
        .notifyCb = NULL,
        .activate_after_sak = activate_after_sak,
    };
    params.lmConfigPA.nfcidLen = uid_len;
    memcpy(params.lmConfigPA.nfcid, uid, uid_len);
    params.lmConfigPA.SENS_RES[0] = atqa[0];
    params.lmConfigPA.SENS_RES[1] = atqa[1];
    params.lmConfigPA.SEL_RES = sak;
    rfalNfcDiscover(&params);

    uint32_t start = DWT->CYCCNT;
    while(state != RFAL_NFC_STATE_ACTIVATED) {
        rfalNfcWorker();
        state = rfalNfcGetState();
        if(DWT->CYCCNT - start > timeout * clocks_in_ms) {
            rfalNfcDeactivate(true);
            return false;
        }
        osDelay(1);
    }
    return true;
}

void rfal_interrupt_callback_handler() {
    osEventFlagsSet(event, EVENT_FLAG_INTERRUPT);
}

void rfal_state_changed_callback(void* context) {
    UNUSED(context);
    osEventFlagsSet(event, EVENT_FLAG_STATE_CHANGED);
}

void furi_hal_nfc_stop() {
    if(event) {
        osEventFlagsSet(event, EVENT_FLAG_STOP);
    }
}

bool furi_hal_nfc_emulate_nfca(
    uint8_t* uid,
    uint8_t uid_len,
    uint8_t* atqa,
    uint8_t sak,
    FuriHalNfcEmulateCallback callback,
    void* context,
    uint32_t timeout) {
    rfalSetUpperLayerCallback(rfal_interrupt_callback_handler);
    rfal_set_state_changed_callback(rfal_state_changed_callback);

    rfalLmConfPA config;
    config.nfcidLen = uid_len;
    memcpy(config.nfcid, uid, uid_len);
    memcpy(config.SENS_RES, atqa, RFAL_LM_SENS_RES_LEN);
    config.SEL_RES = sak;
    uint8_t buff_rx[256];
    uint16_t buff_rx_size = 256;
    uint16_t buff_rx_len = 0;
    uint8_t buff_tx[1040];
    uint16_t buff_tx_len = 0;
    uint32_t data_type = FURI_HAL_NFC_TXRX_DEFAULT;

    rfalLowPowerModeStop();
    if(rfalListenStart(
           RFAL_LM_MASK_NFCA,
           &config,
           NULL,
           NULL,
           buff_rx,
           rfalConvBytesToBits(buff_rx_size),
           &buff_rx_len)) {
        rfalListenStop();
        FURI_LOG_E(TAG, "Failed to start listen mode");
        return false;
    }
    while(true) {
        buff_rx_len = 0;
        buff_tx_len = 0;
        uint32_t flag = osEventFlagsWait(event, EVENT_FLAG_ALL, osFlagsWaitAny, timeout);
        if(flag == osFlagsErrorTimeout || flag == EVENT_FLAG_STOP) {
            break;
        }
        bool data_received = false;
        buff_rx_len = 0;
        rfalWorker();
        rfalLmState state = rfalListenGetState(&data_received, NULL);
        if(data_received) {
            rfalTransceiveBlockingRx();
            if(nfca_emulation_handler(buff_rx, buff_rx_len, buff_tx, &buff_tx_len)) {
                if(rfalListenSleepStart(
                       RFAL_LM_STATE_SLEEP_A,
                       buff_rx,
                       rfalConvBytesToBits(buff_rx_size),
                       &buff_rx_len)) {
                    FURI_LOG_E(TAG, "Failed to enter sleep mode");
                    break;
                } else {
                    continue;
                }
            }
            if(buff_tx_len) {
                ReturnCode ret = rfalTransceiveBitsBlockingTx(
                    buff_tx,
                    buff_tx_len,
                    buff_rx,
                    sizeof(buff_rx),
                    &buff_rx_len,
                    data_type,
                    RFAL_FWT_NONE);
                if(ret) {
                    FURI_LOG_E(TAG, "Tranceive failed with status %d", ret);
                    break;
                }
                continue;
            }
            if((state == RFAL_LM_STATE_ACTIVE_A || state == RFAL_LM_STATE_ACTIVE_Ax)) {
                if(callback) {
                    callback(buff_rx, buff_rx_len, buff_tx, &buff_tx_len, &data_type, context);
                }
                if(!rfalIsExtFieldOn()) {
                    break;
                }
                if(buff_tx_len) {
                    if(buff_tx_len == UINT16_MAX) buff_tx_len = 0;

                    ReturnCode ret = rfalTransceiveBitsBlockingTx(
                        buff_tx,
                        buff_tx_len,
                        buff_rx,
                        sizeof(buff_rx),
                        &buff_rx_len,
                        data_type,
                        RFAL_FWT_NONE);
                    if(ret) {
                        FURI_LOG_E(TAG, "Tranceive failed with status %d", ret);
                        continue;
                    }
                } else {
                    break;
                }
            }
        }
    }
    rfalListenStop();
    return true;
}

static bool furi_hal_nfc_transparent_tx_rx(FuriHalNfcTxRxContext* tx_rx, uint16_t timeout_ms) {
    furi_assert(tx_rx->nfca_signal);

    platformDisableIrqCallback();

    bool ret = false;

    // Start transparent mode
    st25r3916ExecuteCommand(ST25R3916_CMD_TRANSPARENT_MODE);
    // Reconfigure gpio
    furi_hal_spi_bus_handle_deinit(&furi_hal_spi_bus_handle_nfc);
    furi_hal_gpio_init(&gpio_spi_r_sck, GpioModeInput, GpioPullUp, GpioSpeedLow);
    furi_hal_gpio_init(&gpio_spi_r_miso, GpioModeInput, GpioPullUp, GpioSpeedLow);
    furi_hal_gpio_init(&gpio_nfc_cs, GpioModeInput, GpioPullUp, GpioSpeedLow);
    furi_hal_gpio_init(&gpio_spi_r_mosi, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);
    furi_hal_gpio_write(&gpio_spi_r_mosi, false);

    // Send signal
    nfca_signal_encode(tx_rx->nfca_signal, tx_rx->tx_data, tx_rx->tx_bits, tx_rx->tx_parity);
    digital_signal_send(tx_rx->nfca_signal->tx_signal, &gpio_spi_r_mosi);
    furi_hal_gpio_write(&gpio_spi_r_mosi, false);

    // Configure gpio back to SPI and exit transparent
    furi_hal_spi_bus_handle_init(&furi_hal_spi_bus_handle_nfc);
    st25r3916ExecuteCommand(ST25R3916_CMD_UNMASK_RECEIVE_DATA);

    if(tx_rx->sniff_tx) {
        tx_rx->sniff_tx(tx_rx->tx_data, tx_rx->tx_bits, false, tx_rx->sniff_context);
    }

    // Manually wait for interrupt
    furi_hal_gpio_init(&gpio_rfid_pull, GpioModeInput, GpioPullDown, GpioSpeedVeryHigh);
    st25r3916ClearAndEnableInterrupts(ST25R3916_IRQ_MASK_RXE);

    uint32_t irq = 0;
    uint8_t rxe = 0;
    uint32_t start = DWT->CYCCNT;
    while(true) {
        if(furi_hal_gpio_read(&gpio_rfid_pull) == true) {
            st25r3916ReadRegister(ST25R3916_REG_IRQ_MAIN, &rxe);
            if(rxe & (1 << 4)) {
                irq = 1;
                break;
            }
        }
        uint32_t timeout = DWT->CYCCNT - start;
        if(timeout / furi_hal_delay_instructions_per_microsecond() > timeout_ms * 1000) {
            FURI_LOG_D(TAG, "Interrupt waiting timeout");
            break;
        }
    }
    if(irq) {
        uint8_t fifo_stat[2];
        st25r3916ReadMultipleRegisters(
            ST25R3916_REG_FIFO_STATUS1, fifo_stat, ST25R3916_FIFO_STATUS_LEN);
        uint16_t len =
            ((((uint16_t)fifo_stat[1] & ST25R3916_REG_FIFO_STATUS2_fifo_b_mask) >>
              ST25R3916_REG_FIFO_STATUS2_fifo_b_shift)
             << RFAL_BITS_IN_BYTE);
        len |= (((uint16_t)fifo_stat[0]) & 0x00FFU);
        uint8_t rx[100];
        st25r3916ReadFifo(rx, len);

        tx_rx->rx_bits = len * 8;
        memcpy(tx_rx->rx_data, rx, len);

        if(tx_rx->sniff_rx) {
            tx_rx->sniff_rx(tx_rx->rx_data, tx_rx->rx_bits, false, tx_rx->sniff_context);
        }

        ret = true;
    } else {
        FURI_LOG_E(TAG, "Timeout error");
        ret = false;
    }

    st25r3916ClearInterrupts();
    platformEnableIrqCallback();

    return ret;
}

static uint32_t furi_hal_nfc_tx_rx_get_flag(FuriHalNfcTxRxType type) {
    uint32_t flags = 0;

    if(type == FuriHalNfcTxRxTypeRxNoCrc) {
        flags = RFAL_TXRX_FLAGS_CRC_RX_KEEP;
    } else if(type == FuriHalNfcTxRxTypeRxKeepPar) {
        flags = RFAL_TXRX_FLAGS_CRC_TX_MANUAL | RFAL_TXRX_FLAGS_CRC_RX_KEEP |
                RFAL_TXRX_FLAGS_PAR_RX_KEEP;
    } else if(type == FuriHalNfcTxRxTypeRaw) {
        flags = RFAL_TXRX_FLAGS_CRC_TX_MANUAL | RFAL_TXRX_FLAGS_CRC_RX_KEEP |
                RFAL_TXRX_FLAGS_PAR_RX_KEEP | RFAL_TXRX_FLAGS_PAR_TX_NONE;
    } else if(type == FuriHalNfcTxRxTypeRxRaw) {
        flags = RFAL_TXRX_FLAGS_CRC_TX_MANUAL | RFAL_TXRX_FLAGS_CRC_RX_KEEP |
                RFAL_TXRX_FLAGS_PAR_RX_KEEP | RFAL_TXRX_FLAGS_PAR_TX_NONE;
    }

    return flags;
}

static uint16_t furi_hal_nfc_data_and_parity_to_bitstream(
    uint8_t* data,
    uint16_t len,
    uint8_t* parity,
    uint8_t* out) {
    furi_assert(data);
    furi_assert(out);

    uint8_t next_par_bit = 0;
    uint16_t curr_bit_pos = 0;
    for(uint16_t i = 0; i < len; i++) {
        next_par_bit = FURI_BIT(parity[i / 8], 7 - (i % 8));
        if(curr_bit_pos % 8 == 0) {
            out[curr_bit_pos / 8] = data[i];
            curr_bit_pos += 8;
            out[curr_bit_pos / 8] = next_par_bit;
            curr_bit_pos++;
        } else {
            out[curr_bit_pos / 8] |= data[i] << curr_bit_pos % 8;
            out[curr_bit_pos / 8 + 1] = data[i] >> (8 - curr_bit_pos % 8);
            out[curr_bit_pos / 8 + 1] |= next_par_bit << curr_bit_pos % 8;
            curr_bit_pos += 9;
        }
    }
    return curr_bit_pos;
}

uint16_t furi_hal_nfc_bitstream_to_data_and_parity(
    uint8_t* in_buff,
    uint16_t in_buff_bits,
    uint8_t* out_data,
    uint8_t* out_parity) {
    if(in_buff_bits % 9 != 0) {
        return 0;
    }

    uint8_t curr_byte = 0;
    uint16_t bit_processed = 0;
    memset(out_parity, 0, in_buff_bits / 9);
    while(bit_processed < in_buff_bits) {
        out_data[curr_byte] = in_buff[bit_processed / 8] >> bit_processed % 8;
        out_data[curr_byte] |= in_buff[bit_processed / 8 + 1] << (8 - bit_processed % 8);
        out_parity[curr_byte / 8] |= FURI_BIT(in_buff[bit_processed / 8 + 1], bit_processed % 8)
                                     << (7 - curr_byte % 8);
        bit_processed += 9;
        curr_byte++;
    }
    return curr_byte;
}

bool furi_hal_nfc_tx_rx(FuriHalNfcTxRxContext* tx_rx, uint16_t timeout_ms) {
    furi_assert(tx_rx);

    ReturnCode ret;
    rfalNfcState state = RFAL_NFC_STATE_ACTIVATED;
    uint8_t temp_tx_buff[FURI_HAL_NFC_DATA_BUFF_SIZE] = {};
    uint16_t temp_tx_bits = 0;
    uint8_t* temp_rx_buff = NULL;
    uint16_t* temp_rx_bits = NULL;

    if(tx_rx->tx_rx_type == FuriHalNfcTxRxTransparent) {
        return furi_hal_nfc_transparent_tx_rx(tx_rx, timeout_ms);
    }

    // Prepare data for FIFO if necessary
    uint32_t flags = furi_hal_nfc_tx_rx_get_flag(tx_rx->tx_rx_type);
    if(tx_rx->tx_rx_type == FuriHalNfcTxRxTypeRaw) {
        temp_tx_bits = furi_hal_nfc_data_and_parity_to_bitstream(
            tx_rx->tx_data, tx_rx->tx_bits / 8, tx_rx->tx_parity, temp_tx_buff);
        ret = rfalNfcDataExchangeCustomStart(
            temp_tx_buff, temp_tx_bits, &temp_rx_buff, &temp_rx_bits, RFAL_FWT_NONE, flags);
    } else {
        ret = rfalNfcDataExchangeCustomStart(
            tx_rx->tx_data, tx_rx->tx_bits, &temp_rx_buff, &temp_rx_bits, RFAL_FWT_NONE, flags);
    }
    if(ret != ERR_NONE) {
        FURI_LOG_E(TAG, "Failed to start data exchange");
        return false;
    }

    if(tx_rx->sniff_tx) {
        bool crc_dropped = !(flags & RFAL_TXRX_FLAGS_CRC_TX_MANUAL);
        tx_rx->sniff_tx(tx_rx->tx_data, tx_rx->tx_bits, crc_dropped, tx_rx->sniff_context);
    }

    uint32_t start = DWT->CYCCNT;
    while(state != RFAL_NFC_STATE_DATAEXCHANGE_DONE) {
        rfalNfcWorker();
        state = rfalNfcGetState();
        ret = rfalNfcDataExchangeGetStatus();
        if(ret == ERR_BUSY) {
            if(DWT->CYCCNT - start > timeout_ms * clocks_in_ms) {
                FURI_LOG_D(TAG, "Timeout during data exchange");
                return false;
            }
            continue;
        } else {
            start = DWT->CYCCNT;
        }
        osDelay(1);
    }

    if(tx_rx->tx_rx_type == FuriHalNfcTxRxTypeRaw ||
       tx_rx->tx_rx_type == FuriHalNfcTxRxTypeRxRaw) {
        tx_rx->rx_bits = 8 * furi_hal_nfc_bitstream_to_data_and_parity(
                                 temp_rx_buff, *temp_rx_bits, tx_rx->rx_data, tx_rx->rx_parity);
    } else {
        memcpy(tx_rx->rx_data, temp_rx_buff, MIN(*temp_rx_bits / 8, FURI_HAL_NFC_DATA_BUFF_SIZE));
        tx_rx->rx_bits = *temp_rx_bits;
    }

    if(tx_rx->sniff_rx) {
        bool crc_dropped = !(flags & RFAL_TXRX_FLAGS_CRC_RX_KEEP);
        tx_rx->sniff_rx(tx_rx->rx_data, tx_rx->rx_bits, crc_dropped, tx_rx->sniff_context);
    }

    return true;
}

bool furi_hal_nfc_tx_rx_full(FuriHalNfcTxRxContext* tx_rx) {
    uint16_t part_len_bytes;

    if(!furi_hal_nfc_tx_rx(tx_rx, 1000)) {
        return false;
    }
    while(tx_rx->rx_bits && tx_rx->rx_data[0] == 0xAF) {
        FuriHalNfcTxRxContext tmp = *tx_rx;
        tmp.tx_data[0] = 0xAF;
        tmp.tx_bits = 8;
        if(!furi_hal_nfc_tx_rx(&tmp, 1000)) {
            return false;
        }
        part_len_bytes = tmp.rx_bits / 8;
        if(part_len_bytes > FURI_HAL_NFC_DATA_BUFF_SIZE - tx_rx->rx_bits / 8) {
            FURI_LOG_W(TAG, "Overrun rx buf");
            return false;
        }
        if(part_len_bytes == 0) {
            FURI_LOG_W(TAG, "Empty 0xAF response");
            return false;
        }
        memcpy(tx_rx->rx_data + tx_rx->rx_bits / 8, tmp.rx_data + 1, part_len_bytes - 1);
        tx_rx->rx_data[0] = tmp.rx_data[0];
        tx_rx->rx_bits += 8 * (part_len_bytes - 1);
    }

    return true;
}

void furi_hal_nfc_sleep() {
    rfalNfcDeactivate(false);
    rfalLowPowerModeStart();
}
