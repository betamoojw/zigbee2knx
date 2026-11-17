#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
// dump_z2m.cjs — distil the zigbee-herdsman-converters device definitions into plain JSON, one line per
// definition, as the input of the Zigbee-to-KNX mapping.
//
//   node dump_z2m.cjs <install-dir> <out.jsonl>
//
// <install-dir> holds node_modules/zigbee-herdsman-converters (npm package, MIT, (c) Koen Kanters).
// Only facts are kept: identity, exposes (type, property, access, unit, range, enum values, features,
// category), options, toZigbee keys, fromZigbee clusters, OTA flag. No converter source is copied.
// Access bits (src/lib/exposes.ts): 1 = published in state, 2 = settable, 4 = gettable.
"use strict";
const fs = require("fs");
const path = require("path");
const {createRequire} = require("module");

const [base, out] = process.argv.slice(2);
if (!base || !out) {
    console.error("usage: node dump_z2m.cjs <install-dir> <out.jsonl>");
    process.exit(2);
}
const req = createRequire(path.join(path.resolve(base), "package.json"));
const zhc = req("zigbee-herdsman-converters");
const all = req("zigbee-herdsman-converters/devices/index").default;
const pkg = req("zigbee-herdsman-converters/package.json");

const KEEP = [
    "type", "name", "label", "property", "access", "unit", "value_min", "value_max", "value_step",
    "values", "value_on", "value_off", "value_toggle", "presets", "category", "description", "endpoint",
    "length_min", "length_max",
];

function expose(e) {
    const o = {};
    for (const k of KEEP) if (e[k] !== undefined) o[k] = e[k];
    if (Array.isArray(e.features)) o.features = e.features.map(expose);
    if (e.item_type && typeof e.item_type === "object") o.item_type = expose(e.item_type);
    return o;
}

// meta flags that change device behaviour (e.g. turnsOffAtBrightness1); only plain values, no functions
function plainMeta(meta) {
    const o = {};
    for (const [k, v] of Object.entries(meta || {})) {
        if (["boolean", "number", "string"].includes(typeof v)) o[k] = v;
        else if (Array.isArray(v) && v.every((x) => ["boolean", "number", "string"].includes(typeof x))) o[k] = v;
    }
    return o;
}

const lines = [];
let written = 0;
let failed = 0;
for (const raw of all) {
    let d;
    try {
        d = zhc.prepareDefinition(raw);
    } catch (err) {
        failed++;
        lines.push(JSON.stringify({model: raw.model, vendor: raw.vendor, error: `prepare: ${err.message}`}));
        continue;
    }
    let exposes = d.exposes;
    let note;
    if (typeof exposes === "function") {
        // The library itself calls such functions with a stand-in device ({isDummyDevice: true}, index.js of
        // 26.57.0) to list every expose a model can have; without one about 70 definitions (IKEA TRETAKT among them)
        // came out empty.
        const fn = exposes;
        try {
            exposes = fn({isDummyDevice: true}, {});
            note = "function, called with a stand-in device";
        } catch (err) {
            try {
                exposes = fn(undefined, {});
                note = "function, called without device";
            } catch (err2) {
                exposes = [];
                note = `function failed: ${err.message} / without device: ${err2.message}`;
            }
        }
    }
    lines.push(JSON.stringify({
        model: d.model,
        vendor: d.vendor,
        description: d.description,
        zigbeeModel: d.zigbeeModel || [],
        fingerprint: (d.fingerprint || []).map((f) => ({modelID: f.modelID, manufacturerName: f.manufacturerName})),
        whiteLabel: (d.whiteLabel || []).map((w) => ({vendor: w.vendor, model: w.model})),
        exposes: (exposes || []).map(expose),
        exposes_note: note,
        options: (d.options || []).map(expose),
        toZigbee_keys: [...new Set((d.toZigbee || []).flatMap((c) => c.key || []))].sort(),
        fromZigbee_clusters: [...new Set((d.fromZigbee || []).map((c) => String(c.cluster)))].sort(),
        ota: Boolean(d.ota),
        meta: plainMeta(d.meta),
    }));
    written++;
}
fs.writeFileSync(`${out}.tmp`, `${lines.join("\n")}\n`);
fs.renameSync(`${out}.tmp`, out);
console.log(JSON.stringify({package: pkg.name, version: pkg.version, definitions: all.length, written, failed}));
