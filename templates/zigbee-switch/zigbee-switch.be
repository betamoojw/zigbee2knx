class zb_switch
static KO_SWITCH = 1
static KO_SWITCH_STATUS = 2
static KO_IDENTIFY = 3
static KO_AVAILABLE = 4
static EP = 1
static POWER_ON = ['off', 'on', 'toggle', 'previous']
static F_POB = 1
static F_IDENTIFY = 2
static F_CHILD_LOCK = 16
static MODELS = {'TRADFRI control outlet': [3, ''], 'ASKVADER on/off switch': [3, ''], 'TRETAKT Smart plug': [19, 'fc85,0000,10,117c'], 'GRILLPLATS Plug\x00': [3, ''], 'GRILLPLATS Plug': [3, '']}
static P_POB = 0
static P_LOCK = 1
static def model()
var zm = host.dp_get(0, 'zigbeeModel')
var e = zm != nil ? zb_switch.MODELS.find(zm) : nil
return e != nil ? e : [3, '']
end
static def has(flag)
return (zb_switch.model()[0] & flag) != 0
end
static def clamp(v, lo, hi)
if v < lo return lo end
if v > hi return hi end
return v
end
static def apply()
var Z = zb_switch
var p = host.state_get('p')
var zm = host.dp_get(0, 'zigbeeModel')
if p == nil || zm == nil || host.state_get('pm') == zm return end
host.state_set('pm', zm)
if Z.has(Z.F_POB)
host.dp_set(Z.EP, 'power_on_behavior', Z.POWER_ON[Z.clamp(p[Z.P_POB], 0, 3)])
end
var e = Z.model()
if Z.has(Z.F_CHILD_LOCK) && e[1] != ''
host.dp_set(Z.EP, 'zcl.write', e[1] + (p[Z.P_LOCK] != 0 ? ',1' : ',0'))
end
end
end
def knx_write(ko, raw)
var Z = zb_switch
if ko == Z.KO_SWITCH && raw.size() == 1
host.dp_set(Z.EP, 'state', (raw[0] & 1) != 0 ? 'ON' : 'OFF')
elif ko == Z.KO_IDENTIFY && raw.size() == 1
if (raw[0] & 1) != 0 && Z.has(Z.F_IDENTIFY) host.dp_set(Z.EP, 'identify', 'identify') end
else
host.log('ko_write_ignored', {'ko': ko, 'value': raw.tohex()})
end
end
def dp_event(ch, key, value, initial)
var Z = zb_switch
if ch == 0
if key == 'availability'
host.ko_status(Z.KO_AVAILABLE, value == true ? bytes('01') : bytes('00'), initial)
elif key == 'zigbeeModel'
Z.apply()
end
return
end
if ch != Z.EP return end
if key == 'state'
host.ko_status(Z.KO_SWITCH_STATUS, value == 'ON' ? bytes('01') : bytes('00'), initial)
end
end
def params(p)
var Z = zb_switch
if p.size() < 2 return end
host.state_set('p', p)
host.state_set('pm', nil)
Z.apply()
host.log('zb_params', {'pob': p[Z.P_POB], 'lock': p[Z.P_LOCK], 'wait_model': host.dp_get(0, 'zigbeeModel') == nil})
end
