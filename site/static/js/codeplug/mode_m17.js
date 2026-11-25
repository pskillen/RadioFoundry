// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 conventional (kind 6): channel parameters and the radio-wide callsign (docs/spec/m17-codeplug.md).

import { u8, grow } from "./modebytes.js";

const CHARS = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/.";
const ALL = 0xFFFFFFFFFFFFn, RESERVED = 0xEE6B28000000n;
const CAN_MAX = 15, NO_CAN_SQUELCH = 0x10;
const CALL_BYTES = 10, CHAN_BYTES = 7;

// Fold only the ASCII letters in the wire alphabet.
const upper = (text) => text.replace(/[a-z]/g, (c) => c.toUpperCase());
const dropTrailingSpaces = (text) => text.replace(/ +$/, "");

export function addrOf(text) {
	const dst = upper(dropTrailingSpaces(text));
	if (dst === "" || dst === "ALL" || dst === "@ALL") return ALL;
	if (dst.length > 9 || dst.startsWith(" ")) return null;
	let addr = 0n;
	for (let i = dst.length - 1; i >= 0; i--) {
		const digit = CHARS.indexOf(dst[i]);
		if (digit < 0) return null;
		addr = addr * 40n + BigInt(digit);
	}
	return addr;
}

function textOf(addr) {
	if (addr >= RESERVED) return "";
	let text = "";
	while (addr > 0n) {
		text += CHARS[Number(addr % 40n)];
		addr /= 40n;
	}
	return text;
}

function callOf(text) {
	const call = upper(text);
	return call.length >= 1 && call.length <= 9 && [...call].every((c) => CHARS.indexOf(c) > 0) ? call : null;
}

export function decodeChan(b) {
	const flags = u8(b, 0);
	let addr = 0n;
	if (b?.length >= CHAN_BYTES) {
		for (let i = 1; i < CHAN_BYTES; i++) addr = (addr << 8n) | BigInt(b[i]);
	}
	return { can: flags & CAN_MAX, canSquelch: (flags & NO_CAN_SQUELCH) === 0, dst: textOf(addr) };
}

export function encodeChan(p, prev) {
	const v = grow(prev, CHAN_BYTES);
	const can = p.can >= 0 && p.can <= CAN_MAX ? p.can : 0;
	v[0] = (v[0] & 0xE0) | can | (p.canSquelch ? 0 : NO_CAN_SQUELCH);
	let addr = addrOf(p.dst ?? "");
	if (addr === ALL || addr === null) addr = 0n;
	for (let i = CHAN_BYTES - 1; i > 0; i--) {
		v[i] = Number(addr & 0xFFn);
		addr >>= 8n;
	}
	return v;
}

export function decodeCall(b) {
	let text = "";
	for (let i = 0; i < CALL_BYTES; i++) {
		const byte = u8(b, i);
		if (byte === 0 || byte === 0xFF) break;
		text += String.fromCharCode(byte);
	}
	return { callsign: callOf(dropTrailingSpaces(text)) ?? "" };
}

function encodeCall(s, prev) {
	if (s.callsign === "") return null;
	const call = callOf(s.callsign ?? "");
	if (call === null) return prev ?? null;
	const v = grow(prev, CALL_BYTES);
	v.fill(0, 0, CALL_BYTES);
	for (let i = 0; i < call.length; i++) v[i] = call.charCodeAt(i);
	return v;
}

export const m17Kind = {
	id: 6,
	feature: "M17",
	systemType: null,
	contacts: false,
	ownFreqs: true,
	defaultTx: true,
	recordFields: [],
	flags: {},
	newParams: () => ({ can: 0, canSquelch: true, dst: "" }),
	decode: decodeChan,
	encode: encodeChan,
	fields: () => [
		{ key: "can", label: "editor.m17.can", type: "int", min: 0, max: CAN_MAX, blank: 0, help: "editor.m17.can_help" },
		{ key: "canSquelch", label: "editor.m17.can_squelch", type: "bool" },
		{ key: "dst", label: "editor.m17.dst", type: "text", help: "editor.m17.dst_help" },
	],
	validate(c, p, problem) {
		if (addrOf(p?.dst ?? "") === null) problem.err("dst", "m17_dest");
	},
	settings: {
		key: "m17",
		decode: decodeCall,
		encode: encodeCall,
		fields: [{ key: "callsign", label: "editor.m17.callsign", type: "text", help: "editor.m17.callsign_help" }],
		validate(s, problem) {
			if (s.callsign && callOf(s.callsign) === null) problem.err("callsign", "m17_callsign");
		},
	},
};
