#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Extract the acoustic piano presets from FluidR3_GM into a small SoundFont 2 file.

The built-in piano sound needs a piano sample set, and the full FluidR3_GM bank
is 148 MB. This keeps only the chosen presets (bank 0, GM programs 0 to 2 by default) with the
instruments and samples they use, and writes them to a new SF2 with the same generators and
modulators. The output is byte-for-byte reproducible: same input and options, same SHA-256.

    tools/extract_piano_sf2.py --out build/toonval-piano.sf2
    tools/extract_piano_sf2.py --source ~/Downloads/FluidR3_GM.sf2 --out piano.sf2 --presets 0

Without --source the bank is downloaded from SOURCE_URL and checked against SOURCE_SHA256. The
tool prints the size and SHA-256 of the result and a report of the modulators the kept presets and
instruments carry, since TinySoundFont ignores modulators.

FluidR3_GM is MIT-licensed (Copyright (c) 2000-2002, 2008 Frank Wen); the notice is copied into the
output's ICOP and ICMT fields and must ship with the file.
"""

import argparse
import hashlib
import os
import struct
import sys
import tempfile
import urllib.request

SOURCE_URL = "https://github.com/pianobooster/fluid-soundfont/releases/download/v3.1/FluidR3_GM.sf2"
SOURCE_SHA256 = "74594e8f4250680adf590507a306655a299935343583256f3b722c48a1bc1cb0"
SOURCE_SIZE = 148398306

MIT_NOTICE = (
    "FluidR3_GM: Copyright (c) 2000-2002, 2008 Frank Wen <getfrank@gmail.com>. "
    "Permission is hereby granted, free of charge, to any person obtaining a copy of this "
    "software and associated documentation files (the \"Software\"), to deal in the Software "
    "without restriction, including without limitation the rights to use, copy, modify, merge, "
    "publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons "
    "to whom the Software is furnished to do so, subject to the following conditions: The above "
    "copyright notice and this permission notice shall be included in all copies or substantial "
    "portions of the Software. THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, "
    "EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS "
    "FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT "
    "HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF "
    "CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE "
    "USE OR OTHER DEALINGS IN THE SOFTWARE."
)

# Generator numbers from the SoundFont 2.04 spec, section 8.1.2.
GEN_INSTRUMENT = 41
GEN_SAMPLE_ID = 53
GEN_NAMES = {
    0: "startAddrsOffset", 1: "endAddrsOffset", 2: "startloopAddrsOffset",
    3: "endloopAddrsOffset", 4: "startAddrsCoarseOffset", 5: "modLfoToPitch",
    6: "vibLfoToPitch", 7: "modEnvToPitch", 8: "initialFilterFc", 9: "initialFilterQ",
    10: "modLfoToFilterFc", 11: "modEnvToFilterFc", 12: "endAddrsCoarseOffset",
    13: "modLfoToVolume", 15: "chorusEffectsSend", 16: "reverbEffectsSend", 17: "pan",
    21: "delayModLFO", 22: "freqModLFO", 23: "delayVibLFO", 24: "freqVibLFO",
    25: "delayModEnv", 26: "attackModEnv", 27: "holdModEnv", 28: "decayModEnv",
    29: "sustainModEnv", 30: "releaseModEnv", 31: "keynumToModEnvHold",
    32: "keynumToModEnvDecay", 33: "delayVolEnv", 34: "attackVolEnv", 35: "holdVolEnv",
    36: "decayVolEnv", 37: "sustainVolEnv", 38: "releaseVolEnv", 39: "keynumToVolEnvHold",
    40: "keynumToVolEnvDecay", 41: "instrument", 43: "keyRange", 44: "velRange",
    45: "startloopAddrsCoarseOffset", 46: "keynum", 47: "velocity", 48: "initialAttenuation",
    50: "endloopAddrsCoarseOffset", 51: "coarseTune", 52: "fineTune", 53: "sampleID",
    54: "sampleModes", 56: "scaleTuning", 57: "exclusiveClass", 58: "overridingRootKey",
}
# General controller sources (spec 8.2.1), used when the CC flag is clear.
MOD_GENERAL_SOURCES = {
    0: "none", 2: "note-on velocity", 3: "note-on key number", 10: "poly pressure",
    13: "channel pressure", 14: "pitch wheel", 16: "pitch wheel sensitivity", 127: "link",
}
# Generators TinySoundFont reads (tsf_region_operator in tsf.h); the rest it ignores.
TSF_IGNORED_GENS = {15: "chorusEffectsSend", 16: "reverbEffectsSend"}


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(1)


# --- Reading ------------------------------------------------------------------------------------

def read_chunks(data, start, end):
    """Yields (fourcc, payload_start, payload_size) for the chunks in data[start:end]."""
    pos = start
    while pos + 8 <= end:
        fourcc = data[pos:pos + 4].decode("latin-1")
        (size,) = struct.unpack_from("<I", data, pos + 4)
        yield fourcc, pos + 8, size
        pos += 8 + size + (size & 1)


def read_lists(data):
    if data[0:4] != b"RIFF" or data[8:12] != b"sfbk":
        fail("not a SoundFont 2 file (no RIFF sfbk header)")
    lists = {}
    for fourcc, start, size in read_chunks(data, 12, len(data)):
        if fourcc != "LIST":
            continue
        kind = data[start:start + 4].decode("latin-1")
        lists[kind] = {name: (s, n) for name, s, n in read_chunks(data, start + 4, start + size)}
    for kind in ("INFO", "sdta", "pdta"):
        if kind not in lists:
            fail(f"missing LIST {kind}")
    return lists


def records(data, chunk, fmt):
    start, size = chunk
    record = struct.calcsize(fmt)
    if size % record != 0:
        fail(f"chunk size {size} is not a multiple of {record}")
    return [struct.unpack_from(fmt, data, start + i * record) for i in range(size // record)]


PHDR = "<20sHHHIII"   # name, preset, bank, bag index, library, genre, morphology
BAG = "<HH"           # generator index, modulator index
MOD = "<HHhHH"        # src, dest, amount, amount src, transform
GEN = "<HH"           # operator, amount (raw 16 bits; ranges are two bytes)
INST = "<20sH"        # name, bag index
SHDR = "<20sIIIIIbbHH"  # name, start, end, loop start, loop end, rate, pitch, correction, link, type


def name_of(raw):
    return raw.split(b"\0", 1)[0].decode("latin-1")


# --- Modulator description -----------------------------------------------------------------------

def describe_source(src):
    index = src & 0x7F
    cc = bool(src & 0x80)
    direction = "-" if src & 0x100 else "+"
    polarity = "bipolar" if src & 0x200 else "unipolar"
    curve = ["linear", "concave", "convex", "switch"][(src >> 10) & 0x3F] if ((src >> 10) & 0x3F) < 4 \
        else f"type{(src >> 10) & 0x3F}"
    what = f"CC{index}" if cc else MOD_GENERAL_SOURCES.get(index, f"source{index}")
    return f"{what} ({direction}{polarity} {curve})"


def describe_mod(mod):
    src, dest, amount, amount_src, transform = mod
    dest_name = GEN_NAMES.get(dest, f"gen{dest}") if not dest & 0x8000 else f"modulator{dest & 0x7FFF}"
    text = f"{describe_source(src)} -> {dest_name}, amount {amount}"
    if amount_src & 0x7F or amount_src & 0x80:
        text += f", scaled by {describe_source(amount_src)}"
    if transform:
        text += f", transform {transform}"
    return text


# --- Extraction ----------------------------------------------------------------------------------

def extract(data, wanted_presets, bank):
    lists = read_lists(data)
    pdta = lists["pdta"]
    phdr = records(data, pdta["phdr"], PHDR)
    pbag = records(data, pdta["pbag"], BAG)
    pmod = records(data, pdta["pmod"], MOD)
    pgen = records(data, pdta["pgen"], GEN)
    inst = records(data, pdta["inst"], INST)
    ibag = records(data, pdta["ibag"], BAG)
    imod = records(data, pdta["imod"], MOD)
    igen = records(data, pdta["igen"], GEN)
    shdr = records(data, pdta["shdr"], SHDR)
    smpl_start, smpl_size = lists["sdta"]["smpl"]
    sm24 = lists["sdta"].get("sm24")

    report = {"preset_mods": [], "inst_mods": [], "ignored_gens": [], "presets": [],
              "instruments": [], "velocity_layers": {}}

    # Presets, in the order asked for, each with its zones (bags).
    kept_presets = []
    for number in wanted_presets:
        found = [i for i in range(len(phdr) - 1) if phdr[i][1] == number and phdr[i][2] == bank]
        if not found:
            fail(f"preset {bank}:{number} not found")
        kept_presets.append(found[0])

    inst_map = {}    # old instrument index -> new
    sample_map = {}  # old sample index -> new

    def zone_range(bags, index):
        return range(bags[index][0], bags[index + 1][0]), range(bags[index][1], bags[index + 1][1])

    # Walk presets -> instruments -> samples, in order, so the new indices are deterministic.
    for p in kept_presets:
        report["presets"].append(f"{phdr[p][2]}:{phdr[p][1]} {name_of(phdr[p][0])}")
        for b in range(phdr[p][3], phdr[p + 1][3]):
            gens, mods = zone_range(pbag, b)
            for m in mods:
                report["preset_mods"].append((name_of(phdr[p][0]), describe_mod(pmod[m])))
            for g in gens:
                op, amount = pgen[g]
                if op in TSF_IGNORED_GENS:
                    report["ignored_gens"].append((name_of(phdr[p][0]), TSF_IGNORED_GENS[op], amount))
                if op == 44:  # velRange
                    report["velocity_layers"].setdefault(name_of(phdr[p][0]), set()).add(
                        (amount & 0xFF, amount >> 8))
                if op == GEN_INSTRUMENT and amount not in inst_map:
                    inst_map[amount] = len(inst_map)
    for old_inst in inst_map:
        report["instruments"].append(name_of(inst[old_inst][0]))
        layers = set()
        for b in range(inst[old_inst][1], inst[old_inst + 1][1]):
            gens, mods = zone_range(ibag, b)
            for m in mods:
                report["inst_mods"].append((name_of(inst[old_inst][0]), describe_mod(imod[m])))
            for g in gens:
                op, amount = igen[g]
                if op in TSF_IGNORED_GENS:
                    report["ignored_gens"].append((name_of(inst[old_inst][0]), TSF_IGNORED_GENS[op],
                                                   amount))
                if op == 44:  # velRange
                    layers.add((amount & 0xFF, amount >> 8))
                if op == GEN_SAMPLE_ID and amount not in sample_map:
                    sample_map[amount] = len(sample_map)
        report["velocity_layers"]["instrument " + name_of(inst[old_inst][0])] = layers
    # Stereo pairs: a kept sample's linked partner must come along.
    pending = list(sample_map)
    while pending:
        old = pending.pop()
        link = shdr[old][8]
        if shdr[old][9] & 0x6 and link not in sample_map:  # right (2) or left (4) sample
            sample_map[link] = len(sample_map)
            pending.append(link)

    # Sample data: each sample's points followed by 46 zero points (spec 7.10).
    new_smpl = bytearray()
    new_sm24 = bytearray()
    new_shdr = []
    for old in sorted(sample_map, key=sample_map.get):
        name, start, end, loop_start, loop_end, rate, pitch, corr, link, kind = shdr[old]
        if kind & 0x8000:
            fail(f"sample {name_of(name)} is a ROM sample")
        offset = len(new_smpl) // 2
        new_smpl += data[smpl_start + start * 2:smpl_start + end * 2]
        new_smpl += bytes(46 * 2)
        if sm24 is not None:
            s24_start = sm24[0]
            new_sm24 += data[s24_start + start:s24_start + end] + bytes(46)
        delta = offset - start
        new_link = sample_map.get(link, 0) if kind & 0x6 else 0
        new_shdr.append((name, start + delta, end + delta, loop_start + delta, loop_end + delta,
                         rate, pitch, corr, new_link, kind))
    new_shdr.append((b"EOS", 0, 0, 0, 0, 0, 0, 0, 0, 0))

    # Instruments with their zones, sample IDs remapped.
    new_inst, new_ibag, new_imod, new_igen = [], [], [], []
    for old_inst in sorted(inst_map, key=inst_map.get):
        new_inst.append((inst[old_inst][0], len(new_ibag)))
        for b in range(inst[old_inst][1], inst[old_inst + 1][1]):
            gens, mods = zone_range(ibag, b)
            new_ibag.append((len(new_igen), len(new_imod)))
            new_imod.extend(imod[m] for m in mods)
            for g in gens:
                op, amount = igen[g]
                new_igen.append((op, sample_map[amount] if op == GEN_SAMPLE_ID else amount))
    new_inst.append((b"EOI", len(new_ibag)))
    new_ibag.append((len(new_igen), len(new_imod)))
    new_imod.append((0, 0, 0, 0, 0))
    new_igen.append((0, 0))

    # Presets with their zones, instrument indices remapped.
    new_phdr, new_pbag, new_pmod, new_pgen = [], [], [], []
    for p in kept_presets:
        name, number, pbank, _, library, genre, morphology = phdr[p]
        new_phdr.append((name, number, pbank, len(new_pbag), library, genre, morphology))
        for b in range(phdr[p][3], phdr[p + 1][3]):
            gens, mods = zone_range(pbag, b)
            new_pbag.append((len(new_pgen), len(new_pmod)))
            new_pmod.extend(pmod[m] for m in mods)
            for g in gens:
                op, amount = pgen[g]
                new_pgen.append((op, inst_map[amount] if op == GEN_INSTRUMENT else amount))
    new_phdr.append((b"EOP", 0, 0, len(new_pbag), 0, 0, 0))
    new_pbag.append((len(new_pgen), len(new_pmod)))
    new_pmod.append((0, 0, 0, 0, 0))
    new_pgen.append((0, 0))

    info = lists["INFO"]
    ifil = data[info["ifil"][0]:info["ifil"][0] + info["ifil"][1]]
    out = write_sf2(ifil, new_smpl, new_sm24 if sm24 is not None else None,
                    [("phdr", PHDR, new_phdr), ("pbag", BAG, new_pbag), ("pmod", MOD, new_pmod),
                     ("pgen", GEN, new_pgen), ("inst", INST, new_inst), ("ibag", BAG, new_ibag),
                     ("imod", MOD, new_imod), ("igen", GEN, new_igen), ("shdr", SHDR, new_shdr)])
    report["samples"] = len(new_shdr) - 1
    report["sample_bytes"] = len(new_smpl)
    report["source_modulators"] = (len(pmod) - 1, len(imod) - 1)
    return out, report


# --- Writing -------------------------------------------------------------------------------------

def chunk(fourcc, payload):
    data = fourcc.encode("latin-1") + struct.pack("<I", len(payload)) + payload
    return data + (b"\0" if len(payload) & 1 else b"")


def zstr(text):
    raw = text.encode("latin-1") + b"\0"
    return raw + (b"\0" if len(raw) & 1 else b"")


def write_sf2(ifil, smpl, sm24, pdta_tables):
    info = b"INFO" + chunk("ifil", ifil) + chunk("isng", zstr("EMU8000")) \
        + chunk("INAM", zstr("toonval piano (from FluidR3_GM)")) \
        + chunk("ICOP", zstr("Copyright (c) 2000-2002, 2008 Frank Wen. MIT license.")) \
        + chunk("ICMT", zstr("Acoustic piano presets extracted from FluidR3_GM by "
                             "toonval tools/extract_piano_sf2.py. " + MIT_NOTICE)) \
        + chunk("ISFT", zstr("toonval extract_piano_sf2.py"))
    sdta = b"sdta" + chunk("smpl", bytes(smpl)) + (chunk("sm24", bytes(sm24)) if sm24 else b"")
    pdta = b"pdta"
    for name, fmt, rows in pdta_tables:
        pdta += chunk(name, b"".join(struct.pack(fmt, *row) for row in rows))
    body = b"sfbk" + chunk("LIST", info) + chunk("LIST", sdta) + chunk("LIST", pdta)
    return b"RIFF" + struct.pack("<I", len(body)) + body


# --- Main ----------------------------------------------------------------------------------------

def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch_source(cache_dir):
    os.makedirs(cache_dir, exist_ok=True)
    path = os.path.join(cache_dir, "FluidR3_GM.sf2")
    if os.path.exists(path) and sha256_file(path) == SOURCE_SHA256:
        return path
    print(f"downloading {SOURCE_URL} ({SOURCE_SIZE / 1e6:.0f} MB)", file=sys.stderr)
    fd, tmp = tempfile.mkstemp(dir=cache_dir, suffix=".part")
    os.close(fd)
    try:
        urllib.request.urlretrieve(SOURCE_URL, tmp)
        got = sha256_file(tmp)
        if got != SOURCE_SHA256:
            fail(f"downloaded file has SHA-256 {got}, expected {SOURCE_SHA256}")
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--source", help="local FluidR3_GM.sf2 (default: download and verify)")
    parser.add_argument("--cache-dir", default=os.path.join("build", "piano-source"),
                        help="where the downloaded bank is kept (default build/piano-source)")
    parser.add_argument("--out", action="append", required=True,
                        help="output .sf2 path; repeat to write several copies")
    parser.add_argument("--presets", default="0,1,2",
                        help="comma-separated GM program numbers in bank 0 (default 0,1,2)")
    parser.add_argument("--bank", type=int, default=0)
    args = parser.parse_args()

    source = args.source or fetch_source(args.cache_dir)
    source_hash = sha256_file(source)
    if args.source and source_hash != SOURCE_SHA256:
        print(f"note: {source} has SHA-256 {source_hash}, not the pinned FluidR3_GM v3.1 "
              f"{SOURCE_SHA256}; the output will not match the published hash", file=sys.stderr)
    with open(source, "rb") as f:
        data = f.read()
    presets = [int(p) for p in args.presets.split(",")]
    out, report = extract(data, presets, args.bank)
    digest = hashlib.sha256(out).hexdigest()
    for path in args.out:
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, "wb") as f:
            f.write(out)

    print(f"source:      {source} ({len(data)} bytes, sha256 {source_hash})")
    print(f"presets:     {'; '.join(report['presets'])}")
    print(f"instruments: {'; '.join(report['instruments'])}")
    for name, layers in report["velocity_layers"].items():
        shown = ", ".join(f"{lo}-{hi}" for lo, hi in sorted(layers)) or "none (one layer)"
        print(f"  velocity ranges in {name}: {shown}")
    print(f"samples:     {report['samples']} ({report['sample_bytes']} bytes of 16-bit data)")
    print(f"modulators in the whole source bank: {report['source_modulators'][0]} preset, "
          f"{report['source_modulators'][1]} instrument")
    print(f"modulators in the kept presets: {len(report['preset_mods'])}; "
          f"in the kept instruments: {len(report['inst_mods'])}")
    seen = set()
    for owner, text in report["preset_mods"] + report["inst_mods"]:
        if (owner, text) not in seen:
            seen.add((owner, text))
            print(f"  {owner}: {text}")
    ignored = sorted(set(report["ignored_gens"]))
    if ignored:
        print("generators TinySoundFont ignores:")
        for owner, gen, amount in ignored:
            print(f"  {owner}: {gen} = {amount}")
    print("TinySoundFont also skips the SF2 default modulators (spec 8.4), e.g. velocity to filter "
          "cutoff; it maps velocity to gain itself.")
    for path in args.out:
        print(f"wrote:       {path}")
    print(f"size:        {len(out)} bytes")
    print(f"sha256:      {digest}")


if __name__ == "__main__":
    main()
