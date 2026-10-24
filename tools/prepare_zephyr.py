#!/usr/bin/env python3
"""Apply the small CC26x2 custom-L2 fix to the pinned Zephyr revision."""
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
path = root / "drivers/ieee802154/ieee802154_cc13xx_cc26xx.c"
old = "#define MTU 1280\n#endif"
new = """#define MTU 1280
#elif defined(CONFIG_NET_L2_CUSTOM_IEEE802154)
#define L2 CUSTOM_IEEE802154_L2
#define L2_CTX_TYPE NET_L2_GET_CTX_TYPE(CUSTOM_IEEE802154_L2)
#define MTU CONFIG_NET_L2_CUSTOM_IEEE802154_MTU
#endif"""
source = path.read_text()
if new in source:
    print("CC26x2 custom-L2 patch already applied")
elif source.count(old) == 1:
    path.write_text(source.replace(old, new))
    print("Applied CC26x2 custom-L2 patch")
else:
    raise SystemExit("Unexpected driver version; refusing to patch")

# ZBDongle-P uses the CC2652P 2.4 GHz high PA and a DIO29 RF switch.
# TI SmartRF IEEE 802.15.4 PA settings (also shipped in Z-Stack's
# txPowerTable_2400_pa5_20). Restrict this change to our board.
path = root / "drivers/ieee802154/ieee802154_cc13xx_cc26xx.c"
source = path.read_text()
if "zigmodem_pa_std" not in source:
    old = "/* 2.4 GHz power table */"
    new = '''#if defined(CONFIG_BOARD_ZBDONGLE_P)
static uint32_t zigmodem_pa_std[] = {
    TX_STD_POWER_OVERRIDE(0x7217), 0x05320703, 0x00008783, 0x050206C3,
    HW_REG_OVERRIDE(0x60A8,0x0401), 0xFFFFFFFF
};
static uint32_t zigmodem_pa_high[] = {
    TX20_POWER_OVERRIDE(0x003F75F5), 0x01C20703, 0x00038783, 0x010206C3,
    HW_REG_OVERRIDE(0x60A8,0x0001), 0xFFFFFFFF
};
#endif
/* 2.4 GHz power table */'''
    if source.count(old) != 1:
        raise SystemExit("Unexpected TI PA table")
    source = source.replace(old, new)
    source = source.replace("\tRF_TxPowerTable_TERMINATION_ENTRY,", '''#if defined(CONFIG_BOARD_ZBDONGLE_P)
    {14, RF_TxPowerTable_HIGH_PA_ENTRY(22, 3, 1, 19, 27)},
    {17, RF_TxPowerTable_HIGH_PA_ENTRY(37, 3, 1, 39, 27)},
    {20, RF_TxPowerTable_HIGH_PA_ENTRY(53, 3, 1, 58, 63)},
#endif
\tRF_TxPowerTable_TERMINATION_ENTRY,''', 1)
    old = "\t\t.pRegOverride = overrides"
    new = '''#if defined(CONFIG_BOARD_ZBDONGLE_P)
        .pRegOverrideTxStd = zigmodem_pa_std,
        .pRegOverrideTx20 = zigmodem_pa_high,
#endif
\t\t.pRegOverride = overrides'''
    if source.count(old) != 1:
        raise SystemExit("Unexpected TI radio setup")
    source = source.replace(old, new)
    path.write_text(source)

# TI energy detection stops the background receiver. Restore it with a forced
# same-channel restart; the public start() hook alone does not restart RX.
ti_radio = root / "drivers/ieee802154/ieee802154_cc13xx_cc26xx.c"
ti_source = ti_radio.read_text()
if 'int zigmodem_ti_resume_rx(' not in ti_source:
    ti_source += '''
int zigmodem_ti_resume_rx(const struct device *dev)
{
    return ieee802154_cc13xx_cc26xx_reset_channel(dev);
}
'''
    ti_radio.write_text(ti_source)

old = "\tieee802154_init(iface);"
new = "#if !defined(CONFIG_NET_L2_CUSTOM_IEEE802154)\n\tieee802154_init(iface);\n#endif"
source = path.read_text()
if new not in source:
    if source.count(old) != 1:
        raise SystemExit("Unexpected TI interface initializer")
    path.write_text(source.replace(old, new))

path = root / "drivers/ieee802154/ieee802154_silabs_efr32.c"
source = path.read_text()
old = '#include <zephyr/net/openthread.h>\n#include <zephyr/net/ieee802154_radio_openthread.h>'
new = '#if defined(CONFIG_NET_L2_OPENTHREAD)\n' + old + '\n#endif'
if new not in source:
    if source.count(old) != 1:
        raise SystemExit("Unexpected Silicon Labs OpenThread includes")
    path.write_text(source.replace(old, new))
print("Radio drivers prepared for custom L2")

source = path.read_text()
marker = "/* Zigmodem: PHY constants without an OpenThread stack. */"
if marker not in source:
    source = source.replace('#include "ieee802154_silabs_efr32.h"', '''#include "ieee802154_silabs_efr32.h"
/* Zigmodem: PHY constants without an OpenThread stack. */
#if !defined(CONFIG_NET_L2_OPENTHREAD)
#define OT_MAC_KEY_SIZE 16
#define OT_US_PER_TEN_SYMBOLS 160
#define OT_RADIO_SYMBOL_TIME 16
#define OT_ACK_IE_MAX_SIZE 16
#define OT_RADIO_2P4GHZ_OQPSK_CHANNEL_MIN 11
#define OT_RADIO_2P4GHZ_OQPSK_CHANNEL_MAX 26
#endif''')
    for setter, field in [('seb','seb'), ('fc','fc'), ('keyid','key_id')]:
        call = f'\tnet_pkt_set_ieee802154_ack_{setter}(pkt, data->mac_data.ack_{field});'
        if source.count(call) != 1:
            raise SystemExit(f"Unexpected ACK metadata setter: {setter}")
        source = source.replace(call, '#if defined(CONFIG_NET_L2_OPENTHREAD)\n' + call + '\n#endif')
    # net_if_set_link_addr retains this buffer; it cannot live on the stack.
    source = source.replace('uint8_t eui64[SL_802154_EUI64_BYTES] = {0};',
                            'static uint8_t eui64[SL_802154_EUI64_BYTES];')
    path.write_text(source)

source = path.read_text()
old = "\tieee802154_init(iface);"
new = "#if !defined(CONFIG_NET_L2_CUSTOM_IEEE802154)\n\tieee802154_init(iface);\n#endif"
if new not in source:
    if source.count(old) != 1:
        raise SystemExit("Unexpected Silicon Labs interface initializer")
    source = source.replace(old, new)
old = "\tif (ieee802154_handle_ack(data->iface, ack_pkt) != NET_OK) {"
new = "\tif (IS_ENABLED(CONFIG_NET_L2_CUSTOM_IEEE802154)) {\n\t\t/* Custom L2 uses its own acknowledgments. */\n\t} else if (ieee802154_handle_ack(data->iface, ack_pkt) != NET_OK) {"
if new not in source:
    if source.count(old) != 1:
        raise SystemExit("Unexpected Silicon Labs ACK handler")
    source = source.replace(old, new)
path.write_text(source)

# SONOFF's fitted crystal uses a different load trim than TI LaunchPads.
# This value is read from the original ZBDongle-P CCFG MODE_CONF word.
path = root / "soc/ti/simplelink/cc13x2_cc26x2/ccfg.c"
source = path.read_text()
old = "#define SET_CCFG_MODE_CONF_XOSC_CAPARRAY_DELTA 0xD5"
new = """#if defined(CONFIG_BOARD_ZBDONGLE_P)
#define SET_CCFG_MODE_CONF_XOSC_CAPARRAY_DELTA 0xC1
#else
#define SET_CCFG_MODE_CONF_XOSC_CAPARRAY_DELTA 0xD5
#endif"""
if new not in source:
    if source.count(old) != 1:
        raise SystemExit("Unexpected TI crystal trim definition")
    path.write_text(source.replace(old, new))

path = root / "drivers/ieee802154/ieee802154_cc13xx_cc26xx.c"
source = path.read_text()
old = "\t\tdrv_data->cmd_ieee_csma.status = IDLE;"
new = """\t\t/* CSMA command fields are in/out: start a fresh backoff for every TX. */
\t\tdrv_data->cmd_ieee_csma.NB = 0;
\t\tdrv_data->cmd_ieee_csma.BE = CONFIG_IEEE802154_CC13XX_CC26XX_RADIO_CSMA_CA_MIN_BE;
\t\tdrv_data->cmd_ieee_csma.remainingPeriods = 0;
\t\tdrv_data->cmd_ieee_csma.status = IDLE;"""
if new not in source:
    if source.count(old) != 1:
        raise SystemExit("Unexpected TI CSMA initialization")
    path.write_text(source.replace(old, new))

# Expose command status for diagnostics without logging on the modem UART.
path = root / "drivers/ieee802154/ieee802154_cc13xx_cc26xx.c"
source = path.read_text()
if 'uint32_t zigmodem_ti_status(' not in source:
    source += '''
uint32_t zigmodem_ti_status(const struct device *dev)
{
    struct ieee802154_cc13xx_cc26xx_data *d = dev->data;
    return ((uint32_t)d->cmd_ieee_csma.status << 16) | d->cmd_ieee_tx.status;
}
'''
    path.write_text(source)

# CCA mode 3 requires energy AND correlation, not OR. Match TI's OpenThread
# reference configuration; sync detection still always marks the channel busy.
source = path.read_text()
old = "\t\t\t.ccaCorrOp = 0,"
new = "\t\t\t.ccaCorrOp = 1, /* CCA mode 3: energy AND correlation */"
if new not in source:
    if source.count(old) != 1:
        raise SystemExit("Unexpected TI CCA correlation operator")
    path.write_text(source.replace(old, new))
