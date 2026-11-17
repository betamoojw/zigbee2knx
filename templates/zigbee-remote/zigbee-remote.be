import string
class zb_remote
static KO_SWITCH = 1
static KO_DIM = 2
static KO_VALUE = 3
static KO_BATTERY = 4
static KO_SWITCH_STATUS = 5
static KO_AVAILABLE = 6
static MODELS = {'TRADFRI on/off switch': [1], 'SYMFONISK Sound Controller': [1], 'TRADFRI SHORTCUT Button': [1], 'KNYCKLAN Open/Close remote': [1], 'RODRET Dimmer': [1], 'RODRET wireless dimmer': [1], 'TRADFRI wireless dimmer': [1]}
static P_INV = 0
static def cfg()
var c = host.state_get('cfg')
return c == nil ? {'inv': false} : c
end
static def byte(v)
var b = bytes()
b.add((v * 255 + 50) / 100, 1)
return b
end
static def switch(on)
host.state_set('sw', on)
var out = zb_remote.cfg()['inv'] ? !on : on
host.ko_send(zb_remote.KO_SWITCH, out ? bytes('01') : bytes('00'))
end
static def dim(code)
var b = bytes()
b.add(code, 1)
host.ko_send(zb_remote.KO_DIM, b)
end
end
def knx_write(ko, raw)
var Z = zb_remote
if ko == Z.KO_SWITCH_STATUS && raw.size() == 1
host.state_set('sw', (raw[0] & 1) != 0)
else
host.log('ko_write_ignored', {'ko': ko, 'value': raw.tohex()})
end
end
def dp_event(ch, key, value, initial)
var Z = zb_remote
if ch == 0
if key == 'availability'
host.ko_status(Z.KO_AVAILABLE, value == true ? bytes('01') : bytes('00'), initial)
end
return
end
if key == 'battery'
if value != nil host.ko_status(Z.KO_BATTERY, Z.byte(value), initial) end
return
end
if key != 'zcl.cmd' || value == nil return end
var f = string.split(value, ',')
if size(f) < 2 return end
var cl = bytes(f[0])
var cmd = bytes(f[1])
var p = size(f) > 2 ? bytes(f[2]) : bytes()
if size(cl) != 2 || size(cmd) != 1 return end
var cluster = (cl[0] << 8) | cl[1]
var c = cmd[0]
if cluster == 0x0006
if c == 0x00 || c == 0x40
Z.switch(false)
elif c == 0x01 || c == 0x41 || c == 0x42
Z.switch(true)
elif c == 0x02
var last = host.state_get('sw')
Z.switch(last == nil ? true : !last)
end
elif cluster == 0x0008
var up = size(p) > 0 && p[0] == 0
if (c == 0x00 || c == 0x04) && size(p) > 0
var b = bytes()
b.add((p[0] * 255 + 127) / 254, 1)
host.ko_send(Z.KO_VALUE, b)
elif c == 0x01 || c == 0x05
Z.dim(up ? 0x09 : 0x01)
elif c == 0x02 || c == 0x06
Z.dim(up ? 0x0C : 0x04)
elif c == 0x03 || c == 0x07
Z.dim(0x00)
end
else
host.log('command_ignored', {'cmd': value})
end
end
def params(p)
if p.size() < 1 return end
host.state_set('cfg', {'inv': p[zb_remote.P_INV] != 0})
end
