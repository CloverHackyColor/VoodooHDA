#!/usr/bin/env python3
"""
Generate AppleALCPinConfigs.h and AppleALCPinConfigs.cpp from
AppleALC's PinConfigs.kext/Contents/Info.plist.

Usage:
    python3 tools/gen_pinconfigs.py <path-to-PinConfigs-Info.plist> <output-dir>

Example:
    python3 tools/gen_pinconfigs.py \
        ../AppleALC/Resources/PinConfigs.kext/Contents/Info.plist \
        tranc
"""

import plistlib
import struct
import sys
import os
from collections import defaultdict


def parse_verbs(data):
    """Parse ConfigData/WakeConfigData into (pin_configs, extra_verbs).

    Each 4-byte big-endian word: [CAD:4][NID:8][VERB:12][PARAM:8]
    Verbs 0x71C-0x71F are SET_CONFIG_DEFAULT_BYTES 0..3.
    Everything else is an extra verb.

    Returns:
        pin_configs: list of (nid, pinConfig_u32)
        extra_verbs: list of raw 32-bit verb words (with CAD zeroed for portability)
    """
    if not data or len(data) % 4 != 0:
        return [], []

    verbs_by_nid = defaultdict(dict)  # nid -> {verb_id: param}
    extra_verbs = []

    for i in range(0, len(data), 4):
        word = struct.unpack('>I', data[i:i+4])[0]
        # cad = (word >> 28) & 0xF  # not used for storage
        nid = (word >> 20) & 0xFF
        verb_id = (word >> 8) & 0xFFF
        param = word & 0xFF

        if 0x71C <= verb_id <= 0x71F:
            verbs_by_nid[nid][verb_id] = param
        else:
            # Store verb with CAD=0 so we can substitute real CAD at runtime
            extra_verbs.append(word & 0x0FFFFFFF)

    pin_configs = []
    for nid in sorted(verbs_by_nid.keys()):
        vmap = verbs_by_nid[nid]
        if len(vmap) == 4:
            pin_config = (
                (vmap[0x71F] << 24) |
                (vmap[0x71E] << 16) |
                (vmap[0x71D] << 8) |
                vmap[0x71C]
            )
            pin_configs.append((nid, pin_config))

    return pin_configs, extra_verbs


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <plist-path> <output-dir>", file=sys.stderr)
        sys.exit(1)

    plist_path = sys.argv[1]
    output_dir = sys.argv[2]

    with open(plist_path, 'rb') as f:
        plist = plistlib.load(f)

    # Navigate to HDAConfigDefault array
    personalities = plist.get('IOKitPersonalities', {})
    hda_config = None
    for key, val in personalities.items():
        if isinstance(val, dict) and 'HDAConfigDefault' in val:
            hda_config = val['HDAConfigDefault']
            break

    if not hda_config:
        print("Error: HDAConfigDefault not found in plist", file=sys.stderr)
        sys.exit(1)

    print(f"Found {len(hda_config)} entries in HDAConfigDefault")

    # Process entries
    entries = []
    all_pin_configs = []
    all_extra_verbs = []
    all_wake_verbs = []

    for item in hda_config:
        codec_id = item.get('CodecID', 0)
        layout_id = item.get('LayoutID', 0)
        config_data = item.get('ConfigData', b'')
        wake_config_data = item.get('WakeConfigData', b'')
        wake_verb_reinit = item.get('WakeVerbReinit', False)

        pins, extras = parse_verbs(config_data)
        wake_pins, wake_extras = parse_verbs(wake_config_data)

        # For wake verbs, we only care about non-pin-config verbs
        # (pin configs are non-volatile), so wake_extras is what matters
        wake_verbs = wake_extras

        # If WakeVerbReinit is true and no dedicated wake data,
        # wake verbs should replay extra verbs
        if wake_verb_reinit and not wake_verbs and extras:
            wake_verbs = list(extras)

        pin_start = len(all_pin_configs)
        pin_count = len(pins)
        all_pin_configs.extend(pins)

        extra_start = len(all_extra_verbs)
        extra_count = len(extras)
        all_extra_verbs.extend(extras)

        wake_start = len(all_wake_verbs)
        wake_count = len(wake_verbs)
        all_wake_verbs.extend(wake_verbs)

        entries.append({
            'codecId': codec_id,
            'layoutId': layout_id,
            'pinConfigStart': pin_start,
            'pinConfigCount': pin_count,
            'extraVerbStart': extra_start,
            'extraVerbCount': extra_count,
            'wakeVerbStart': wake_start,
            'wakeVerbCount': wake_count,
        })

    # Sort entries by (codecId, layoutId) for binary search
    entries.sort(key=lambda e: (e['codecId'], e['layoutId']))

    print(f"Total pin configs: {len(all_pin_configs)}")
    print(f"Total extra verbs: {len(all_extra_verbs)}")
    print(f"Total wake verbs:  {len(all_wake_verbs)}")
    print(f"Total entries:     {len(entries)}")

    # Estimate data size
    data_size = (
        len(all_pin_configs) * 5 +  # ALCPinConfig: 1 + 4 bytes
        len(all_extra_verbs) * 4 +
        len(all_wake_verbs) * 4 +
        len(entries) * 16  # ALCConfigEntry
    )
    print(f"Estimated const data size: {data_size / 1024:.1f} KB")

    # Generate header
    header_path = os.path.join(output_dir, 'AppleALCPinConfigs.h')
    with open(header_path, 'w') as f:
        f.write("/* Auto-generated by tools/gen_pinconfigs.py from AppleALC PinConfigs.kext */\n")
        f.write("/* Do not edit manually. */\n\n")
        f.write("#ifndef _APPLEALC_PIN_CONFIGS_H\n")
        f.write("#define _APPLEALC_PIN_CONFIGS_H\n\n")
        f.write("#include <libkern/OSTypes.h>\n\n")

        f.write("typedef struct {\n")
        f.write("    UInt8  nid;\n")
        f.write("    UInt32 pinConfig;\n")
        f.write("} __attribute__((packed)) ALCPinConfig;\n\n")

        f.write("typedef struct {\n")
        f.write("    UInt32 codecId;\n")
        f.write("    UInt32 layoutId;\n")
        f.write("    UInt16 pinConfigStart;\n")
        f.write("    UInt8  pinConfigCount;\n")
        f.write("    UInt16 extraVerbStart;\n")
        f.write("    UInt16 extraVerbCount;\n")
        f.write("    UInt16 wakeVerbStart;\n")
        f.write("    UInt16 wakeVerbCount;\n")
        f.write("} __attribute__((packed)) ALCConfigEntry;\n\n")

        f.write(f"#define ALC_PIN_CONFIGS_COUNT  {len(all_pin_configs)}\n")
        f.write(f"#define ALC_EXTRA_VERBS_COUNT  {len(all_extra_verbs)}\n")
        f.write(f"#define ALC_WAKE_VERBS_COUNT   {len(all_wake_verbs)}\n")
        f.write(f"#define ALC_CONFIG_ENTRIES_COUNT {len(entries)}\n\n")

        f.write("extern const ALCPinConfig   gALCPinConfigs[];\n")
        f.write("extern const UInt32         gALCExtraVerbs[];\n")
        f.write("extern const UInt32         gALCWakeVerbs[];\n")
        f.write("extern const ALCConfigEntry gALCConfigEntries[];\n\n")

        f.write("const ALCConfigEntry* alcLookupPinConfig(UInt32 codecId, UInt32 layoutId);\n\n")

        f.write("#endif /* _APPLEALC_PIN_CONFIGS_H */\n")

    print(f"Generated {header_path}")

    # Generate source
    source_path = os.path.join(output_dir, 'AppleALCPinConfigs.cpp')
    with open(source_path, 'w') as f:
        f.write("/* Auto-generated by tools/gen_pinconfigs.py from AppleALC PinConfigs.kext */\n")
        f.write("/* Do not edit manually. */\n\n")
        f.write('#include "AppleALCPinConfigs.h"\n\n')

        # Pin configs array
        f.write(f"const ALCPinConfig gALCPinConfigs[ALC_PIN_CONFIGS_COUNT] = {{\n")
        for i, (nid, pc) in enumerate(all_pin_configs):
            f.write(f"    {{ 0x{nid:02X}, 0x{pc:08X} }}")
            f.write(",\n" if i < len(all_pin_configs) - 1 else "\n")
        f.write("};\n\n")

        # Extra verbs array
        if all_extra_verbs:
            f.write(f"const UInt32 gALCExtraVerbs[ALC_EXTRA_VERBS_COUNT] = {{\n")
            for i, verb in enumerate(all_extra_verbs):
                if i % 8 == 0:
                    f.write("    ")
                f.write(f"0x{verb:08X}")
                if i < len(all_extra_verbs) - 1:
                    f.write(", ")
                if i % 8 == 7 or i == len(all_extra_verbs) - 1:
                    f.write("\n")
            f.write("};\n\n")
        else:
            f.write("const UInt32 gALCExtraVerbs[1] = { 0 };\n\n")

        # Wake verbs array
        if all_wake_verbs:
            f.write(f"const UInt32 gALCWakeVerbs[ALC_WAKE_VERBS_COUNT] = {{\n")
            for i, verb in enumerate(all_wake_verbs):
                if i % 8 == 0:
                    f.write("    ")
                f.write(f"0x{verb:08X}")
                if i < len(all_wake_verbs) - 1:
                    f.write(", ")
                if i % 8 == 7 or i == len(all_wake_verbs) - 1:
                    f.write("\n")
            f.write("};\n\n")
        else:
            f.write("const UInt32 gALCWakeVerbs[1] = { 0 };\n\n")

        # Config entries array (sorted by codecId, layoutId)
        f.write(f"const ALCConfigEntry gALCConfigEntries[ALC_CONFIG_ENTRIES_COUNT] = {{\n")
        for i, e in enumerate(entries):
            f.write(f"    {{ 0x{e['codecId']:08X}, {e['layoutId']:4d}, "
                    f"{e['pinConfigStart']:5d}, {e['pinConfigCount']:3d}, "
                    f"{e['extraVerbStart']:5d}, {e['extraVerbCount']:4d}, "
                    f"{e['wakeVerbStart']:5d}, {e['wakeVerbCount']:4d} }}")
            f.write(",\n" if i < len(entries) - 1 else "\n")
        f.write("};\n\n")

        # Binary search implementation
        f.write("const ALCConfigEntry* alcLookupPinConfig(UInt32 codecId, UInt32 layoutId)\n")
        f.write("{\n")
        f.write("    int lo = 0, hi = ALC_CONFIG_ENTRIES_COUNT - 1;\n")
        f.write("    while (lo <= hi) {\n")
        f.write("        int mid = (lo + hi) / 2;\n")
        f.write("        const ALCConfigEntry *e = &gALCConfigEntries[mid];\n")
        f.write("        if (e->codecId < codecId) {\n")
        f.write("            lo = mid + 1;\n")
        f.write("        } else if (e->codecId > codecId) {\n")
        f.write("            hi = mid - 1;\n")
        f.write("        } else if (e->layoutId < layoutId) {\n")
        f.write("            lo = mid + 1;\n")
        f.write("        } else if (e->layoutId > layoutId) {\n")
        f.write("            hi = mid - 1;\n")
        f.write("        } else {\n")
        f.write("            return e;\n")
        f.write("        }\n")
        f.write("    }\n")
        f.write("    return 0;\n")
        f.write("}\n")

    print(f"Generated {source_path}")


if __name__ == '__main__':
    main()
