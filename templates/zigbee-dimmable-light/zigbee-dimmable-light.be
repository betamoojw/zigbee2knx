class zb_light
static KO_SWITCH = 1
static KO_DIM = 2
static KO_VALUE = 3
static KO_SWITCH_STATUS = 4
static KO_VALUE_STATUS = 5
static KO_IDENTIFY = 6
static KO_AVAILABLE = 7
static EP = 1
static BRI_MAX = 254
static SETTLE_MS = 1000
static POWER_ON = ['off', 'on', 'toggle', 'previous']
static START = ['minimum', 'previous']
static F_LEVEL_START = 1
static F_IDENTIFY = 2
static F_CT_START = 4
static F_FREEZE = 8
static MODELS = {'Floor lamp WW': [1, 0, 0, 3], 'TRADFRI transformer 10W': [2, 0, 0, 3], 'TRADFRI Driver 10W': [2, 0, 0, 3], 'SILVERGLANS IP44 LED driver': [2, 0, 0, 3], 'TRADFRI transformer 30W': [2, 0, 0, 3], 'TRADFRI Driver 30W': [2, 0, 0, 3], 'TRADFRI bulb E26 opal 1000lm': [1, 0, 0, 3], 'TRADFRI bulb E26 W opal 1000lm': [1, 0, 0, 3], 'TRADFRI bulb E27 opal 1000lm': [1, 0, 0, 3], 'TRADFRI bulb E27 W opal 1000lm': [1, 0, 0, 3], 'TRADFRI bulb E14 W op/ch 400lm': [1, 0, 0, 3], 'TRADFRI bulb E12 W op/ch 400lm': [1, 0, 0, 3], 'TRADFRI bulb E17 W op/ch 400lm': [1, 0, 0, 3], 'TRADFRI bulb GU10 W 400lm': [1, 0, 0, 3], 'TRADFRI bulb E27 WW 806lm': [2, 0, 0, 3], 'TRADFRI bulb E26 WW 806lm': [2, 0, 0, 3], 'TRADFRI bulb GU10 WW 400lm': [1, 0, 0, 3], 'TRADFRI bulb E27 WW clear 250lm': [1, 0, 0, 3], 'TRADFRI bulb E26 WW clear 250lm': [1, 0, 0, 3], 'TRADFRIbulbE27WWclear250lm': [2, 0, 0, 3], 'TRADFRIbulbE26WWclear250lm': [2, 0, 0, 3], 'TRADFRIbulbE14WWclear250lm': [1, 0, 0, 3], 'TRADFRIbulbE12WWclear250lm': [1, 0, 0, 3], 'TRADFRIbulbE17WWclear250lm': [1, 0, 0, 3], 'TRADFRIbulbE26WWglobeclear250lm': [1, 0, 0, 3], 'TRADFRIbulbE12WWcandleclear250lm': [1, 0, 0, 3], 'TRADFRI bulb E27 WW G95 CL 470lm': [1, 0, 0, 3], 'TRADFRI bulb E26 WW G95 CL 450lm': [1, 0, 0, 3], 'TRADFRI bulb E26 WW G95 CL 440lm': [1, 0, 0, 3], 'TRADFRI bulb E26 WW G95 CL 470lm': [1, 0, 0, 3], 'TRADFRI bulb E26 WW globe 800lm': [1, 0, 0, 3], 'TRADFRI bulb E26 WW globe 806lm': [1, 0, 0, 3], 'TRADFRI bulb E26 WW globe 810lm': [1, 0, 0, 3], 'TRADFRI bulb E27 WW globe 806lm': [1, 0, 0, 3], '\x1aTRADFRI bulb GU10 WW 345lm': [1, 0, 0, 3], 'TRADFRI bulb GU10 WW 345lm': [1, 0, 0, 3], '\\u001TRADFRI bulb GU10 WW 345lm': [1, 0, 0, 3], '\x1aTRADFRI bulb GU10 WW 345lm8': [1, 0, 0, 3], 'TRADFRI bulb GU10 WW 380lm': [1, 0, 0, 3], 'Pendant lamp WW': [2, 0, 0, 3], 'STOFTMOLN ceiling/wall lamp WW24': [1, 0, 0, 3], 'STOFTMOLN ceiling/wall lamp WW37': [1, 0, 0, 3], 'STOFTMOLN ceiling/wall lamp WW10': [1, 0, 0, 3], 'STOFTMOLN ceiling/wall lamp WW15': [1, 0, 0, 3]}
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
static def defaults()
return {'osv': 100, 'mf': true, 'min': 1, 'max': 100, 'roe': false, 'trans': 0, 'rate': 60}
end
static def cfg()
var c = host.state_get('cfg')
return c == nil ? zb_light.defaults() : c
end
static def model()
var zm = host.dp_get(0, 'zigbeeModel')
var e = zm != nil ? zb_light.MODELS.find(zm) : nil
return e != nil ? e : [1, 0, 0, 3]
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
static def level()
var Z = zb_light
var m = host.state_get('lv')
if m != nil && host.now_ms() < m[2] + m[3] + Z.SETTLE_MS return m[1] end
if host.dp_get(Z.EP, 'state') != 'ON' return 0 end
var b = host.dp_get(Z.EP, 'brightness')
return b == nil ? 0 : b
end
static def moving(key, at, to, tenths)
host.state_set(key, [at, to, host.now_ms(), tenths * 100])
end
static def stopped(key)
var m = host.state_get(key)
var now = host.now_ms()
if m != nil && now < m[2] + m[3]
var at = m[0] + (m[1] - m[0]) * (now - m[2]) / m[3]
host.state_set(key, [at, at, now, 0])
end
end
static def set_bri(c, bri)
var Z = zb_light
var lo = Z.pct_to_bri(c['min'])
var hi = Z.pct_to_bri(c['max'])
var to = Z.clamp(bri, lo, hi)
Z.moving('lv', Z.level(), to, c['trans'])
Z.transition(c)
host.dp_set(Z.EP, 'brightness', to)
end
static def off(c)
var Z = zb_light
Z.moving('lv', Z.level(), 0, c['trans'])
Z.transition(c)
host.dp_set(Z.EP, 'state', 'OFF')
end
static def step(c, raw)
var Z = zb_light
var code = raw & 0x07
if code == 0
Z.stopped('lv')
host.dp_set(Z.EP, c['roe'] ? 'brightness_move_onoff' : 'brightness_move', 'stop')
return
end
var lo = Z.pct_to_bri(c['min'])
var hi = Z.pct_to_bri(c['max'])
var d = (Z.BRI_MAX * 2 / (1 << (code - 1)) + 1) / 2
var at = Z.level()
var to = 0
if (raw & 0x08) != 0
if at == 0
host.dp_set(Z.EP, 'brightness', lo)
at = lo
end
to = Z.clamp(at + d, lo, hi)
elif at == 0
return
elif c['roe'] && at - d < lo
Z.moving('lv', at, 0, (at * 10 + c['rate'] / 2) / c['rate'])
host.dp_set(Z.EP, 'brightness_move_onoff', -c['rate'])
return
else
to = Z.clamp(at - d, lo, hi)
end
if to == at
Z.moving('lv', at, to, 0)
return
end
var t = ((to > at ? to - at : at - to) * 10 + c['rate'] / 2) / c['rate']
Z.moving('lv', at, to, t)
if t > 0 host.dp_set(Z.EP, 'transition', real(t) / 10) end
host.dp_set(Z.EP, 'brightness', to)
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
Z.step(c, raw[0])
elif ko == Z.KO_IDENTIFY && raw.size() == 1
if Z.has(Z.F_IDENTIFY) host.dp_set(Z.EP, 'identify', 'identify') end
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
end
end
def params(p)
var Z = zb_light
if p.size() < 10 return end
var c = {'osv': Z.clamp(p[Z.P_OSV], 1, 100), 'mf': p[Z.P_MF] != 0, 'min': Z.clamp(p[Z.P_MIN], 1, 100),
'max': Z.clamp(p[Z.P_MAX], 1, 100), 'roe': p[Z.P_ROE] != 0, 'trans': p[Z.P_TRANS],
'rate': Z.clamp(p[Z.P_RATE], 1, 255)}
if c['min'] > c['max'] c['min'] = c['max'] end
host.state_set('cfg', c)
host.dp_set(Z.EP, 'power_on_behavior', Z.POWER_ON[Z.clamp(p[Z.P_POB], 0, 3)])
if Z.has(Z.F_LEVEL_START)
if p[Z.P_START] < Z.START.size()
host.dp_set(Z.EP, 'level_config.current_level_startup', Z.START[p[Z.P_START]])
else
host.dp_set(Z.EP, 'level_config.current_level_startup', Z.pct_to_bri(Z.clamp(p[Z.P_STARTVAL], 1, 100)))
end
end
host.log('zb_params', c)
end
