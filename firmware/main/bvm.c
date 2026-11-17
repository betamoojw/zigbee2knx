// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
// Berry with the class templates: one VM per template (per template digest), the prelude (berry/prelude.be)
// unchanged, every instance of a class inside its class's VM. Slots 0 and 1 run the two classes built
// into the firmware. The slots from 2 run templates that ETS downloaded inside a parameter segment; kgt.c checks
// checksum and signature first, and only a verified payload reaches a VM (Berry is no sandbox).
// One worker task serves all VMs. Requests are the JSON lines of berry/PROTOCOL.md; instead of the prelude's
// _main() loop (stdin) a small helper runs one line per call. Ops a hook emits are counted here and handed to zlink.c,
// which acts on them for instances linked to a real Zigbee device.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "berry.h"
#include "be_gc.h"
#include "app.h"

static const char *TAG = "bvm";

extern const char prelude_be[] asm("_binary_prelude_be_start");
extern const char dimmable_be[] asm("_binary_zigbee_dimmable_light_be_start");
extern const char white_spectrum_be[] asm("_binary_zigbee_white_spectrum_light_be_start");

// one request line per call, the same steps as the body of the prelude's _main()
static const char HELPER_BE[] =
    "def _line()\n"
    "    var req = json.load(_req())\n"
    "    if req == nil || classname(req) != 'map'  return 'request is not a JSON object' end\n"
    "    _inst = req.find('inst', 0)\n"
    "    try\n"
    "        _call(req)\n"
    "    except .. as e, m\n"
    "        return str(e) + ': ' + str(m)\n"
    "    end\n"
    "    return nil\n"
    "end\n";

// What a template VM runs first, with the verified payload: take the Berry source out of it and run it. A template
// made for another radio family is refused although a trusted key signed it.
static const char BOOT_BE[] =
    "_tpl_id = ''\n"
    "_tpl_version = ''\n"
    "_tpl_models = nil\n"
    "def _kgt_boot()\n"
    "    var p = json.load(_kgt_payload())\n"
    "    if p == nil || classname(p) != 'map' || p.find('schema') != 'kgt/1'  return 'payload is not a kgt/1 template' end\n"
    "    var m = p.find('manifest')\n"
    "    var b = p.find('behaviour')\n"
    "    if m == nil || classname(m) != 'map' || type(b) != 'string'  return 'payload is not a kgt/1 template' end\n"
    "    if type(p.find('id')) != 'string' || p.find('version') == nil  return 'template without id or version' end\n"
    "    if m.find('schema') != 'knxgw-template/0'  return 'manifest schema ' + str(m.find('schema')) end\n"
    "    var bh = m.find('behaviour')\n"
    "    if bh == nil || classname(bh) != 'map' || bh.find('language') != 'berry'  return 'behaviour is not Berry' end\n"
    "    if m.find('radio') != 'zigbee'\n"
    "        return 'template ' + str(p.find('id')) + ' is for radio ' + str(m.find('radio')) + ', this gateway is zigbee'\n"
    "    end\n"
    "    _tpl_id = str(p.find('id'))\n"
    "    _tpl_version = str(p.find('version'))\n"
    "    var mt = m.find('match')\n"
    "    var zm = mt != nil && classname(mt) == 'map' ? mt.find('zigbeeModel') : nil\n"
    "    if zm != nil && classname(zm) == 'list' && zm.size() > 0\n"
    "        _tpl_models = {}\n"
    "        for x : zm  _tpl_models[x] = true end\n"
    "    end\n"
    "    p = nil\n"
    "    m = nil\n"
    "    compile(b)()\n"
    "    return nil\n"
    "end\n"
    // Is the model an instance reported one of those the template was made for? '1|44|model' or '0|44|model'; ''
    // while the device has not told its model yet; nil if the template names no models (an empty list is none).
    "def _tpl_check(inst)\n"
    "    if _tpl_models == nil  return nil end\n"
    "    var d = _dp.find(inst)\n"
    "    var zm = d != nil ? d.find('0.zigbeeModel') : nil\n"
    "    if zm == nil  return '' end\n"
    "    return (_tpl_models.find(zm) != nil ? '1' : '0') + '|' + str(_tpl_models.size()) + '|' + str(zm)\n"
    "end\n";

#define NSLOT BVM_SLOTS

typedef struct {
    const char *name, *src, *model, *seg;
    bvm *vm;
    int inst;          // instances in this VM
    uint32_t ops, errs, calls;
    size_t heap_load;  // heap the VM took to load prelude + template + helper
    size_t heap_peak;  // how far the free heap fell while it loaded (other tasks included), the figure a refusal needs
    size_t plen;       // the template's Berry source, unpacked
    char id[48], digest[17], fpr[17];   // slots from 2: the template, its content digest and the signing key
} bclass_t;

// models and default parameter segments (tests/vmtest.py Template.segment())
static bclass_t s_cls[NSLOT] = {
    {.name = "zigbee-dimmable-light", .model = "TRADFRIbulbE27WWclear250lm", .seg = "0101646401016400003c"},
    {.name = "zigbee-white-spectrum-light", .model = "TRADFRI bulb E27 WS clear 806lm",
     .seg = "0101646401016400003c03e82710000a8c3700000a8c"},
};

// a request line; collect everywhere; GC step rate (line = percent); load a template (s_job); drop unused template
// VMs; compare a model; check a template without loading it
enum { OP_LINE, OP_GC, OP_RATE, OP_TPL, OP_KEEP, OP_CHECK, OP_TCHECK };
typedef struct {
    uint8_t op;
    int k;   // slot of an OP_LINE
    char line[240];
} breq_t;

typedef struct {
    const uint8_t *seg;
    size_t n, wn;
    kgt_info_t *info;
    char *why;
    uint32_t mask;
    int result;
    int k, inst, count;   // OP_CHECK: slot and instance in, number of models out; why/wn take the model
    bool dry;             // OP_TCHECK: every check up to the VM, none is brought up
    size_t free_extra;    // OP_TCHECK: heap that goes back before the template would load
} tjob_t;
static tjob_t s_job;   // one job at a time, under s_mx

static QueueHandle_t s_q;
static SemaphoreHandle_t s_done, s_mx;
static volatile bool s_ok, s_loaded;
static TaskHandle_t s_task;
static int s_total;   // instances created, numbered 1..s_total across both VMs
static uint32_t s_id;
// Collect when usage grew by 20 % since the last collection. Berry's default of 200 % lets every VM pile up as much
// garbage as it holds alive. Measured 2026-11-13 with 30 simulated devices: heap minimum 16 KB and largest block
// 18 KB at 200 %, 84 KB and 66 KB at 120 %.
static volatile int s_rate = 120;
static volatile uint32_t s_gc_us;
#define CALL_US 2000000              // a hook that runs longer is stopped
#define BOOT_US 5000000              // loading a template
// Only the Berry task touches these. Whatever a VM needs from C is fetched by the VM from inside the protected call:
// a Berry API call that needs memory outside of one ends in abort() when there is none (be_exec.c, be_throw()).
static const char *s_cur_line;       // the request line of the call that is running
static uint8_t *s_payload;           // the verified payload on its way into a new VM
static size_t s_plen;
static int64_t s_deadline;           // 0: no call is running
static bool s_stopped;               // the call that just ended was stopped for taking too long
static uint32_t s_timeouts;

static bclass_t *cls_of(bvm *vm)
{
    for (int k = 0; k < NSLOT; k++) {
        if (s_cls[k].vm == vm) {
            return &s_cls[k];
        }
    }
    return NULL;
}

static int m_in(bvm *vm)   // the firmware never runs _main(); a stray _in() reads end of input
{
    be_return_nil(vm);
}

static int m_out(bvm *vm)
{
    bclass_t *c = cls_of(vm);
    if (c) {
        c->ops++;
    }
    if (be_top(vm) >= 1 && be_isstring(vm, 1)) {
        zlink_out(be_tostring(vm, 1));
    }
    be_return_nil(vm);
}

static int m_req(bvm *vm)
{
    be_pushstring(vm, s_cur_line ? s_cur_line : "");
    be_return(vm);
}

static int m_payload(bvm *vm)   // once, inside _kgt_boot(); the C copy goes as soon as Berry has its own
{
    if (!s_payload) {
        be_return_nil(vm);
    }
    be_pushnstring(vm, (const char *)s_payload, s_plen);
    free(s_payload);
    s_payload = NULL;
    be_return(vm);
}

// Called by the VM every 2^19 instructions: a call that does not come to an end is stopped. With be_exit() and not
// with an exception: a try block in Berry catches only exceptions (be_exec.c, be_except_block_resume()), so this
// ends the call whatever the template does, back in the be_pcall() that started it.
static void obs(bvm *vm, int event, ...)
{
    if (event == BE_OBS_VM_HEARTBEAT && s_deadline && esp_timer_get_time() > s_deadline) {
        s_deadline = 0;
        s_stopped = true;
        s_timeouts++;
        be_exit(vm, 0);
    }
}

static void fail(bvm *vm, const char *what, int r, const char *line)
{
    if (r == BE_EXCEPTION) {   // only then the exception is on the stack
        ESP_LOGW(TAG, "%s: exception <- %s", what, line);
        be_dumpexcept(vm);
    } else if (r == BE_EXIT && s_stopped) {
        ESP_LOGW(TAG, "%s: the call took too long and was stopped <- %s", what, line);
    } else {
        ESP_LOGE(TAG, "%s: Berry error %d%s <- %s", what, r, r == BE_MALLOC_FAIL ? " (out of memory)" : "", line);
    }
}

static int m_now_ms(bvm *vm)
{
    be_pushint(vm, (bint)(esp_timer_get_time() / 1000));
    be_return(vm);
}

static int load(bvm *vm, const char *name, const char *src)
{
    int top = be_top(vm);
    int r = be_loadbuffer(vm, name, src, strlen(src));
    if (r == 0) {
        r = be_pcall(vm, 0);
    }
    if (r != 0) {
        fail(vm, name, r, "load");
    }
    be_pop(vm, be_top(vm) - top);
    return r;
}

static bool run_line(bclass_t *c, const char *line)
{
    bvm *vm = c->vm;
    int top = be_top(vm);
    bool ok = false;
    s_cur_line = line;
    s_stopped = false;
    s_deadline = esp_timer_get_time() + CALL_US;
    be_getglobal(vm, "_line");
    int r = be_pcall(vm, 0);
    s_deadline = 0;
    s_cur_line = NULL;
    if (r == 0) {
        ok = be_isnil(vm, -1);
        if (!ok) {
            ESP_LOGW(TAG, "%s: %s <- %s", c->name, be_isstring(vm, -1) ? be_tostring(vm, -1) : "?", line);
        }
    } else {
        fail(vm, c->name, r, line);
    }
    be_pop(vm, be_top(vm) - top);
    if (r == BE_MALLOC_FAIL || r == BE_EXIT) {
        // What the call had allocated is garbage now, and Berry collects only when a VM grows past its last size
        // again. Seen on 2026-11-12: 215 KB stayed with the VM and 30 KB free for everybody else.
        be_gc_collect(vm);
        ESP_LOGW(TAG, "%s: collected after the failed call, Berry counts %u B, heap free %u B", c->name,
                 (unsigned)be_gc_memcount(vm), (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    }
    return ok;
}

static bvm *vm_new(void)
{
    bvm *vm = be_vm_new();
    be_gc_setsteprate(vm, s_rate);
    be_regfunc(vm, "_in", m_in);
    be_regfunc(vm, "_out", m_out);
    be_regfunc(vm, "_now_ms", m_now_ms);
    be_regfunc(vm, "_req", m_req);
    be_regfunc(vm, "_kgt_payload", m_payload);
    be_set_obs_hook(vm, obs);
    return vm;
}

static void global_str(bvm *vm, const char *name, char *out, size_t n)
{
    out[0] = 0;
    if (be_getglobal(vm, name) && be_isstring(vm, -1)) {
        snprintf(out, n, "%s", be_tostring(vm, -1));
    }
    be_pop(vm, 1);
}

// The deepest point of the free heap from low_begin() to low_end(): what loading a VM really takes at its peak,
// as against what it keeps. Other tasks allocating meanwhile count too, which is the figure a
// refusal has to go by anyway.
static size_t low_begin(void)
{
    size_t before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    heap_caps_monitor_local_minimum_free_size_start();
    return before;
}

static size_t low_end(size_t before)
{
    size_t low = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    heap_caps_monitor_local_minimum_free_size_stop();
    return before > low ? before - low : 0;
}

// On the Berry task: check the template embedded in a parameter segment and bring it up in a VM of its own.
static int tpl_load_vm(tjob_t *j, size_t before, bool *fresh);

static int tpl_load(tjob_t *j)
{
    bool fresh = false;
    size_t before = low_begin();
    int k = tpl_load_vm(j, before, &fresh);
    size_t peak = low_end(before);
    if (k >= 2 && fresh) {
        bclass_t *c = &s_cls[k];
        c->heap_peak = peak;
        ESP_LOGI(TAG, "slot %d: loading took up to %u B at its peak for %u B of source (%u.%u times), %u B stay", k,
                 (unsigned)peak, (unsigned)c->plen, c->plen ? (unsigned)(peak / c->plen) : 0,
                 c->plen ? (unsigned)(peak * 10 / c->plen % 10) : 0, (unsigned)c->heap_load);
    } else if (k == BVM_REFUSED) {
        ESP_LOGI(TAG, "refused load: the heap fell by up to %u B before it stopped", (unsigned)peak);
    }
    return k;
}

static int tpl_load_vm(tjob_t *j, size_t before, bool *fresh)
{
    uint8_t *payload = NULL;
    size_t plen = 0;
    int r = kgt_open(j->seg, j->n, &payload, &plen, j->info, j->why, j->wn);
    if (r == KGT_NONE) {
        return BVM_NO_TEMPLATE;
    }
    if (r != KGT_OK) {
        return BVM_REFUSED;   // kgt_open set info->err
    }
    int k = -1;
    for (int i = 2; i < NSLOT; i++) {
        if (s_cls[i].vm && strcmp(s_cls[i].digest, j->info->digest) == 0) {   // one VM per content
            free(payload);
            return j->dry ? 0 : i;
        }
        if (!s_cls[i].vm && k < 0) {
            k = i;
        }
    }
    if (k < 0) {
        free(payload);
        snprintf(j->why, j->wn, "all %d template slots are in use", NSLOT - 2);
        j->info->err = KNX_ERR_NO_RAM;
        return BVM_REFUSED;
    }
    // Measured on 2026-11-13: the heap falls by 39.2 KB loading 3 KB of source and by 80.1 KB loading 17.9 KB,
    // about 31 KB + 2.75 x source. An earlier estimate of six times the source asked far too much for a large
    // template and too little for a small one. Refuse rather than run dry (see s_cur_line above), and leave
    // the same 64 KB as before for Zigbee and the bus.
    // A check before the load counts the heap of the VM the device gives up (free_extra) as free; the largest block
    // it cannot predict, so a block that is too small refuses only if the VM given up could not make up for it.
    size_t fr = heap_caps_get_free_size(MALLOC_CAP_8BIT) + j->free_extra;
    size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t want = 32 * 1024 + 3 * plen + 64 * 1024;
    if (fr < want || (big < 2 * plen + 4096 && j->free_extra < 2 * plen + 4096)) {
        free(payload);
        snprintf(j->why, j->wn, "not enough memory to load it: %u B free, largest block %u B, about %u B needed",
                 (unsigned)fr, (unsigned)big, (unsigned)want);
        j->info->err = KNX_ERR_NO_RAM;
        return BVM_REFUSED;
    }
    if (j->dry) {
        free(payload);
        return 0;
    }
    bvm *vm = vm_new();
    bool ok = load(vm, "prelude.be", prelude_be) == 0 && load(vm, "boot", BOOT_BE) == 0;
    j->info->err = KNX_ERR_SOFTWARE;   // until it is up: whatever fails from here is the Berry side
    if (!ok) {
        snprintf(j->why, j->wn, "prelude does not load");
    } else {
        int top = be_top(vm);
        s_payload = payload;   // _kgt_payload() takes it over inside the protected call
        s_plen = plen;
        payload = NULL;
        s_stopped = false;
        s_deadline = esp_timer_get_time() + BOOT_US;
        be_getglobal(vm, "_kgt_boot");
        int e = be_pcall(vm, 0);
        s_deadline = 0;
        free(s_payload);       // still here if the VM never got to ask for it
        s_payload = NULL;
        if (e != 0) {
            ok = false;
            if (e == BE_EXIT && s_stopped) {
                snprintf(j->why, j->wn, "the template's Berry source did not come to an end within %d s",
                         BOOT_US / 1000000);
            } else {
                snprintf(j->why, j->wn, "the template's Berry source does not load (Berry error %d)", e);
            }
            fail(vm, "template", e, "_kgt_boot");
        } else if (!be_isnil(vm, -1)) {   // _kgt_boot refuses it, e.g. a template for another radio
            ok = false;
            snprintf(j->why, j->wn, "%s", be_isstring(vm, -1) ? be_tostring(vm, -1) : "the template does not start");
            j->info->err = KNX_ERR_CONFIG;
        }
        be_pop(vm, be_top(vm) - top);
        if (ok && load(vm, "helper", HELPER_BE) != 0) {
            ok = false;
            snprintf(j->why, j->wn, "helper does not load");
        }
    }
    free(payload);
    if (!ok) {
        be_vm_delete(vm);
        return BVM_REFUSED;
    }
    j->info->err = KNX_ERR_NONE;
    bclass_t *c = &s_cls[k];
    memset(c, 0, sizeof *c);
    char id[32], ver[12];
    global_str(vm, "_tpl_id", id, sizeof id);
    global_str(vm, "_tpl_version", ver, sizeof ver);
    snprintf(c->id, sizeof c->id, "%s v%s", id, ver);
    c->name = c->id;
    snprintf(c->digest, sizeof c->digest, "%s", j->info->digest);
    snprintf(c->fpr, sizeof c->fpr, "%s", j->info->fpr);
    be_gc_collect(vm);
    c->heap_load = before - heap_caps_get_free_size(MALLOC_CAP_8BIT);
    c->plen = plen;
    c->vm = vm;
    *fresh = true;
    ESP_LOGI(TAG, "slot %d: template %s loaded, digest %s, signed by %s, %u B heap, Berry counts %u B", k, c->name,
             c->digest, c->fpr, (unsigned)c->heap_load, (unsigned)be_gc_memcount(vm));
    return k;
}

// A built-in class gets its VM when a device first needs it, on this task. Not at start: two VMs nobody uses held
// about 71 KB, and on 2026-11-12 a white spectrum template was refused for want of memory next to them.
static bool builtin_up(int k)
{
    bclass_t *c = &s_cls[k];
    if (c->vm) {
        return true;
    }
    size_t before = low_begin();
    c->vm = vm_new();
    bool ok = c->vm && load(c->vm, "prelude.be", prelude_be) == 0 && load(c->vm, c->name, c->src) == 0 &&
              load(c->vm, "helper", HELPER_BE) == 0;
    if (!ok) {
        if (c->vm) {
            be_vm_delete(c->vm);
            c->vm = NULL;
        }
        low_end(before);
        ESP_LOGE(TAG, "%s: does not load (%u B heap free)", c->name,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return false;
    }
    be_gc_collect(c->vm);
    c->heap_load = before - heap_caps_get_free_size(MALLOC_CAP_8BIT);
    c->heap_peak = low_end(before);
    c->plen = strlen(c->src);
    ESP_LOGI(TAG, "%s: loaded for a device, %u B heap, Berry counts %u B; up to %u B at its peak for %u B of source",
             c->name, (unsigned)c->heap_load, (unsigned)be_gc_memcount(c->vm), (unsigned)c->heap_peak,
             (unsigned)c->plen);
    return true;
}

static void tpl_keep(uint32_t mask)
{
    for (int k = 0; k < NSLOT; k++) {
        bclass_t *c = &s_cls[k];
        if (c->vm && !(mask >> k & 1)) {
            if (k < 2) {   // a built-in class keeps its source and comes back when a device needs it
                ESP_LOGI(TAG, "slot %d: built-in class %s not used, VM deleted", k, c->name);
                be_vm_delete(c->vm);
                c->vm = NULL;
                c->inst = 0;
                c->heap_load = 0;
                continue;
            }
            ESP_LOGI(TAG, "slot %d: template %s no longer used, VM deleted", k, c->name);
            be_vm_delete(c->vm);
            memset(c, 0, sizeof *c);
        }
    }
}

// 1: the template lists the instance's model, 0: it does not, -1: nothing to compare, -2: no model yet
static int tpl_check(tjob_t *j)
{
    if (j->k < 2 || j->k >= NSLOT || !s_cls[j->k].vm) {
        return -1;
    }
    bvm *vm = s_cls[j->k].vm;
    int top = be_top(vm), r = -1;
    s_stopped = false;
    s_deadline = esp_timer_get_time() + CALL_US;
    be_getglobal(vm, "_tpl_check");
    be_pushint(vm, j->inst);
    if (be_pcall(vm, 1) == 0 && be_isstring(vm, -2)) {
        const char *s = be_tostring(vm, -2);
        if (!s[0]) {
            r = -2;
        } else if ((s[0] == '0' || s[0] == '1') && s[1] == '|') {
            const char *bar = strchr(s + 2, '|');
            if (bar) {
                r = s[0] == '1';
                j->count = atoi(s + 2);
                snprintf(j->why, j->wn, "%s", bar + 1);
            }
        }
    }
    s_deadline = 0;
    be_pop(vm, be_top(vm) - top);
    return r;
}

static void bvm_task(void *arg)
{
    (void)arg;
    s_cls[0].src = dimmable_be;
    s_cls[1].src = white_spectrum_be;
    s_loaded = true;   // the built-in classes come up when a device needs them (builtin_up)
    xSemaphoreGive(s_done);
    breq_t rq;
    for (;;) {
        xQueueReceive(s_q, &rq, portMAX_DELAY);
        if (rq.op == OP_GC) {   // collect in every VM, on this task's stack
            int64_t t0 = esp_timer_get_time();
            for (int k = 0; k < NSLOT; k++) {
                if (s_cls[k].vm) {
                    be_gc_collect(s_cls[k].vm);
                }
            }
            s_gc_us = (uint32_t)(esp_timer_get_time() - t0);
            s_ok = true;
        } else if (rq.op == OP_RATE) {
            s_rate = atoi(rq.line);
            for (int k = 0; k < NSLOT; k++) {
                if (s_cls[k].vm) {
                    be_gc_setsteprate(s_cls[k].vm, s_rate);
                }
            }
            s_ok = true;
        } else if (rq.op == OP_TPL) {
            s_job.result = tpl_load(&s_job);
            s_ok = true;
        } else if (rq.op == OP_KEEP) {
            tpl_keep(s_job.mask);
            s_ok = true;
        } else if (rq.op == OP_CHECK) {
            s_job.result = tpl_check(&s_job);
            s_ok = true;
        } else if (rq.op == OP_TCHECK) {
            bool fresh = false;
            s_job.result = tpl_load_vm(&s_job, 0, &fresh);
            s_ok = true;
        } else {
            s_ok = (rq.k >= 2 || builtin_up(rq.k)) && s_cls[rq.k].vm && run_line(&s_cls[rq.k], rq.line);
        }
        xSemaphoreGive(s_done);
    }
}

esp_err_t bvm_start(void)
{
    s_q = xQueueCreate(1, sizeof(breq_t));
    s_done = xSemaphoreCreateBinary();
    s_mx = xSemaphoreCreateMutex();
    xTaskCreate(bvm_task, "berry", 16384, NULL, 4, &s_task);
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(20000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return s_loaded ? ESP_OK : ESP_FAIL;
}

static bool vcall(int k, const char *fmt, va_list ap)
{
    if (!s_loaded || k < 0 || k >= NSLOT || (k >= 2 && !s_cls[k].vm)) {   // a built-in class comes up on demand
        return false;
    }
    breq_t rq = {.op = OP_LINE, .k = k};
    if (vsnprintf(rq.line, sizeof rq.line, fmt, ap) >= (int)sizeof rq.line) {
        ESP_LOGW(TAG, "request longer than %u B, not sent", (unsigned)sizeof rq.line);
        return false;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    xQueueSend(s_q, &rq, portMAX_DELAY);
    xSemaphoreTake(s_done, portMAX_DELAY);
    bool ok = s_ok;
    xSemaphoreGive(s_mx);
    s_cls[k].calls++;
    if (!ok) {
        s_cls[k].errs++;
    }
    return ok;
}

static bool call(int k, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    bool ok = vcall(k, fmt, ap);
    va_end(ap);
    return ok;
}

bool bvm_line(int k, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    bool ok = vcall(k, fmt, ap);
    va_end(ap);
    return ok;
}

unsigned long bvm_next_id(void)
{
    return (unsigned long)++s_id;
}

bool bvm_slot_ok(int k)   // a built-in class is always there: its VM comes up when it is used
{
    return s_loaded && k >= 0 && k < NSLOT && (k < 2 || s_cls[k].vm);
}

const char *bvm_slot_name(int k)   // for logs; a template slot that is being emptied just now reads "none"
{
    const char *n = k >= 0 && k < NSLOT && (k < 2 || s_cls[k].vm) ? s_cls[k].name : NULL;
    return n ? n : "none";
}

int bvm_template(const uint8_t *seg, size_t n, kgt_info_t *info, char *why, size_t wn)
{
    why[0] = 0;
    memset(info, 0, sizeof *info);
    if (!s_loaded) {
        snprintf(why, wn, "Berry is not up");
        info->err = KNX_ERR_SOFTWARE;
        return BVM_REFUSED;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_job = (tjob_t){.seg = seg, .n = n, .wn = wn, .info = info, .why = why};
    breq_t rq = {.op = OP_TPL};
    xQueueSend(s_q, &rq, portMAX_DELAY);
    xSemaphoreTake(s_done, portMAX_DELAY);
    int r = s_job.result;
    xSemaphoreGive(s_mx);
    return r;
}

int bvm_template_check(const uint8_t *seg, size_t n, size_t free_extra, kgt_info_t *info, char *why, size_t wn)
{
    why[0] = 0;
    memset(info, 0, sizeof *info);
    if (!s_loaded) {
        snprintf(why, wn, "Berry is not up");
        info->err = KNX_ERR_SOFTWARE;
        return BVM_REFUSED;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_job = (tjob_t){.seg = seg, .n = n, .wn = wn, .info = info, .why = why, .dry = true, .free_extra = free_extra};
    breq_t rq = {.op = OP_TCHECK};
    xQueueSend(s_q, &rq, portMAX_DELAY);
    xSemaphoreTake(s_done, portMAX_DELAY);
    int r = s_job.result;
    xSemaphoreGive(s_mx);
    return r;
}

// Read without the lock, as bvm_slot_name(): a slot changes only on the Berry task, at the request of the device
// server, which is the one asking here.
int bvm_vm_count(void)   // for the gateway's diagnostics; read without the lock, as bvm_slot_heap()
{
    int n = 0;
    for (int k = 0; k < NSLOT; k++) {
        n += s_cls[k].vm != NULL;
    }
    return n;
}

size_t bvm_slot_heap(int k)
{
    return k >= 0 && k < NSLOT && s_cls[k].vm ? s_cls[k].heap_load : 0;
}

const char *bvm_slot_digest(int k)
{
    return k >= 2 && k < NSLOT && s_cls[k].vm ? s_cls[k].digest : "";
}

int bvm_model_check(int k, int inst, char *model, size_t n, int *count)
{
    model[0] = 0;
    *count = 0;
    if (!s_loaded) {
        return -1;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_job = (tjob_t){.k = k, .inst = inst, .why = model, .wn = n};
    breq_t rq = {.op = OP_CHECK};
    xQueueSend(s_q, &rq, portMAX_DELAY);
    xSemaphoreTake(s_done, portMAX_DELAY);
    int r = s_job.result;
    *count = s_job.count;
    xSemaphoreGive(s_mx);
    return r;
}

void bvm_keep(uint32_t mask)
{
    if (!s_loaded) {
        return;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_job = (tjob_t){.mask = mask};
    breq_t rq = {.op = OP_KEEP};
    xQueueSend(s_q, &rq, portMAX_DELAY);
    xSemaphoreTake(s_done, portMAX_DELAY);
    xSemaphoreGive(s_mx);
}

const char *bvm_class_model(int k)
{
    return k == 0 || k == 1 ? s_cls[k].model : "";
}

const char *bvm_class_seg(int k)
{
    return k == 0 || k == 1 ? s_cls[k].seg : "";
}

// the calls a fresh device gets: identity, parameters from the ETS download, the first reports, one KNX write each
static void create(int inst)
{
    int k = (inst - 1) % 2;
    bclass_t *c = &s_cls[k];
    call(k, "{\"call\":\"dp_event\",\"inst\":%d,\"id\":%lu,\"ch\":0,\"key\":\"zigbeeModel\",\"value\":\"%s\",\"initial\":true}",
         inst, (unsigned long)++s_id, c->model);
    call(k, "{\"call\":\"params\",\"inst\":%d,\"id\":%lu,\"p\":\"%s\"}", inst, (unsigned long)++s_id, c->seg);
    call(k, "{\"call\":\"dp_event\",\"inst\":%d,\"id\":%lu,\"ch\":1,\"key\":\"brightness\",\"value\":200,\"initial\":true}",
         inst, (unsigned long)++s_id);
    call(k, "{\"call\":\"dp_event\",\"inst\":%d,\"id\":%lu,\"ch\":1,\"key\":\"state\",\"value\":\"ON\",\"initial\":true}",
         inst, (unsigned long)++s_id);
    if (k == 1) {
        call(k, "{\"call\":\"dp_event\",\"inst\":%d,\"id\":%lu,\"ch\":1,\"key\":\"colorTempPhysicalMin\",\"value\":250,\"initial\":true}",
             inst, (unsigned long)++s_id);
        call(k, "{\"call\":\"dp_event\",\"inst\":%d,\"id\":%lu,\"ch\":1,\"key\":\"colorTempPhysicalMax\",\"value\":454,\"initial\":true}",
             inst, (unsigned long)++s_id);
        call(k, "{\"call\":\"dp_event\",\"inst\":%d,\"id\":%lu,\"ch\":1,\"key\":\"color_temp\",\"value\":370,\"initial\":true}",
             inst, (unsigned long)++s_id);
        call(k, "{\"call\":\"knx_write\",\"inst\":%d,\"id\":%lu,\"ko\":7,\"raw\":\"0a8c\"}", inst, (unsigned long)++s_id);
    }
    call(k, "{\"call\":\"knx_write\",\"inst\":%d,\"id\":%lu,\"ko\":3,\"raw\":\"80\"}", inst, (unsigned long)++s_id);
    c->inst++;
}

int bvm_populate(int total)
{
    if (!s_loaded) {
        return 0;
    }
    while (s_total < total) {
        create(++s_total);
    }
    breq_t rq = {.op = OP_GC};
    xSemaphoreTake(s_mx, portMAX_DELAY);
    xQueueSend(s_q, &rq, portMAX_DELAY);
    xSemaphoreTake(s_done, portMAX_DELAY);
    xSemaphoreGive(s_mx);
    return s_total;
}

// one step of simulated traffic: a brightness report and a KNX switch command for a random instance
void bvm_sim_step(void)
{
    if (!s_total) {
        return;
    }
    int inst = 1 + (int)(esp_random() % (uint32_t)s_total);
    int k = (inst - 1) % 2;
    call(k, "{\"call\":\"dp_event\",\"inst\":%d,\"id\":%lu,\"ch\":1,\"key\":\"brightness\",\"value\":%u,\"initial\":false}",
         inst, (unsigned long)++s_id, (unsigned)(1 + esp_random() % 254));
    call(k, "{\"call\":\"knx_write\",\"inst\":%d,\"id\":%lu,\"ko\":1,\"raw\":\"%s\"}", inst, (unsigned long)++s_id,
         (esp_random() & 1) ? "01" : "00");
}

void bvm_print(void)
{
    if (s_mx) {
        xSemaphoreTake(s_mx, portMAX_DELAY);   // no request runs meanwhile, so no VM goes away under the reading
    }
    for (int k = 0; k < NSLOT; k++) {
        bclass_t *c = &s_cls[k];
        if (k >= 2 && !c->vm) {
            continue;
        }
        printf("berry slot %d %s: %s, load %u B (peak %u B, source %u B), now %u B (Berry count), %d instances, "
               "%lu calls, %lu errors, %lu ops",
               k, c->name, c->vm ? "up" : "down", (unsigned)c->heap_load, (unsigned)c->heap_peak, (unsigned)c->plen,
               c->vm ? (unsigned)be_gc_memcount(c->vm) : 0, c->inst, (unsigned long)c->calls,
               (unsigned long)c->errs, (unsigned long)c->ops);
        if (k >= 2) {
            printf("; from a download, digest %s, signed by %s", c->digest, c->fpr);
        }
        printf("\n");
    }
    printf("berry task stack free %u B, %lu calls stopped after %d s\n",
           s_task ? (unsigned)(uxTaskGetStackHighWaterMark(s_task) * sizeof(StackType_t)) : 0,
           (unsigned long)s_timeouts, CALL_US / 1000000);
    if (s_mx) {
        xSemaphoreGive(s_mx);
    }
}

static void ctl(int op, const char *line)
{
    breq_t rq = {.op = (uint8_t)op};
    snprintf(rq.line, sizeof rq.line, "%s", line);
    xSemaphoreTake(s_mx, portMAX_DELAY);
    xQueueSend(s_q, &rq, portMAX_DELAY);
    xSemaphoreTake(s_done, portMAX_DELAY);
    xSemaphoreGive(s_mx);
}

void bvm_gc(void)
{
    if (!s_loaded) {
        return;
    }
    size_t before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ctl(OP_GC, "");
    printf("gc: %lu us, heap free %u -> %u B\n", (unsigned long)s_gc_us, (unsigned)before,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
}

void bvm_set_rate(int pct)
{
    if (!s_loaded || pct < 101 || pct > 355) {   // Berry keeps rate - 100 in one octet
        printf("gcrate: 101..355 %% (now %d)\n", s_rate);
        return;
    }
    char b[8];
    snprintf(b, sizeof b, "%d", pct);
    ctl(OP_RATE, b);
    printf("gcrate: %d %%\n", s_rate);
}
