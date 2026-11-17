# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dirk Tostmann
#
# Prelude of knxgw-berry: the host.* API of the device templates and the request loop (PROTOCOL.md).
# The firmware loads it unchanged into every template VM and hands it one request line per call; on a PC,
# knxgw-berry runs it with _main(), which reads the requests from stdin and answers on stdout.
#
# One VM serves every instance of one template. Before a hook runs, _inst names the instance; the
# host functions act on it. A template keeps no state of its own between calls: per-instance values
# live in host.state_*, the last value of every datapoint in host.dp_get.

import json
import string

_inst = 0
_dp = {}            # instance -> {"ch.KEY": last value}
_st = {}            # instance -> {key: value}

def _emit(m)
    m['inst'] = _inst
    _out(json.dump(m))
end

# A value with its Berry type: JSON alone loses the kind of a number (5.0 and 5 look the same
# after json.dump).
def _typed(v)
    var t = type(v)
    if t == 'bool' || t == 'int' || t == 'real' || t == 'string'  return [t, v] end
    raise 'value_error', 'datapoint values are bool, int, real or string, not ' + t
end

class _Host
    # --- radio side: channel number + parameter name (Zigbee: endpoint + Zigbee2MQTT property)
    def dp_set(ch, key, value)                # write a VALUES datapoint
        var tv = _typed(value)
        _emit({'op': 'dp_set', 'ch': ch, 'key': key, 'vt': tv[0], 'value': tv[1]})
    end
    def dp_get(ch, key)                       # last value the device reported (nil: none yet)
        var m = _dp.find(_inst)
        return m == nil ? nil : m.find(str(ch) + '.' + key)
    end
    def master_put(ch, values)                # MASTER values of one channel; the link writes what changed
        var vals = {}
        var types = {}
        for k : values.keys()
            var tv = _typed(values[k])
            types[k] = tv[0]
            vals[k] = tv[1]
        end
        _emit({'op': 'master_put', 'ch': ch, 'values': vals, 'vt': types})
    end
    # --- KNX side: raw object values as bytes, as in the group object table
    def ko_send(ko, raw)                      # send now (a trigger, an event)
        _emit({'op': 'ko_send', 'ko': ko, 'raw': raw.tohex()})
    end
    def ko_status(ko, raw, initial)           # status object: sent on change; initial=true only fills the shadow
        _emit({'op': 'ko_status', 'ko': ko, 'raw': raw.tohex(), 'initial': initial ? true : false})
    end
    # --- instance state and log
    def state_get(key)
        var m = _st.find(_inst)
        return m == nil ? nil : m.find(key)
    end
    def state_set(key, value)
        if !_st.contains(_inst)  _st[_inst] = {} end
        _st[_inst][key] = value
    end
    def log(kind, fields)
        _emit({'op': 'log', 'kind': kind, 'fields': fields == nil ? {} : fields})
    end
    def now_ms()
        return _now_ms()
    end
end
host = _Host()

# Hooks a template may define (missing ones are skipped):
#   knx_write(ko, raw)              group write from KNX to object ko (raw: bytes)
#   dp_event(ch, key, value, initial)  a datapoint value from the radio side; initial = read at start
#   params(p)                       parameter segment (LSM 4) after a download or a start (p: bytes)
#   start()                         the instance starts (after its state was restored)
def _call(req)
    import global
    var c = req['call']
    var fn = nil
    var args = []
    if c == 'knx_write'
        fn = global.contains('knx_write') ? global.knx_write : nil
        args = [req['ko'], bytes(req['raw'])]
    elif c == 'dp_event'
        var key = str(req['ch']) + '.' + req['key']
        if !_dp.contains(_inst)  _dp[_inst] = {} end
        _dp[_inst][key] = req['value']        # before the hook: it may combine several datapoints
        fn = global.contains('dp_event') ? global.dp_event : nil
        args = [req['ch'], req['key'], req['value'], req.find('initial', false)]
    elif c == 'params'
        fn = global.contains('params') ? global.params : nil
        args = [bytes(req['p'])]
    elif c == 'start'
        fn = global.contains('start') ? global.start : nil
    elif c == 'dp_seed'                        # after a restart of the VM: last values back, no hook runs
        if !_dp.contains(_inst)  _dp[_inst] = {} end
        _dp[_inst][str(req['ch']) + '.' + req['key']] = req['value']
    elif c == 'forget'                         # instance removed: drop its datapoints and state
        _dp.remove(_inst)
        _st.remove(_inst)
    else
        raise 'value_error', 'unknown call ' + str(c)
    end
    if fn != nil  call(fn, args) end
end

def _main()
    _out(json.dump({'op': 'ready', 'berry': '1.1.0'}))
    while true
        var line = _in()
        if line == nil  break end
        if size(line) == 0  continue end
        var req = json.load(line)
        var ok = true
        var err = nil
        if req == nil || classname(req) != 'map'
            ok = false
            err = 'request is not a JSON object'
        else
            _inst = req.find('inst', 0)
            try
                _call(req)
            except .. as e, m
                ok = false
                err = str(e) + ': ' + str(m)
            end
        end
        var id = (req != nil && classname(req) == 'map') ? req.find('id') : nil
        _out(json.dump({'op': 'done', 'id': id, 'ok': ok, 'err': err}))
    end
end
