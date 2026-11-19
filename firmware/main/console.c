// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
// Console on USB-Serial-JTAG (TUL32) or UART0 (TULX32). Commands: heap, tasks, zb, join [s], zbreset yes, knx, ack,
// stat, gc; Zigbee devices: on|off|toggle ADDR, level ADDR L, rd ADDR CL ATTR, basic ADDR; Berry <-> device: zlink INST
// ADDR, kw INST KO HEX, links; KNX devices: vdev, prog, ia, dump, apdu, gw, pair, ... The measuring commands of the
// first bench firmware (dev N, sim on|off, gcrate N) are in the TUL32 build only.
// Every number is checked whole: atoi() made 0 of a typo, and "vdev del x yes" removed device 0.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h"

#if defined(CONFIG_Z2K_BOARD_TULX32) && CONFIG_Z2K_BOARD_TULX32
#define CONSOLE_BENCH 0   // a product: no measuring commands
#else
#define CONSOLE_BENCH 1
#endif

static volatile bool s_sim;

// a whole number from lo to hi, decimal or 0x hex; false: something else
static bool parse_int(const char *s, long lo, long hi, int *v)
{
    char *e;
    long x = strtol(s, &e, 0);
    if (e == s || *e || x < lo || x > hi) {
        return false;
    }
    *v = (int)x;
    return true;
}

static int c_heap(int argc, char **argv)
{
    app_heap("cmd");
    return 0;
}

static int c_tasks(int argc, char **argv)
{
    static char buf[1024];
    vTaskList(buf);   // IDF 6.0.1 FreeRTOS has no vTaskListTasks
    printf("name          state prio stack-free(words) num\n%s", buf);
    return 0;
}

static int c_zb(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "tx") == 0) {   // zb tx [dBm]: the 802.15.4 TX power, until the next restart
        int8_t v = 0, now = 0;
        if (argc >= 3) {
            int d;
            if (!parse_int(argv[2], -24, 20, &d)) {
                printf("zb tx: -24 to 20 dBm\n");
                return 1;
            }
            v = (int8_t)d;
        }
        if (!zb_tx_power(argc >= 3 ? &v : NULL, &now)) {
            printf("zb tx: Zigbee is not up\n");
            return 1;
        }
        printf("zb tx: %d dBm%s\n", now, argc >= 3 ? " (until the next restart)" : "");
        return 0;
    }
    zb_print();
    return 0;
}

static int c_join(int argc, char **argv)
{
    int s = 180;
    if (argc > 1 && !parse_int(argv[1], 0, 254, &s)) {
        printf("join [0..254] [foreign]\n");
        return 1;
    }
    bool own = !(argc > 2 && strcmp(argv[2], "foreign") == 0);   // foreign: test of the path that closes it again
    if (own) {   // by hand: whoever joins meanwhile stays (pair.c asks any other device that joins to leave)
        pair_manual(s);
    }
    esp_err_t e = zb_permit_join_as((uint8_t)s, own);
    printf("join %d s%s: %s\n", s, own ? "" : ", as if a device on the network had asked", esp_err_to_name(e));
    return 0;
}

static int c_zbreset(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "yes") != 0) {
        printf("zbreset yes: erases the Zigbee network (all paired devices) and restarts\n");
        return 1;
    }
    if (!zb_factory_reset()) {
        printf("zbreset: not on this board: the Zigbee data shares nvs with the recovery system (board.h)\n");
        return 1;
    }
    return 0;
}

static bool parse_u16(const char *s, uint16_t *v)
{
    char *e;
    unsigned long x = strtoul(s, &e, 0);
    if (e == s || *e || x > 0xffff) {
        return false;
    }
    *v = (uint16_t)x;
    return true;
}

static int ep_arg(int argc, char **argv, int i)   // the endpoint argument, 1 if there is none; -1: not an endpoint
{
    int ep = 1;   // IKEA lights answer on endpoint 1
    return argc > i && !parse_int(argv[i], 1, 240, &ep) ? -1 : ep;
}

// on|off|toggle ADDR [ep]
static int c_switch(int argc, char **argv)
{
    uint16_t a;
    if (argc < 2 || !parse_u16(argv[1], &a)) {
        printf("%s <short address> [ep], e.g. %s 0xec4d\n", argv[0], argv[0]);
        return 1;
    }
    int what = strcmp(argv[0], "off") == 0 ? 0 : strcmp(argv[0], "on") == 0 ? 1 : 2;
    int ep = ep_arg(argc, argv, 2);
    if (ep < 0) {
        printf("%s: endpoint 1..240\n", argv[0]);
        return 1;
    }
    printf("%s 0x%04x ep %d: %s\n", argv[0], a, ep, esp_err_to_name(zb_onoff(a, (uint8_t)ep, what)));
    return 0;
}

// move ADDR up|down|stop [rate] [onoff]: Level Move or Stop, with On/Off if the last word is onoff (bench tests)
static int c_move(int argc, char **argv)
{
    uint16_t a;
    int rate = 50;
    bool onoff = argc > 3 && strcmp(argv[argc - 1], "onoff") == 0;
    int n = onoff ? argc - 1 : argc;
    bool stop = n > 2 && strcmp(argv[2], "stop") == 0, up = n > 2 && strcmp(argv[2], "up") == 0;
    if (n < 3 || !parse_u16(argv[1], &a) || !(stop || up || strcmp(argv[2], "down") == 0) ||
        (n > 3 && !parse_int(argv[3], 1, 254, &rate))) {
        printf("move <short address> up|down|stop [rate 1..254, default 50] [onoff]\n");
        return 1;
    }
    esp_err_t e = stop ? zb_level_stop(a, 1, onoff) : zb_level_move(a, 1, up, (uint8_t)rate, onoff);
    printf("move 0x%04x %s%s: %s\n", a, argv[2], onoff ? " with On/Off" : "", esp_err_to_name(e));
    return 0;
}

// level ADDR L [tenths] [ep]: Move to Level with On/Off; mtl: the same without On/Off
static int c_level(int argc, char **argv)
{
    uint16_t a;
    int l, t = 5, ep = ep_arg(argc, argv, 4);
    bool onoff = strcmp(argv[0], "mtl") != 0;
    if (argc < 3 || !parse_u16(argv[1], &a) || !parse_int(argv[2], 0, 254, &l) ||
        (argc > 3 && !parse_int(argv[3], 0, 0xfffe, &t)) || ep < 0) {
        printf("%s <short address> <0..254> [transition in 1/10 s, default 5] [ep]\n", argv[0]);
        return 1;
    }
    printf("%s 0x%04x ep %d -> %d in %d/10 s: %s\n", argv[0], a, ep, l, t,
           esp_err_to_name(zb_level(a, (uint8_t)ep, (uint8_t)l, (uint16_t)t, onoff)));
    return 0;
}

// rd ADDR CLUSTER ATTR[,ATTR...] [ep]
static int c_rd(int argc, char **argv)
{
    uint16_t a, cl, ids[8];
    int n = 0;
    if (argc < 4 || !parse_u16(argv[1], &a) || !parse_u16(argv[2], &cl)) {
        printf("rd <short address> <cluster> <attr>[,<attr>...] [ep], e.g. rd 0xec4d 0x0008 0x0000\n");
        return 1;
    }
    char *save = NULL;
    for (char *t = strtok_r(argv[3], ",", &save); t && n < 8; t = strtok_r(NULL, ",", &save)) {
        if (!parse_u16(t, &ids[n])) {
            printf("rd: bad attribute id '%s'\n", t);
            return 1;
        }
        n++;
    }
    int ep = ep_arg(argc, argv, 4);
    if (ep < 0) {
        printf("rd: endpoint 1..240\n");
        return 1;
    }
    printf("rd 0x%04x ep %d cluster 0x%04x, %d attribute(s): %s\n", a, ep, cl, n,
           esp_err_to_name(zb_read(a, (uint8_t)ep, cl, ids, n)));
    return 0;
}

// basic ADDR [ep]: manufacturer name, model identifier, date code, software build
static int c_basic(int argc, char **argv)
{
    static const uint16_t ids[] = {0x0004, 0x0005, 0x0006, 0x4000};
    uint16_t a;
    if (argc < 2 || !parse_u16(argv[1], &a)) {
        printf("basic <short address> [ep]\n");
        return 1;
    }
    int ep = ep_arg(argc, argv, 2);
    if (ep < 0) {
        printf("basic: endpoint 1..240\n");
        return 1;
    }
    printf("basic 0x%04x ep %d: %s\n", a, ep, esp_err_to_name(zb_read(a, (uint8_t)ep, 0x0000, ids, 4)));
    return 0;
}

// zlink INST ADDR [ep] [class]: Berry instance INST (100..) of class 0 (dimmable) or 1 (white spectrum) <-> device
static int c_zlink(int argc, char **argv)
{
    uint16_t a;
    int inst, k = 0, ep = ep_arg(argc, argv, 3);
    if (argc < 3 || !parse_u16(argv[2], &a) || !parse_int(argv[1], 100, 32767, &inst) || ep < 0 ||
        (argc > 4 && !parse_int(argv[4], 0, 1, &k))) {
        printf("zlink <inst 100..> <short address> [ep] [class 0 dimmable | 1 white spectrum]\n");
        return 1;
    }
    int r = zlink_add(inst, k, a, (uint8_t)ep);
    printf("zlink inst %d class %d <-> 0x%04x ep %d: %s\n", inst, k, a, ep,
           r == 0 ? "ok" : r == -1 ? "inst must be 100.., class 0 or 1" : r == -2 ? "already linked" : "table full");
    return r == 0 ? 0 : 1;
}

// kw INST KO HEX: as if KNX had written group object KO of instance INST
static int c_kw(int argc, char **argv)
{
    int inst, ko;
    if (argc != 4 || strlen(argv[3]) % 2 || strlen(argv[3]) == 0 || strlen(argv[3]) > 28 ||
        strspn(argv[3], "0123456789abcdefABCDEF") != strlen(argv[3]) || !parse_int(argv[1], 1, 32767, &inst) ||
        !parse_int(argv[2], 1, 255, &ko)) {
        printf("kw <inst> <ko> <hex value>, e.g. kw 100 1 01 (switch on), kw 100 3 40 (25 %%)\n");
        return 1;
    }
    printf("kw inst %d ko %d = %s: %s\n", inst, ko, argv[3], esp_err_to_name(zlink_knx_write(inst, ko, argv[3])));
    return 0;
}

static int c_links(int argc, char **argv)
{
    zlink_print();
    return 0;
}

// poll S: read linked devices back every S seconds (0 stops)
static int c_poll(int argc, char **argv)
{
    int s = 0;
    if (argc > 1 && !parse_int(argv[1], 0, 86400, &s)) {
        printf("poll <0..86400 s>\n");
        return 1;
    }
    zlink_poll(s);
    zlink_print();
    return 0;
}

static int c_knx(int argc, char **argv)
{
    knx_link_print();
    knx_link_ack_print();
    return 0;
}

static bool parse_ia(const char *s, uint16_t *ia)
{
    unsigned a, b, c;
    if (sscanf(s, "%u.%u.%u", &a, &b, &c) != 3 || a > 15 || b > 15 || c > 255) {
        return false;
    }
    *ia = (uint16_t)(a << 12 | b << 8 | c);
    return true;
}

// the addresses the link acknowledges come from the device server (vdev.c)
static int c_ack(int argc, char **argv)
{
    knx_link_ack_print();
    return 0;
}

// vdev | vdev add ADDR [ep] [class] | vdev add none | vdev del N yes
static int c_vdev(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "add") == 0) {
        uint16_t a = 0xFFFF;
        if (strcmp(argv[2], "none") != 0 && !parse_u16(argv[2], &a)) {
            printf("vdev add <Zigbee short address | none> [ep] [class 0 dimmable | 1 white spectrum]\n");
            return 1;
        }
        int ep = ep_arg(argc, argv, 3), k = 0;
        if (ep < 0 || (argc > 4 && !parse_int(argv[4], 0, 1, &k))) {
            printf("vdev add <Zigbee short address | none> [ep] [class 0 dimmable | 1 white spectrum]\n");
            return 1;
        }
        int n = vdev_add(a, (uint8_t)ep, k, NULL);
        printf("vdev add: %s\n", n < 0 ? "no free slot" : "ok");
        if (n < 0) {
            return 1;
        }
    } else if (argc == 4 && strcmp(argv[1], "del") == 0 && strcmp(argv[3], "yes") == 0) {
        // The Zigbee device leaves the network as well: otherwise it stays a member nobody serves, and the trust
        // centre keeps its key (a lamp that was only reset could not come back).
        int n;
        if (!parse_int(argv[2], 0, KNX_VDEV_MAX - 1, &n)) {
            printf("vdev del <0..%d> yes\n", KNX_VDEV_MAX - 1);
            return 1;
        }
        uint16_t za;
        uint8_t ep, ieee[8];
        if (vdev_zigbee(n, &za, &ep) && !vdev_is_gone(n)) {
            printf("vdev del %d: Zigbee device 0x%04x asked to leave: %s\n", n, za, esp_err_to_name(zb_leave(za, NULL)));
            if (vdev_ieee(n, ieee)) {
                pair_removed(ieee);   // if the request does not reach it: asked again when it shows up
            }
        }
        printf("vdev del %s: %s\n", argv[2], vdev_del(n) ? "removed" : "no such device");
    } else if (argc != 1) {
        printf("vdev | vdev add <short address | none> [ep] [class] | vdev del <n> yes\n");
        return 1;
    }
    vdev_print();
    return 0;
}

// prog N|gw on|off: the programming button of virtual device N, or of the gateway itself
static int c_prog(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[2], "on") != 0 && strcmp(argv[2], "off") != 0)) {
        printf("prog <n>|gw on|off\n");
        return 1;
    }
    bool on = strcmp(argv[2], "on") == 0;
    int n = VDEV_GW;
    if (strcmp(argv[1], "gw") != 0 && !parse_int(argv[1], 0, KNX_VDEV_MAX - 1, &n)) {
        printf("prog <0..%d>|gw on|off\n", KNX_VDEV_MAX - 1);
        return 1;
    }
    printf("prog %s %s: %s\n", argv[1], argv[2], vdev_prog(n, on) ? "ok" : "no such device");
    return 0;
}

// gw: the gateway's own device, its parameters and its programming mode
static int c_gw(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    gw_print();
    return 0;
}

// reint <n>: ask the Zigbee device behind virtual device n everything again (endpoints, model, state, binding,
// reporting); the console shows its endpoints and clusters
static int c_reint(int argc, char **argv)
{
    int n;
    if (argc != 2 || !parse_int(argv[1], 0, KNX_VDEV_MAX - 1, &n)) {
        printf("reint <n>: re-interview the Zigbee device of virtual device n\n");
        return 1;
    }
    printf("reint %d: %s\n", n, zlink_reinterview(100 + n) ? "asked" : "no linked device");
    return 0;
}

// apdu [n]: PID 56 (max APDU length) of every virtual device and the gateway. The gateway's parameter LongFrames sets it
// at every start and every restart of the gateway's device (15 standard frames, ETS writes 12 octets at a time; 55 or
// 66 long frames, up to 63 octets in one extended frame); this overrides it until then.
static int c_apdu(int argc, char **argv)
{
    int n = 0;
    if (argc > 2 || (argc == 2 && !parse_int(argv[1], 15, KNX_TPDU_MAX - 1, &n))) {
        printf("apdu [15..%d]: PID 56 of every device; above 15 memory services come in extended frames\n",
               KNX_TPDU_MAX - 1);
        return 1;
    }
    printf("apdu: PID 56 = %d\n", vdev_max_apdu(n));
    return 0;
}

// tplfail <code>: the next check at LoadCompleted fails with this error code (DPT 20.011), as a template the gateway
// cannot take would; for testing what ETS shows then. 0 cancels.
static int c_tplfail(int argc, char **argv)
{
    int e = -1;
    if (argc != 2 || !parse_int(argv[1], 0, 18, &e)) {
        printf("tplfail <0..18>: error code of DPT 20.011 for the next LoadCompleted of an application, 0 cancels\n");
        return 1;
    }
    vdev_test_refuse((uint8_t)e);
    printf("tplfail %d: %s\n", e, e ? "the next LoadCompleted of an application ends in Error" : "cancelled");
    return 0;
}

// ia N a.b.c: set the individual address by hand, only inside 15.15.1 .. 15.15.48 (or 15.15.255)
static int c_ia(int argc, char **argv)
{
    uint16_t ia;
    int n;
    if (argc != 3 || !parse_int(argv[1], 0, KNX_VDEV_MAX, &n) || !parse_ia(argv[2], &ia) ||
        ((ia < 0xFF01 || ia > 0xFF30) && ia != 0xFFFF)) {
        printf("ia <0..%d> a.b.c: 15.15.1 .. 15.15.48 (the test range of this command) or 15.15.255 (unprogrammed); "
               "%d is the gateway\n", KNX_VDEV_MAX, KNX_VDEV_MAX);
        return 1;
    }
    printf("ia %s %s: %s\n", argv[1], argv[2],
           vdev_set_ia(n, ia) ? "ok" : "no such device, or another device has that address");
    return 0;
}

// pair | pair on|off | pair range a.b.c a.b.c: binding a Zigbee device when ETS asks for the programming button
static int c_pair(int argc, char **argv)
{
    uint16_t lo, hi;
    if (argc == 2 && (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0)) {
        pair_enable(strcmp(argv[1], "on") == 0);
    } else if (argc == 4 && strcmp(argv[1], "range") == 0 && parse_ia(argv[2], &lo) && parse_ia(argv[3], &hi)) {
        int r = pair_range(lo, hi);
        if (r) {
            printf("pair range: %s\n", r == 1 ? "first address above the last one"
                                              : "0.0.0 and 15.15.255 are not addresses of a device");
            return 1;
        }
    } else if (argc == 3 && strcmp(argv[1], "window") == 0) {   // as the gateway's object or button would
        int s;
        if (!parse_int(argv[2], 0, 254, &s)) {
            printf("pair window <0..254 s> (0 closes)\n");
            return 1;
        }
        pair_window(s);
        vTaskDelay(pdMS_TO_TICKS(200));
    } else if (argc != 1) {
        printf("pair | pair on|off | pair range a.b.c a.b.c | pair window <s> (0 closes)\n");
        return 1;
    }
    pair_print();
    return 0;
}

// leave ADDR yes: ask a Zigbee device to leave the network (ZDO Mgmt_Leave, no rejoin)
static int c_leave(int argc, char **argv)
{
    uint16_t a;
    if (argc != 3 || !parse_u16(argv[1], &a) || strcmp(argv[2], "yes") != 0) {
        printf("leave <short address> yes\n");
        return 1;
    }
    printf("leave 0x%04x: %s\n", a, esp_err_to_name(zb_leave(a, NULL)));
    return 0;
}

// dump N OBJ: segment of load object OBJ (1 address table .. 4 parameters) of virtual device N
static int c_dump(int argc, char **argv)
{
    int n, obj;
    if (argc != 3 || !parse_int(argv[1], 0, KNX_VDEV_MAX, &n) || !parse_int(argv[2], 1, 5, &obj)) {
        printf("dump <0..%d> <object 1..5> (%d is the gateway)\n", KNX_VDEV_MAX, KNX_VDEV_MAX);
        return 1;
    }
    vdev_dump(n, obj);
    return 0;
}

static int c_gc(int argc, char **argv)
{
    bvm_gc();
    app_heap("gc");
    return 0;
}

#if CONSOLE_BENCH
static int c_dev(int argc, char **argv)
{
    int n;
    if (argc != 2 || !parse_int(argv[1], 0, 64, &n)) {
        printf("dev N: N virtual devices (device-server records + Berry instances, alternating classes)\n");
        return 1;
    }
    int d = devsrv_resize(n);
    int b = bvm_populate(n);
    printf("dev: %d device records, %d Berry instances\n", d, b);
    app_heap("dev");
    return 0;
}

static int c_sim(int argc, char **argv)
{
    if (argc == 2) {
        s_sim = strcmp(argv[1], "on") == 0;
    }
    printf("sim %s\n", s_sim ? "on" : "off");
    return 0;
}

static int c_gcrate(int argc, char **argv)
{
    int pct = 0;
    if (argc > 1 && !parse_int(argv[1], 0, 1000, &pct)) {
        printf("gcrate 101..355\n");
        return 1;
    }
    bvm_set_rate(pct);
    return 0;
}

static void sim_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_sim) {
            bvm_sim_step();
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
#endif

static int c_stat(int argc, char **argv)
{
    app_heap("stat");
    zb_print();
    knx_link_print();
    knx_link_ack_print();
    vdev_print();
    devsrv_print();
    bvm_print();
    zlink_print();
    pair_print();
    gw_print();
    return 0;
}

static void reg(const char *name, const char *help, esp_console_cmd_func_t f)
{
    const esp_console_cmd_t c = {.command = name, .help = help, .func = f};
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}

void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "z2k> ";
    rc.max_cmdline_length = 128;
    rc.task_stack_size = 4096;
#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    // TULX32: UART0 on the service header (board.h)
    esp_console_dev_uart_config_t hw = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw, &rc, &repl));
#else
    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &rc, &repl));
#endif
    reg("heap", "heap now / minimum / largest block", c_heap);
    reg("tasks", "FreeRTOS tasks with free stack", c_tasks);
    reg("zb", "Zigbee status; zb tx [dBm]: TX power", c_zb);
    reg("join", "join [s] [foreign]: permit joining for s seconds (default 180, 0 closes); foreign: as a device would", c_join);
    reg("zbreset", "zbreset yes: erase the Zigbee network and restart", c_zbreset);
    reg("on", "on <short address> [ep]: ZCL On", c_switch);
    reg("off", "off <short address> [ep]: ZCL Off", c_switch);
    reg("toggle", "toggle <short address> [ep]: ZCL Toggle", c_switch);
    reg("level", "level <short address> <0..254> [1/10 s] [ep]: Move to Level with On/Off", c_level);
    reg("move", "move <short address> up|down|stop [rate] [onoff]: Level Move or Stop, with On/Off if onoff", c_move);
    reg("mtl", "mtl <short address> <0..254> [1/10 s] [ep]: Move to Level without On/Off, also while off (ExecuteIfOff)", c_level);
    reg("rd", "rd <short address> <cluster> <attr>[,attr...] [ep]: read attributes", c_rd);
    reg("basic", "basic <short address> [ep]: manufacturer, model, date code, sw build", c_basic);
    reg("zlink", "zlink <inst 100..> <short address> [ep] [class 0|1]: link a Berry instance to a device", c_zlink);
    reg("kw", "kw <inst> <ko> <hex>: KNX write into group object ko of an instance", c_kw);
    reg("links", "linked instances", c_links);
    reg("poll", "poll S: read linked devices back every S seconds, 0 stops", c_poll);
    reg("knx", "NCN5130 link statistics", c_knx);
    reg("ack", "addresses the link acknowledges", c_ack);
    reg("vdev", "vdev | vdev add <short address|none> [ep] [class] | vdev del <n> yes: KNX devices on the bus", c_vdev);
    reg("prog", "prog <n>|gw on|off: programming mode of virtual device n or of the gateway", c_prog);
    reg("gw", "the gateway's own KNX device: parameters, programming mode, what it refused last", c_gw);
    reg("apdu", "apdu [15..66]: PID 56 (max APDU length) of every device until the gateway's next restart; above 15 "
                "long frames", c_apdu);
    reg("reint", "reint <n>: re-interview the Zigbee device of virtual device n (endpoints, model, state, binding, "
                 "reporting)",
        c_reint);
    reg("tplfail", "tplfail <code>: the next LoadCompleted of an application ends in Error with this code (test)",
        c_tplfail);
    reg("ia", "ia <n> a.b.c: individual address of virtual device n by hand", c_ia);
    reg("dump", "dump <n> <object 1..5>: segment of a load object", c_dump);
    reg("pair", "pair | pair on|off | pair range a.b.c a.b.c | pair window <s>: bind a Zigbee device", c_pair);
    reg("leave", "leave <short address> yes: ask a Zigbee device to leave the network", c_leave);
    reg("stat", "everything", c_stat);
    reg("gc", "collect now in every Berry VM", c_gc);
#if CONSOLE_BENCH
    reg("dev", "dev N: N simulated devices (RAM records + Berry instances, no bus)", c_dev);
    reg("sim", "sim on|off: simulated traffic every 2 s", c_sim);
    reg("gcrate", "gcrate N: Berry GC step rate in percent (120 here, 200 = Berry's own default)", c_gcrate);
#endif
    esp_console_register_help_command();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
#if CONSOLE_BENCH
    xTaskCreate(sim_task, "sim", 3072, NULL, 3, NULL);
#endif
}
