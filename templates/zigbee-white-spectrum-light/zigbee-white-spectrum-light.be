class zb_light
static KO_SWITCH = 1
static KO_DIM = 2
static KO_VALUE = 3
static KO_SWITCH_STATUS = 4
static KO_VALUE_STATUS = 5
static KO_IDENTIFY = 6
static KO_AVAILABLE = 11
static EP = 1
static BRI_MAX = 254
static POWER_ON = ['off', 'on', 'toggle', 'previous']
static START = ['minimum', 'previous']
static F_LEVEL_START = 1
static F_IDENTIFY = 2
static F_CT_START = 4
static F_FREEZE = 8
static MODELS = {'NYMANE PENDANT': [1, 250, 454, 15], 'KAJPLATS E27 WS globe 470lm': [2, 153, 454, 7], 'KAJPLATS E27 WS globe 1055lm': [2, 153, 454, 7], 'KAJPLATS GU10 WS 575lm': [2, 153, 454, 7], 'KAJPLATS E27 WS globe 1521lm': [2, 153, 454, 7], 'KAJPLATS E14 WS globe 806lm': [2, 153, 454, 7], 'KAJPLATS E26 WS globe 450lm': [2, 153, 454, 7], 'KAJPLATS E26 WS globe 1100lm': [2, 153, 454, 7], 'KAJPLATS E26 WS globe 1600lm': [2, 153, 454, 7], 'KAJPLATS E12 WS globe 800lm': [2, 153, 454, 7], 'KAJPLATS E14 WS B38 CL 470lm': [2, 153, 454, 7], 'KAJPLATS E27 WS G95 clear 806lm': [2, 153, 454, 7], 'KAJPLATS E27 806lm 95mm smart WS': [2, 153, 454, 7], 'KAJPLATS E27 WS G60 clear 470lm': [2, 153, 454, 7], 'KAJPLATS E27 470lm smart WS': [2, 153, 454, 7], 'KAJPLATS E12 WS B38 CL 450lm': [2, 153, 454, 7], 'FLOALT panel WS 30x30': [2, 250, 454, 15], 'FLOALT panel WS 30x90': [2, 250, 454, 15], 'FLOALT panel WS 60x60': [2, 250, 454, 15], 'JORMLIEN door WS 40x80': [1, 250, 454, 15], 'SURTE door WS 38x64': [1, 250, 454, 15], 'JETSTROM 6060': [1, 250, 454, 15], 'JETSTROM 6060 JP': [1, 250, 454, 15], 'JETSTROM 6060 NA': [1, 250, 454, 15], 'JETSTROM 40100': [1, 250, 454, 15], 'JETSTROM 40100 NA': [1, 250, 454, 15], 'TRADFRI bulb E14 WS opal 400lm': [1, 250, 454, 15], 'TRADFRI bulb E12 WS opal 400lm': [1, 250, 454, 15], 'TRADFRI bulb GU10 WS 400lm': [1, 250, 454, 15], 'TRADFRI bulb E27 WS opal 980lm': [1, 250, 454, 15], 'TRADFRI bulb E26 WS opal 980lm': [1, 250, 454, 15], 'TRADFRI bulb E27 WS�opal 980lm': [1, 250, 454, 15], 'TRADFRI bulb E27 WS clear 950lm': [1, 250, 454, 15], 'TRADFRI bulb E26 WS clear 950lm': [1, 250, 454, 15], 'TRADFRI bulb E27 WS�clear 950lm': [1, 250, 454, 15], 'TRADFRI bulb E27 WS opal 1000lm': [1, 250, 454, 15], 'TRADFRI bulb E26 WS opal 1000lm': [1, 250, 454, 15], 'TRADFRI bulb E14 WS opal 600lm': [1, 250, 454, 15], 'TRADFRI bulb E27 WS clear 806lm': [1, 250, 454, 15], 'TRADFRI bulb E26 WS clear 806lm': [1, 250, 454, 15], 'TRADFRI bulb E12 WS opal 600lm': [1, 250, 454, 15], 'TRADFRI bulb E17 WS opal 600lm': [1, 250, 454, 15], 'TRADFRI bulb E14 WS 470lm': [1, 250, 454, 15], 'TRADFRI bulb E12 WS 450lm': [1, 250, 454, 15], 'TRADFRI bulb E17 WS 440lm': [1, 250, 454, 15], 'TRADFRI bulb E17 WS candle 440lm': [1, 250, 454, 15], 'TRADFRIbulbG125E27WSopal470lm': [1, 250, 454, 15], 'TRADFRIbulbG125E26WSopal450lm': [1, 250, 454, 15], 'TRADFRIbulbG125E26WSopal470lm': [1, 250, 454, 15], 'TRADFRIbulbG125E26WSopal440lm': [1, 250, 454, 15], 'TRADFRI bulb E27 opal 470lm': [1, 250, 454, 15], 'TRADFRI bulb E27 W opal 470lm': [1, 250, 454, 15], 'TRADFRIbulbT120E27WSopal470lm': [1, 250, 454, 15], 'TRADFRIbulbT120E26WSopal450lm': [1, 250, 454, 15], 'TRADFRIbulbT120E26WSopal470lm': [1, 250, 454, 15], 'TRADFRI bulb E26 WS opal 440lm': [1, 250, 454, 15], 'TRADFRIbulbT120E26WSopal440lm': [1, 250, 454, 15], 'TRADFRIbulbE14WScandleopal470lm': [1, 250, 454, 15], 'TRADFRIbulbE12WScandleopal450lm': [1, 250, 454, 15], 'TRADFRIbulbE17WScandleopal440lm': [1, 250, 454, 15], 'TRADFRIbulbE14WSglobeopal470lm': [1, 250, 454, 15], 'TRADFRIbulbE12WSglobeopal470lm': [1, 250, 454, 15], 'TRADFRI bulb E17 WS globe 440lm': [1, 250, 454, 15], 'TRADFRIbulbE17WSglobeopal470lm': [1, 250, 454, 15], 'TRADFRIbulbE27WSglobeopal1055lm': [1, 250, 454, 15], 'TRADFRIbulbE26WSglobeopal1100lm': [1, 250, 454, 15], 'TRADFRIbulbE26WSglobeopal1160lm': [1, 250, 454, 15], 'TRADFRIbulbE26WSglobeopal1055lm': [1, 250, 454, 15], 'TRADFRIbulbE26WSglobeclear800lm': [1, 250, 454, 15], 'TRADFRIbulbE27WSglobeclear806lm': [1, 250, 454, 15], 'TRADFRIbulbE26WSglobeclear806lm': [1, 250, 454, 15], 'TRADFRIbulbE26WSglobeclear810lm': [1, 250, 454, 15], 'TRADFRI_bulb_GU10_WS_345lm': [1, 250, 454, 15], 'TRADFRIbulbGU10WS345lm': [1, 250, 454, 15], 'TRADFRI bulb GU10 WS 345lm': [1, 250, 454, 15], 'TRADFRIbulbGU10WS380lm': [1, 250, 454, 15], 'TRADFRI bulb GU10 WS 380lm': [1, 250, 454, 15], 'TRADFRIbulbPAR38WS900lm': [1, 250, 454, 15], 'TRADFRIbulbB22WSglobeopal1055lm': [1, 250, 454, 15], 'TRADFRI bulb E14 WS globe 470lm': [1, 250, 454, 15], 'TRADFRI bulb E12 WS globe 450lm': [1, 250, 454, 15], 'TRADFRI bulb E14 WS candle 470lm': [1, 250, 454, 15], 'TRADFRI bulb E12 WS candle 450lm': [1, 250, 454, 15], 'TRADFRI bulb E27 WS globe 1055lm': [1, 250, 454, 15], 'TRADFRI bulb E26 WS globe 1055lm': [1, 250, 454, 15], 'TRADFRI bulb E26 WS globe 1100lm': [1, 250, 454, 15], 'TRADFRI bulb E26 WS globe 1160lm': [1, 250, 454, 15], 'LEPTITER Recessed spot light': [1, 250, 454, 15], 'GUNNARP panel round': [1, 250, 454, 15], 'GUNNARP panel 40*40': [1, 250, 454, 15], 'TRADFRI Light Engine': [1, 250, 454, 15]}
static KO_CT = 7
static KO_CT_REL = 8
static KO_CT_STATUS = 9
static KO_CT_RANGE = 10
static MIRED_DEFAULT_MIN = 153
static MIRED_DEFAULT_MAX = 500
static CT_PREVIOUS = 65535
static P_POB = 0
static P_START = 1
static P_STARTVAL = 2
static P_OSV = 3
static P_MF = 4
static P_MIN = 5
static P_MAX = 6
static P_ROE = 7
static P_TRANS = 8
static P_RATE = 9
static P_CTMIN = 10
static P_CTMAX = 12
static P_CTSOB = 14
static P_CTSOSV = 15
static P_CTRATE = 17
static P_CTFADE = 18
static P_CTSTART = 19
static P_CTSTARTVAL = 20
static def defaults()
return {'osv': 100, 'mf': true, 'min': 1, 'max': 100, 'roe': false, 'trans': 0, 'rate': 60, 'ctmin': 1000, 'ctmax': 10000, 'ctsob': 0, 'ctsosv': 2700, 'ctrate': 55, 'ctfade': false}
end
static def cfg()
var c = host.state_get('cfg')
return c == nil ? zb_light.defaults() : c
end
static def model()
var zm = host.dp_get(0, 'zigbeeModel')
var e = zm != nil ? zb_light.MODELS.find(zm) : nil
return e != nil ? e : [1, 0, 0, 7]
end
static def has(flag)
return (zb_light.model()[3] & flag) != 0
end
static def clamp(v, lo, hi)
if v < lo return lo end
if v > hi return hi end
return v
end
static def pct_to_bri(pct)
var Z = zb_light
return Z.clamp((pct * Z.BRI_MAX + 50) / 100, Z.model()[0], Z.BRI_MAX)
end
static def bri_byte(bri)
var b = bytes()
b.add((bri * 255 + zb_light.BRI_MAX / 2) / zb_light.BRI_MAX, 1)
return b
end
static def transition(c)
if c['trans'] > 0
host.dp_set(zb_light.EP, 'transition', real(c['trans']) / 10)
end
end
static def set_bri(c, bri)
var Z = zb_light
var lo = Z.pct_to_bri(c['min'])
var hi = Z.pct_to_bri(c['max'])
Z.transition(c)
host.dp_set(Z.EP, 'brightness', Z.clamp(bri, lo, hi))
end
static def off(c)
zb_light.transition(c)
host.dp_set(zb_light.EP, 'state', 'OFF')
end
static def k_word(p, o)
return (p[o] << 8) | p[o + 1]
end
static def mired_to_k(m)
return (1000000 + m / 2) / m
end
static def ct_range()
var Z = zb_light
var lo = host.dp_get(Z.EP, 'colorTempPhysicalMin')
var hi = host.dp_get(Z.EP, 'colorTempPhysicalMax')
if lo != nil && hi != nil && lo > 0 && hi < 0xFEFF && lo < hi return [lo, hi] end
var e = Z.model()
if e[1] > 0 return [e[1], e[2]] end
return [Z.MIRED_DEFAULT_MIN, Z.MIRED_DEFAULT_MAX]
end
static def k_bytes(k)
var b = bytes()
b.add((k >> 8) & 0xFF, 1)
b.add(k & 0xFF, 1)
return b
end
static def set_ct(c, k)
var Z = zb_light
var r = Z.ct_range()
var lo = Z.clamp(c['ctmin'], Z.mired_to_k(r[1]), Z.mired_to_k(r[0]))
var hi = Z.clamp(c['ctmax'], Z.mired_to_k(r[1]), Z.mired_to_k(r[0]))
if lo > hi lo = hi end
var kk = Z.clamp(k, lo, hi)
host.ko_status(Z.KO_CT_RANGE, kk != k ? bytes('01') : bytes('00'), false)
if c['ctfade'] && !Z.has(Z.F_FREEZE) Z.transition(c) end
host.dp_set(Z.EP, 'color_temp', Z.clamp((1000000 + kk / 2) / kk, r[0], r[1]))
end
end
def knx_write(ko, raw)
var Z = zb_light
var c = Z.cfg()
if ko == Z.KO_SWITCH && raw.size() == 1
if (raw[0] & 1) != 0
var bri = Z.pct_to_bri(c['osv'])
var last = host.dp_get(Z.EP, 'brightness')
if c['mf'] && last != nil && last > 0 bri = last end
if c['ctsob'] == 1
Z.set_ct(c, c['ctsosv'])
elif c['ctsob'] == 2 && host.state_get('ct_abs') != nil
Z.set_ct(c, host.state_get('ct_abs'))
end
Z.set_bri(c, bri)
else
Z.off(c)
end
elif ko == Z.KO_VALUE && raw.size() == 1
if raw[0] == 0
Z.off(c)
else
Z.set_bri(c, Z.clamp((raw[0] * Z.BRI_MAX + 127) / 255, Z.model()[0], Z.BRI_MAX))
end
elif ko == Z.KO_DIM && raw.size() == 1
var key = c['roe'] ? 'brightness_move_onoff' : 'brightness_move'
if (raw[0] & 0x07) == 0
host.dp_set(Z.EP, key, 'stop')
elif (raw[0] & 0x08) != 0
host.dp_set(Z.EP, key, c['rate'])
else
host.dp_set(Z.EP, key, -c['rate'])
end
elif ko == Z.KO_IDENTIFY && raw.size() == 1
if (raw[0] & 1) != 0 && Z.has(Z.F_IDENTIFY) host.dp_set(Z.EP, 'identify', 'identify') end
elif ko == Z.KO_CT && raw.size() == 2
var k = (raw[0] << 8) | raw[1]
if k > 0
host.state_set('ct_abs', k)
Z.set_ct(c, k)
end
elif ko == Z.KO_CT_REL && raw.size() == 1
if (raw[0] & 0x07) == 0
host.dp_set(Z.EP, 'color_temp_move', 'stop')
elif (raw[0] & 0x08) != 0
host.dp_set(Z.EP, 'color_temp_move', -c['ctrate'])
else
host.dp_set(Z.EP, 'color_temp_move', c['ctrate'])
end
else
host.log('ko_write_ignored', {'ko': ko, 'value': raw.tohex()})
end
end
def dp_event(ch, key, value, initial)
var Z = zb_light
if ch == 0
if key == 'availability'
host.ko_status(Z.KO_AVAILABLE, value == true ? bytes('01') : bytes('00'), initial)
end
return
end
if ch != Z.EP return end
if key == 'state'
var on = value == 'ON'
host.ko_status(Z.KO_SWITCH_STATUS, on ? bytes('01') : bytes('00'), initial)
var b = host.dp_get(Z.EP, 'brightness')
if !on
host.ko_status(Z.KO_VALUE_STATUS, bytes('00'), initial)
elif b != nil
host.ko_status(Z.KO_VALUE_STATUS, Z.bri_byte(b), initial)
end
elif key == 'brightness'
if host.dp_get(Z.EP, 'state') != 'OFF'
host.ko_status(Z.KO_VALUE_STATUS, Z.bri_byte(value), initial)
end
elif key == 'color_temp'
if value != nil && value > 0
host.ko_status(Z.KO_CT_STATUS, Z.k_bytes(Z.mired_to_k(value)), initial)
end
end
end
def params(p)
var Z = zb_light
if p.size() < 22 return end
var c = {'osv': Z.clamp(p[Z.P_OSV], 1, 100), 'mf': p[Z.P_MF] != 0, 'min': Z.clamp(p[Z.P_MIN], 1, 100),
'max': Z.clamp(p[Z.P_MAX], 1, 100), 'roe': p[Z.P_ROE] != 0, 'trans': p[Z.P_TRANS],
'rate': Z.clamp(p[Z.P_RATE], 1, 255)}
if c['min'] > c['max'] c['min'] = c['max'] end
c['ctmin'] = Z.clamp(Z.k_word(p, Z.P_CTMIN), 1000, 10000)
c['ctmax'] = Z.clamp(Z.k_word(p, Z.P_CTMAX), 1000, 10000)
if c['ctmin'] > c['ctmax'] c['ctmin'] = c['ctmax'] end
c['ctsob'] = Z.clamp(p[Z.P_CTSOB], 0, 2)
c['ctsosv'] = Z.clamp(Z.k_word(p, Z.P_CTSOSV), 1000, 10000)
c['ctrate'] = Z.clamp(p[Z.P_CTRATE], 1, 255)
c['ctfade'] = p[Z.P_CTFADE] != 0
host.state_set('cfg', c)
host.dp_set(Z.EP, 'power_on_behavior', Z.POWER_ON[Z.clamp(p[Z.P_POB], 0, 3)])
if Z.has(Z.F_LEVEL_START)
if p[Z.P_START] < Z.START.size()
host.dp_set(Z.EP, 'level_config.current_level_startup', Z.START[p[Z.P_START]])
else
host.dp_set(Z.EP, 'level_config.current_level_startup', Z.pct_to_bri(Z.clamp(p[Z.P_STARTVAL], 1, 100)))
end
end
if Z.has(Z.F_CT_START)
if p[Z.P_CTSTART] == 0
host.dp_set(Z.EP, 'color_temp_startup', Z.CT_PREVIOUS)
else
var r = Z.ct_range()
var k = Z.clamp(Z.k_word(p, Z.P_CTSTARTVAL), Z.mired_to_k(r[1]), Z.mired_to_k(r[0]))
host.dp_set(Z.EP, 'color_temp_startup', Z.clamp((1000000 + k / 2) / k, r[0], r[1]))
end
end
host.log('zb_params', c)
end
