import string
class zb_remote
static KO_SWITCH = 1
static KO_DIM = 2
static KO_VALUE = 3
static KO_BATTERY = 4
static KO_SWITCH_STATUS = 5
static KO_AVAILABLE = 6
static F_PERCENT = 2
static F_PERCENT_BELOW = 4
static STEP_LIMITS = [180, 90, 45, 23, 12, 6]
static MODELS = {'TRADFRI on/off switch': [5, [2, 4, 0]], 'SYMFONISK Sound Controller': [5, [2, 4, 0]], 'TRADFRI SHORTCUT Button': [5, [2, 4, 0]], 'KNYCKLAN Open/Close remote': [5, [2, 4, 0]], 'RODRET Dimmer': [1, []], 'RODRET wireless dimmer': [1, []], 'TRADFRI wireless dimmer': [3, []]}
static P_INV = 0
static def cfg()
var c = host.state_get('cfg')
return c == nil ? {'inv': false} : c
end
static def model()
var zm = host.dp_get(0, 'zigbeeModel')
var e = zm != nil ? zb_remote.MODELS.find(zm) : nil
return e != nil ? e : [1, []]
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
static def step_code(size)
var code = 1
for lim : zb_remote.STEP_LIMITS
if size < lim code += 1 end
end
return code
end
static def ident(s, i, j)
if i >= j return false end
while i < j
var c = string.byte(s[i])
if !((c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c == 45) return false end
i += 1
end
return true
end
static def idents(s, i, j)
var k = i
while k <= j
if k == j || s[k] == '.'
if !zb_remote.ident(s, i, k) return false end
i = k + 1
end
k += 1
end
return true
end
static def version(s)
if type(s) != 'string' return nil end
var i = 0
var n = size(s)
while i < n && string.find(' \t\r\n', s[i]) >= 0 i += 1 end
while n > i && string.find(' \t\r\n', s[n - 1]) >= 0 n -= 1 end
while i < n && string.find('v= \t\r\n', s[i]) >= 0 i += 1 end
var v = []
for f : 0 .. 2
var d = 0
var k = i
while k < n && string.byte(s[k]) >= 48 && string.byte(s[k]) <= 57
d = d * 10 + string.byte(s[k]) - 48
k += 1
end
if k == i return nil end
v.push(d)
i = k
if f < 2
if i >= n || s[i] != '.' return nil end
i += 1
end
end
var plus = string.find(s, '+', i)
var e = plus >= 0 && plus < n ? plus : n
if plus >= 0 && plus < n && !zb_remote.idents(s, plus + 1, n) return nil end
var pre = false
if i < e
if s[i] == '-' && i + 1 == e
pre = true
else
if s[i] == '-' i += 1 end
if s[i] == '.' || !zb_remote.idents(s, i, e) return nil end
pre = true
end
end
v.push(pre)
return v
end
static def below(v, w)
for f : 0 .. 2
if v[f] != w[f] return v[f] < w[f] end
end
return v[3]
end
static def battery(raw)
var Z = zb_remote
var e = Z.model()
var half = raw
if (e[0] & Z.F_PERCENT) != 0
half = raw * 2
elif (e[0] & Z.F_PERCENT_BELOW) != 0
var v = Z.version(host.dp_get(0, 'swBuildId'))
if v != nil && Z.below(v, e[1]) half = raw * 2 end
end
var b = bytes()
b.add(half >= 200 ? 255 : (half * 255 + 100) / 200, 1)
return b
end
end
def knx_write(ko, raw)
var Z = zb_remote
if ko == Z.KO_SWITCH_STATUS && raw.size() == 1
var v = (raw[0] & 1) != 0
host.state_set('sw', Z.cfg()['inv'] ? !v : v)
else
host.log('ko_write_ignored', {'ko': ko, 'value': raw.tohex()})
end
end
def dp_event(ch, key, value, initial)
var Z = zb_remote
if ch == 0
if key == 'availability'
host.ko_status(Z.KO_AVAILABLE, value == true ? bytes('01') : bytes('00'), initial)
elif key == 'zigbeeModel' || key == 'swBuildId'
var bat = host.state_get('bat')
if bat != nil host.ko_status(Z.KO_BATTERY, Z.battery(bat[0]), bat[1]) end
end
return
end
if key == 'batteryPercentageRemaining'
if type(value) == 'int' && value >= 0 && value < 255
host.state_set('bat', [value, initial])
host.ko_status(Z.KO_BATTERY, Z.battery(value), initial)
end
return
end
if key == 'battery'
if type(value) == 'int' || type(value) == 'real'
var b = bytes()
b.add(value >= 100 ? 255 : value <= 0 ? 0 : int((value * 255 + 50) / 100), 1)
host.ko_status(Z.KO_BATTERY, b, initial)
end
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
var v = (p[0] * 255 + 127) / 254
b.add(v > 255 ? 255 : v, 1)
host.ko_send(Z.KO_VALUE, b)
elif c == 0x01 || c == 0x05
Z.dim(up ? 0x09 : 0x01)
elif (c == 0x02 || c == 0x06) && size(p) > 1
if p[1] > 0 Z.dim((up ? 0x08 : 0x00) | Z.step_code(p[1])) end
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
