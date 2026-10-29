// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
//
// FM-1 .fwsc packages in the browser: the package identity and the image the
// device reads during an update.

const BLOCKS = 20, BLK = 0x30, KEEP = 0x2F;

// the package identity ("FM-1_904"): one marker byte after each of the first 20 blocks
export function productOf(fwsc) {
  if (fwsc.length < BLOCKS * BLK) throw new Error("not an FM-1 package (too short)");
  let s = "";
  for (let i = 0; i < BLOCKS; i++) {
    const m = fwsc[i * BLK + KEEP];
    if (m !== 0x7D) s += String.fromCharCode((m - i - 1) & 0xFF);
  }
  return s;
}

// the image the device addresses during an update: the .fwsc without the 20 marker bytes
export function logicalImage(fwsc) {
  const out = new Uint8Array(fwsc.length - BLOCKS);
  for (let i = 0; i < BLOCKS; i++) out.set(fwsc.subarray(i * BLK, i * BLK + KEEP), i * KEEP);
  out.set(fwsc.subarray(BLOCKS * BLK), BLOCKS * KEEP);
  return out;
}

// The official FM-1 V15 (M-VAVE Downloads -> PC Firmware -> FM-1 V15, FM-1.fwsc), for the return to the
// official firmware: only that exact file is accepted (its SHA-256 pins every byte: the loader, the app, the
// layout; the name of the file proves nothing). The digest is the one Felucca 1.0 pins.
export const STOCK_V15_SHA256 = "db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a";
export const STOCK_V15_SIZE = 699956;
export const STOCK_V15_PRODUCT = "FM-1_015";

// The official FM-1 V16 (2026-10-21; since then the only one M-VAVE offers): the same checks (after Clipper 1.2:
// only V15 was accepted, so the way back failed with the file M-VAVE now gives; sloop-fm1 #8). The firmware's own
// update entry (ota.c) still checks the package's loader (the official one's CRCs) and that its head equals the FM-1's
export const STOCK_V16_SHA256 = "d017fb65e70aca97ee29d944470c3515a74581df5c0576bf70de917c7b2b61d5";
export const STOCK_V16_SIZE = 718196;
export const STOCK_V16_PRODUCT = "FM-1_016";
// the official FM-1 packages accepted for the way back (the file M-VAVE offers, or the one before)
export const STOCK_FM1 = [
  { version: "V16", size: STOCK_V16_SIZE, sha256: STOCK_V16_SHA256, product: STOCK_V16_PRODUCT },
  { version: "V15", size: STOCK_V15_SIZE, sha256: STOCK_V15_SHA256, product: STOCK_V15_PRODUCT },
];
// -> the FM-1 package it is (STOCK_FM1), or null
export async function knownStockFm1(bytes) {
  const c = STOCK_FM1.filter((k) => k.size === bytes.length);
  if (!c.length) return null;
  const sha = await sha256hex(bytes);
  return c.find((k) => k.sha256 === sha && productOf(bytes) === k.product) || null;
}

export async function sha256hex(bytes) {
  const d = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(d)].map((x) => x.toString(16).padStart(2, "0")).join("");
}

export async function validateStockPackage(bytes) {
  const k = await knownStockFm1(bytes);
  if (!k)
    throw new Error("select the unmodified official FM-1 file (FM-1.fwsc, V16 or V15)");
  return { product: k.product, version: k.version, image: logicalImage(bytes) };
}
