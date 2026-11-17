# Devices

The products in the files of `producer/`, one table per class. Every class also has a generic product ("Zigbee dimmable light" and so on) for models it does not list; such a model runs with the class defaults, and the gateway reports it with the message `MODEL` on its object "Last message".

The lists come from the Zigbee2MQTT device definitions (zigbee-herdsman-converters 26.57.0). Only the devices named in [Status](../README.md#status) have run on a gateway; the others are listed because their definitions fall into the class.

## Dimmable light (0xC001)

23 models.

| Model | Description |
|---|---|
| G2015 | PILSKOTT floor lamp, warm white |
| ICPSHC24-10EU-IL-1/ICPSHC24-10EU-IL-2 | TRADFRI LED driver, 10 w |
| ICPSHC24-30-IL44-1 | SILVERGLANS LED driver, 30 w, IP44 |
| ICPSHC24-30EU-IL-1/ICPSHC24-10EU-IL-2 | TRADFRI LED driver, 30 w |
| LED1622G12 | TRADFRI bulb E26, white, globe, opal, 1000 lm |
| LED1623G12 | TRADFRI bulb E27, white, globe, opal, 1000 lm |
| LED1649C5 | TRADFRI bulb E12/E14/E17, white, candle, opal, 400 lm |
| LED1650R5 | TRADFRI bulb GU10, white, 400 lm |
| LED1836G9 | TRADFRI bulb E26/E27, warm white, globe, opal, 806 lm |
| LED1837R5 | TRADFRI bulb GU10, warm white, 400 lm |
| LED1842G3 | TRADFRI bulb E26/E27, warm white, globe, clear, 250 lm |
| LED1934G3 | TRADFRI bulb E26/E27, warm white, globe, clear, 250 lm |
| LED1935C3 | TRADFRI bulb E12/E14/E17, warm white, candle, clear, 250 lm |
| LED2008G3 | TRADFRI bulb E26, warm white, globe, clear, 250 lm |
| LED2009C3 | TRADFRI bulb E12, warm white, candle, clear, 250 lm |
| LED2102G3 | TRADFRI bulb E26/E27, warm white, globe, clear, 440/450/470 lm |
| LED2103G5 | TRADFRI bulb E26/E27, warm white, globe, 806/810 lumen |
| LED2104R3 | TRADFRI bulb GU10, warm white, 345/380 lm |
| T2030 | PILSKOTT pendant lamp, warm white |
| T2035 | STOFTMOLN ceiling/wall lamp, warm white, 24 cm |
| T2037 | STOFTMOLN ceiling/wall lamp, warm white, 37 cm |
| T2105 | STOFTMOLN ceiling/wall lamp, warm white, 10 cm |
| T2106 | STOFTMOLN ceiling/wall lamp, warm white, 15 cm |

## White spectrum light (0xC002)

48 models.

| Model | Description |
|---|---|
| 90504044 | NYMANE pendant lamp |
| KAJPLATS_WS | KAJPLATS bulb, white spectrum (Matter) |
| LED2406G4 | KAJPLATS bulb, white spectrum (Matter) |
| LED2407G8 | KAJPLATS bulb, white spectrum (Matter) |
| LED2403R5/LED2403R5NA | KAJPLATS bulb, white spectrum (Matter) |
| LED2408G10 | KAJPLATS bulb, white spectrum (Matter) |
| LED2404G6 | KAJPLATS bulb, white spectrum (Matter) |
| LED2406G4NA | KAJPLATS bulb, white spectrum (Matter) |
| LED2407G8NA | KAJPLATS bulb, white spectrum (Matter) |
| LED2408G10NA | KAJPLATS bulb, white spectrum (Matter) |
| LED2404G6NA | KAJPLATS bulb, white spectrum (Matter) |
| KAJPLATS_WS_clear | KAJPLATS bulb, white spectrum, clear (Matter) |
| LED2402C3 | KAJPLATS bulb, white spectrum, clear (Matter) |
| LED2401G5 | KAJPLATS bulb, white spectrum, clear (Matter) |
| LED2411G3 | KAJPLATS bulb, white spectrum, clear (Matter) |
| LED2402C3NA | KAJPLATS bulb, white spectrum, clear (Matter) |
| L1527 | FLOALT light panel, white spectrum, 30x30 cm |
| L1528 | FLOALT light panel, white spectrum, 30x90 cm |
| L1529 | FLOALT light panel, white spectrum, 60x60 cm |
| L1530 | JORMLIEN door light panel, white spectrum, 40x80 cm |
| L1531 | SURTE door light panel, white spectrum, 38x64 cm |
| L2207 | JETSTROM ceiling light panel, white spectrum, 60x60 cm |
| L2208 | JETSTROM ceiling light panel, white spectrum, 100x40 cm |
| LED1536G5 | TRADFRI bulb E12/E14, white spectrum, globe, opal, 400 lm |
| LED1537R6/LED1739R5 | TRADFRI bulb GU10, white spectrum, 400 lm |
| LED1545G12 | TRADFRI bulb E26/E27, white spectrum, globe, opal, 980 lm |
| LED1546G12 | TRADFRI bulb E26/E27, white spectrum, globe, clear, 950 lm |
| LED1732G11 | TRADFRI bulb E26/E27, white spectrum, globe, opal, 1000 lm |
| LED1733G7 | TRADFRI bulb E14, white spectrum, globe, opal, 600 lm |
| LED1736G9 | TRADFRI bulb E26/E27, white spectrum, globe, clear, 806 lm |
| LED1738G7 | TRADFRI bulb E12/E17, white spectrum, globe, opal, 600 lm |
| LED1835C6 | TRADFRI bulb E12/E14/E17, white spectrum, candle, opal, 450/470/440 lm |
| LED1936G5 | TRADFRI bulb E26/E27, white spectrum, globe, opal, 440/450/470 lm |
| LED1937T5 | TRADFRI bulb E26/E27, white spectrum, T120 cylinder, opal, 440/450/470 lm |
| LED1949C5 | TRADFRI bulb E12/E14/E17, white spectrum, candle, opal, 450/470/440 lm |
| LED2002G5 | TRADFRI bulb E14/E12/E17, white spectrum, globe, clear, 440/470 lm |
| LED2003G10 | TRADFRI bulb E26/27, white spectrum, globe, opal, 1055/1100/1160 lm |
| LED2004G8 | TRADFRI bulb E26/E27, white spectrum, globe, clear, 800/806/810 lm |
| LED2005R5/LED2106R3 | TRADFRI bulb GU10, white spectrum, 345/380 lm |
| LED2006R9 | TRADFRI bulb E26, white spectrum, PAR38 downlight, clear, 900 lm |
| LED2035G10 | TRADFRI bulb B22, white spectrum, globe, opal, 1055 lm |
| LED2101G4 | TRADFRI bulb E12/E14, white spectrum, globe, opal, 450/470 lm |
| LED2107C4 | TRADFRI bulb E14, white spectrum, candle, opal, 470 lm |
| LED2201G8 | TRADFRI bulb E26/27, white spectrum, globe, opal, 1055/1100/1160 lm |
| T1820 | LEPTITER recessed spot light, white spectrum |
| T1828 | GUNNARP light panel, round |
| T1829 | GUNNARP light panel, 40x40 cm |
| T2011 | OSVALLA panel round |

## Switch and plug (0xC003)

14 models.

| Model | Description |
|---|---|
| E160x/E170x/E190x | TRADFRI control outlet |
| E1603 | TRADFRI control outlet |
| E1702/E1703 | TRADFRI control outlet |
| E1704 | TRADFRI control outlet |
| E1705 | TRADFRI control outlet |
| E1706/E1707/E1708/E1709/E1710 | TRADFRI control outlet |
| E1902 | TRADFRI control outlet |
| E1836 | ASKVADER on/off switch |
| E22x4 | TRETAKT smart plug |
| E2204 | TRETAKT smart plug |
| E2214 | TRETAKT smart plug |
| E2435/E2491 | TOFSMYGGA/GRILLPLATS smart plug (Matter) |
| E2435 | TOFSMYGGA/GRILLPLATS smart plug (Matter) |
| E2491 | TOFSMYGGA/GRILLPLATS smart plug (Matter) |

## Remote (experimental) (0xC004)

6 models.

| Model | Description |
|---|---|
| E1743 | TRADFRI on/off switch |
| E1744 | SYMFONISK sound remote, gen 1 |
| E1812 | TRADFRI shortcut button |
| E1841 | KNYCKLAN open/close water valve remote |
| E2201 | RODRET wireless dimmer/power switch |
| ICTC-G-1 | TRADFRI wireless dimmer |
