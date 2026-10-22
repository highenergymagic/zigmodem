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
