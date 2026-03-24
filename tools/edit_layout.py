#!/usr/bin/env python3
"""
Interactive editor for AppleALC pin config layouts.

Read a layout by codec+layout ID, display pins grouped by association,
move pins between associations, create new associations, and save the
result as a new layout back into the PinConfigs plist.

Usage:
    python3 tools/edit_layout.py <path-to-PinConfigs-Info.plist>

Example:
    python3 tools/edit_layout.py \
        ../AppleALC/Resources/PinConfigs.kext/Contents/Info.plist
"""

import plistlib
import struct
import sys
import copy
from collections import defaultdict

# ── Pin config field decoders ──────────────────────────────────────────

DEVICE_NAMES = {
    0x0: "Line Out",   0x1: "Speaker",     0x2: "HP Out",
    0x3: "CD",         0x4: "SPDIF Out",   0x5: "Digital Out",
    0x6: "Modem Line", 0x7: "Modem Handset",
    0x8: "Line In",    0x9: "Aux",         0xA: "Mic In",
    0xB: "Telephony",  0xC: "SPDIF In",    0xD: "Digital In",
    0xE: "Reserved",   0xF: "Other",
}

CONNECTIVITY = {0: "Jack", 1: "N/C", 2: "Fixed", 3: "Both"}

LOCATION_GROSS = {0: "Ext", 1: "Int", 2: "Sep", 3: "Other"}
LOCATION_FINE = {
    0x00: "N/A",   0x01: "Rear",   0x02: "Front", 0x03: "Left",
    0x04: "Right", 0x05: "Top",    0x06: "Bottom", 0x07: "Rear(S)",
    0x08: "Drive", 0x09: "Riser",  0x0A: "HDMI",  0x0B: "ATAPI",
    0x10: "N/A",   0x17: "Mobile-Lid",
}

COLOR_NAMES = {
    0: "Unknown", 1: "Black", 2: "Grey", 3: "Blue", 4: "Green",
    5: "Red", 6: "Orange", 7: "Yellow", 8: "Purple", 9: "Pink",
    0xE: "White", 0xF: "Other",
}

CONN_TYPE = {
    0: "Unknown", 1: "1/8\"", 2: "1/4\"", 3: "ATAPI", 4: "RCA",
    5: "Optical", 6: "Digital", 7: "Analog", 8: "Multi", 9: "XLR",
    0xA: "RJ-11", 0xB: "Combo", 0xF: "Other",
}


def decode_pin(pc):
    """Decode 32-bit pin config into dict."""
    return {
        'connectivity': (pc >> 30) & 0x3,
        'location':     (pc >> 24) & 0x3F,
        'device':       (pc >> 20) & 0xF,
        'conn_type':    (pc >> 16) & 0xF,
        'color':        (pc >> 12) & 0xF,
        'misc':         (pc >> 8) & 0xF,
        'assoc':        (pc >> 4) & 0xF,
        'seq':          pc & 0xF,
    }


def encode_pin(d):
    """Encode dict back to 32-bit pin config."""
    return ((d['connectivity'] & 0x3) << 30 |
            (d['location'] & 0x3F) << 24 |
            (d['device'] & 0xF) << 20 |
            (d['conn_type'] & 0xF) << 16 |
            (d['color'] & 0xF) << 12 |
            (d['misc'] & 0xF) << 8 |
            (d['assoc'] & 0xF) << 4 |
            (d['seq'] & 0xF))


def pin_desc(pc):
    """Human-readable one-line description of a pin config value."""
    d = decode_pin(pc)
    dev = DEVICE_NAMES.get(d['device'], '?')
    conn = CONNECTIVITY.get(d['connectivity'], '?')
    loc_g = LOCATION_GROSS.get(d['location'] >> 4, '?')
    loc_f = LOCATION_FINE.get(d['location'] & 0x0F,
                              LOCATION_FINE.get(d['location'], '?'))
    color = COLOR_NAMES.get(d['color'], '?')
    ctype = CONN_TYPE.get(d['conn_type'], '?')
    return (f"{dev:14s} {conn:5s} {loc_g}/{loc_f:7s} "
            f"{color:7s} {ctype:7s} "
            f"misc={d['misc']} assoc={d['assoc']:2d} seq={d['seq']}")


# ── Plist parsing ──────────────────────────────────────────────────────

def parse_verbs(data):
    """Parse ConfigData into (pin_configs, extra_verbs)."""
    if not data or len(data) % 4 != 0:
        return [], []
    verbs_by_nid = defaultdict(dict)
    extra_verbs = []
    for i in range(0, len(data), 4):
        word = struct.unpack('>I', data[i:i + 4])[0]
        nid = (word >> 20) & 0xFF
        verb_id = (word >> 8) & 0xFFF
        param = word & 0xFF
        if 0x71C <= verb_id <= 0x71F:
            verbs_by_nid[nid][verb_id] = param
        else:
            extra_verbs.append(word & 0x0FFFFFFF)
    pin_configs = []
    for nid in sorted(verbs_by_nid):
        vmap = verbs_by_nid[nid]
        if len(vmap) == 4:
            pc = ((vmap[0x71F] << 24) | (vmap[0x71E] << 16) |
                  (vmap[0x71D] << 8) | vmap[0x71C])
            pin_configs.append((nid, pc))
    return pin_configs, extra_verbs


def encode_config_data(pins, extra_verbs, cad=0):
    """Encode pin configs + extra verbs back to ConfigData bytes."""
    words = []
    for nid, pc in pins:
        for byte_idx in range(4):
            verb = 0x71C + byte_idx
            param = (pc >> (byte_idx * 8)) & 0xFF
            word = ((cad & 0xF) << 28 | (nid & 0xFF) << 20 |
                    (verb & 0xFFF) << 8 | param)
            words.append(word)
    for v in extra_verbs:
        words.append((cad << 28) | (v & 0x0FFFFFFF))
    return b''.join(struct.pack('>I', w) for w in words)


def load_plist(path):
    with open(path, 'rb') as f:
        return plistlib.load(f)


def get_hda_config(plist):
    for key, val in plist.get('IOKitPersonalities', {}).items():
        if isinstance(val, dict) and 'HDAConfigDefault' in val:
            return val['HDAConfigDefault'], key
    return None, None


# ── Display helpers ────────────────────────────────────────────────────

def show_pins(pins, title=""):
    """Display pins grouped by association."""
    if title:
        print(f"\n{'=' * 72}")
        print(f"  {title}")
        print(f"{'=' * 72}")

    by_assoc = defaultdict(list)
    for nid, pc in pins:
        d = decode_pin(pc)
        by_assoc[d['assoc']].append((nid, pc, d))

    for assoc in sorted(by_assoc):
        label = "Disabled" if assoc == 0 else \
                "Reserved(15)" if assoc == 15 else f"Association {assoc}"
        print(f"\n  ── {label} ──")
        for nid, pc, d in sorted(by_assoc[assoc], key=lambda x: x[2]['seq']):
            print(f"    nid 0x{nid:02X}  0x{pc:08X}  {pin_desc(pc)}")
    print()


def list_codecs(hda_config):
    """List unique codec IDs."""
    codecs = sorted(set(item.get('CodecID', 0) for item in hda_config))
    print(f"\nAvailable codecs ({len(codecs)}):")
    for c in codecs:
        vendor = (c >> 16) & 0xFFFF
        device = c & 0xFFFF
        count = sum(1 for item in hda_config if item.get('CodecID') == c)
        print(f"  0x{c:08X}  (vendor 0x{vendor:04X} device 0x{device:04X})  "
              f"— {count} layout(s)")
    return codecs


def list_layouts(hda_config, codec_id):
    """List layouts for a codec."""
    layouts = sorted(item.get('LayoutID', 0) for item in hda_config
                     if item.get('CodecID') == codec_id)
    print(f"\nLayouts for codec 0x{codec_id:08X} ({len(layouts)}):")
    for lid in layouts:
        print(f"  {lid}")
    return layouts


def find_entry(hda_config, codec_id, layout_id):
    for item in hda_config:
        if item.get('CodecID') == codec_id and item.get('LayoutID') == layout_id:
            return item
    return None


# ── Interactive commands ───────────────────────────────────────────────

def cmd_move(pins, args):
    """move <nid> <new_assoc> [new_seq] — move pin to another association."""
    parts = args.split()
    if len(parts) < 2:
        print("Usage: move <nid> <new_assoc> [new_seq]")
        return pins
    try:
        nid = int(parts[0], 0)
        new_assoc = int(parts[1])
        new_seq = int(parts[2]) if len(parts) > 2 else None
    except ValueError:
        print("Error: invalid number")
        return pins

    if not 0 <= new_assoc <= 15:
        print("Error: assoc must be 0..15")
        return pins

    found = False
    result = []
    for n, pc in pins:
        if n == nid:
            d = decode_pin(pc)
            d['assoc'] = new_assoc
            if new_seq is not None:
                if not 0 <= new_seq <= 15:
                    print("Error: seq must be 0..15")
                    return pins
                d['seq'] = new_seq
            else:
                # Auto-assign next free seq in target association
                used = set()
                for n2, pc2 in pins:
                    d2 = decode_pin(pc2)
                    if d2['assoc'] == new_assoc and n2 != nid:
                        used.add(d2['seq'])
                for s in range(16):
                    if s not in used:
                        d['seq'] = s
                        break
            pc = encode_pin(d)
            found = True
        result.append((n, pc))

    if not found:
        print(f"Error: nid 0x{nid:02X} not found")
        return pins

    print(f"Moved nid 0x{nid:02X} → assoc {new_assoc}")
    return result


def cmd_set(pins, args):
    """set <nid> <field> <value> — set a pin config field."""
    fields = ['connectivity', 'location', 'device', 'conn_type',
              'color', 'misc', 'assoc', 'seq']
    parts = args.split()
    if len(parts) < 3:
        print(f"Usage: set <nid> <field> <value>")
        print(f"  Fields: {', '.join(fields)}")
        return pins
    try:
        nid = int(parts[0], 0)
        field = parts[1]
        value = int(parts[2], 0)
    except ValueError:
        print("Error: invalid number")
        return pins

    if field not in fields:
        print(f"Error: unknown field. Use: {', '.join(fields)}")
        return pins

    found = False
    result = []
    for n, pc in pins:
        if n == nid:
            d = decode_pin(pc)
            d[field] = value
            pc = encode_pin(d)
            found = True
        result.append((n, pc))

    if not found:
        print(f"Error: nid 0x{nid:02X} not found")
        return pins

    print(f"Set nid 0x{nid:02X} {field} = {value}")
    return result


def cmd_disable(pins, args):
    """disable <nid> — set pin to assoc 0 (disabled), connectivity=N/C."""
    try:
        nid = int(args.strip(), 0)
    except ValueError:
        print("Usage: disable <nid>")
        return pins

    result = []
    found = False
    for n, pc in pins:
        if n == nid:
            d = decode_pin(pc)
            d['assoc'] = 0
            d['seq'] = 0
            d['connectivity'] = 1  # No Physical Connection
            pc = encode_pin(d)
            found = True
        result.append((n, pc))

    if not found:
        print(f"Error: nid 0x{nid:02X} not found")
        return pins

    print(f"Disabled nid 0x{nid:02X}")
    return result


def cmd_swap(pins, args):
    """swap <nid1> <nid2> — swap associations+sequences of two pins."""
    parts = args.split()
    if len(parts) != 2:
        print("Usage: swap <nid1> <nid2>")
        return pins
    try:
        nid1 = int(parts[0], 0)
        nid2 = int(parts[1], 0)
    except ValueError:
        print("Error: invalid nid")
        return pins

    d1 = d2 = None
    for n, pc in pins:
        if n == nid1:
            d1 = decode_pin(pc)
        elif n == nid2:
            d2 = decode_pin(pc)

    if d1 is None or d2 is None:
        print("Error: one or both nids not found")
        return pins

    # Swap assoc + seq
    d1['assoc'], d2['assoc'] = d2['assoc'], d1['assoc']
    d1['seq'], d2['seq'] = d2['seq'], d1['seq']

    result = []
    for n, pc in pins:
        if n == nid1:
            d = decode_pin(pc)
            d['assoc'] = d1['assoc']
            d['seq'] = d1['seq']
            pc = encode_pin(d)
        elif n == nid2:
            d = decode_pin(pc)
            d['assoc'] = d2['assoc']
            d['seq'] = d2['seq']
            pc = encode_pin(d)
        result.append((n, pc))

    print(f"Swapped nid 0x{nid1:02X} ↔ 0x{nid2:02X}")
    return result


def cmd_raw(pins, args):
    """raw <nid> <hex32> — set raw 32-bit pin config value."""
    parts = args.split()
    if len(parts) != 2:
        print("Usage: raw <nid> <hex32>")
        return pins
    try:
        nid = int(parts[0], 0)
        value = int(parts[1], 0)
    except ValueError:
        print("Error: invalid number")
        return pins

    result = []
    found = False
    for n, pc in pins:
        if n == nid:
            pc = value & 0xFFFFFFFF
            found = True
        result.append((n, pc))

    if not found:
        print(f"Error: nid 0x{nid:02X} not found")
        return pins

    print(f"Set nid 0x{nid:02X} = 0x{value:08X}")
    return result


def print_help():
    print("""
Commands:
  show                    — display current pin layout
  move <nid> <assoc> [seq] — move pin to association (auto-seq if omitted)
  set <nid> <field> <val> — set field (assoc, seq, device, connectivity,
                             location, color, conn_type, misc)
  disable <nid>           — disable pin (assoc=0, connectivity=N/C)
  swap <nid1> <nid2>      — swap associations of two pins
  raw <nid> <hex32>       — set raw 32-bit pin config
  diff                    — show changes vs original
  save <layout_id>        — save as new layout in plist
  write <path>            — write modified plist to file
  quit                    — exit without saving
""")


# ── Main ───────────────────────────────────────────────────────────────

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <PinConfigs-Info.plist>")
        sys.exit(1)

    plist_path = sys.argv[1]
    plist = load_plist(plist_path)
    hda_config, personality_key = get_hda_config(plist)
    if not hda_config:
        print("Error: HDAConfigDefault not found in plist")
        sys.exit(1)

    # ── Select codec ──
    codecs = list_codecs(hda_config)
    codec_id = None
    while codec_id is None:
        s = input("\nCodec ID (hex, e.g. 0x10EC0897): ").strip()
        if not s:
            continue
        try:
            codec_id = int(s, 0)
        except ValueError:
            print("Invalid hex number")
        if codec_id not in set(item.get('CodecID', 0) for item in hda_config):
            print("Codec not found in plist")
            codec_id = None

    # ── Select layout ──
    layouts = list_layouts(hda_config, codec_id)
    layout_id = None
    while layout_id is None:
        s = input("\nLayout ID: ").strip()
        if not s:
            continue
        try:
            layout_id = int(s)
        except ValueError:
            print("Invalid number")
            continue
        if layout_id not in layouts:
            print("Layout not found")
            layout_id = None

    # ── Load layout ──
    entry = find_entry(hda_config, codec_id, layout_id)
    config_data = entry.get('ConfigData', b'')
    wake_data = entry.get('WakeConfigData', b'')
    wake_reinit = entry.get('WakeVerbReinit', False)

    pins, extra_verbs = parse_verbs(config_data)
    orig_pins = list(pins)  # save copy for diff

    show_pins(pins, f"Codec 0x{codec_id:08X}  Layout {layout_id}")
    print_help()

    # ── Edit loop ──
    while True:
        try:
            line = input("edit> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break

        if not line:
            continue

        cmd, *rest = line.split(maxsplit=1)
        args = rest[0] if rest else ""
        cmd = cmd.lower()

        if cmd == 'quit' or cmd == 'q':
            break
        elif cmd == 'help' or cmd == 'h' or cmd == '?':
            print_help()
        elif cmd == 'show' or cmd == 's':
            show_pins(pins, f"Codec 0x{codec_id:08X}  Layout {layout_id} (editing)")
        elif cmd == 'move' or cmd == 'm':
            pins = cmd_move(pins, args)
        elif cmd == 'set':
            pins = cmd_set(pins, args)
        elif cmd == 'disable' or cmd == 'd':
            pins = cmd_disable(pins, args)
        elif cmd == 'swap':
            pins = cmd_swap(pins, args)
        elif cmd == 'raw':
            pins = cmd_raw(pins, args)
        elif cmd == 'diff':
            print("\nChanges:")
            changes = 0
            for (n1, pc1), (n2, pc2) in zip(orig_pins, pins):
                if pc1 != pc2:
                    changes += 1
                    print(f"  nid 0x{n1:02X}: 0x{pc1:08X} → 0x{pc2:08X}")
                    print(f"    was: {pin_desc(pc1)}")
                    print(f"    now: {pin_desc(pc2)}")
            if changes == 0:
                print("  (no changes)")
            print()
        elif cmd == 'save':
            new_layout = args.strip()
            if not new_layout:
                print("Usage: save <layout_id>")
                continue
            try:
                new_lid = int(new_layout)
            except ValueError:
                print("Invalid layout ID")
                continue

            # Check if already exists
            existing = find_entry(hda_config, codec_id, new_lid)
            if existing:
                confirm = input(f"Layout {new_lid} already exists. Overwrite? [y/N] ")
                if confirm.lower() != 'y':
                    continue
                hda_config.remove(existing)

            # Build new entry
            new_entry = {
                'CodecID': codec_id,
                'LayoutID': new_lid,
                'ConfigData': encode_config_data(pins, extra_verbs),
            }
            if wake_data:
                new_entry['WakeConfigData'] = wake_data
            if wake_reinit:
                new_entry['WakeVerbReinit'] = True

            hda_config.append(new_entry)
            print(f"Layout {new_lid} added to plist (in memory). "
                  f"Use 'write <path>' to save to disk.")

        elif cmd == 'write' or cmd == 'w':
            out_path = args.strip() or plist_path
            confirm = input(f"Write to {out_path}? [y/N] ")
            if confirm.lower() != 'y':
                continue
            with open(out_path, 'wb') as f:
                plistlib.dump(plist, f, sort_keys=False)
            print(f"Written to {out_path}")

        else:
            print(f"Unknown command: {cmd}. Type 'help' for commands.")


if __name__ == '__main__':
    main()
