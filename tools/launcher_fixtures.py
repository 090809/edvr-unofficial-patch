#!/usr/bin/env python3
"""Write the fixture folder tools\\launcher_test reads.

  python tools\\launcher_fixtures.py --out DIR     write the fixtures into DIR
  python tools\\launcher_fixtures.py --self-test   write them to a temp folder and check them

Everything is signed with the RFC 6979 test key from sign_manifest.py, whose
public half is written to test_key.txt for the rig to trust. The launcher's real
keys (update_keys.h) are never used and no private key is read: the test key is
published in RFC 6979 itself and signs nothing real.

What the rig gets:

  test_key.txt, other_key.txt   public keys (128 hex): the trusted one and a stranger
  signing.txt                   name, manifest file, signature file, expected verdict
  shapes.txt                    name and the verdict of sign_manifest.load_manifest on
                                shapes\\<name>.json (signed by the test key as
                                shapes\\<name>.sig): the C++ validator must agree on every
                                line, which is how the two cannot drift apart
  inflate\\inflate.zip + entries.txt   every deflate block kind, empty, 5 MB random and 5 MB
                                compressible entries, with each entry's size and sha256
  inflate\\bomb.zip              an entry that records 1000 bytes and inflates to 100 MB
  releases\\<name>\\              release folders (manifest, signature, assets, launcher)

Writes only inside --out, which it empties first.
"""
import argparse
import copy
import hashlib
import io
import json
import random
import shutil
import struct
import sys
import tempfile
import zipfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sign_manifest as sm  # noqa: E402

TEST_PRIVATE = int(sm.RFC_KEY, 16)
OTHER_PRIVATE = int(sm.OTHER_KEY, 16)
TEST_PUBLIC = sm.p256_public(TEST_PRIVATE).hex()
OTHER_PUBLIC = sm.p256_public(OTHER_PRIVATE).hex()


def sign_text(data, private=TEST_PRIVATE):
    return sm.p256_sign(private, data).hex() + "\n"


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode("ascii"))


# ---- zips, written by hand so each stream's block kind is chosen ---------------

def raw_deflate(data, level=6, strategy=zlib.Z_DEFAULT_STRATEGY):
    comp = zlib.compressobj(level, zlib.DEFLATED, -15, 8, strategy)
    return comp.compress(data) + comp.flush()


def build_zip(entries):
    """entries: (name, method, compressed bytes, recorded size, crc). Returns the zip bytes."""
    body, central = bytearray(), bytearray()
    for name, method, compressed, size, crc in entries:
        encoded = name.encode("ascii")
        offset = len(body)
        body += struct.pack("<IHHHHHIIIHH", 0x04034b50, 20, 0, method, 0, 0x21, crc, len(compressed), size,
                            len(encoded), 0) + encoded + compressed
        central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 20, 20, 0, method, 0, 0x21, crc,
                               len(compressed), size, len(encoded), 0, 0, 0, 0, 0, offset) + encoded
    end = struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, len(entries), len(entries), len(central), len(body), 0)
    return bytes(body) + bytes(central) + end


def entry(name, raw, method, compressed=None):
    return (name, method, raw if compressed is None else compressed, len(raw), zlib.crc32(raw) & 0xffffffff)


def words(seed, count):
    rng = random.Random(seed)
    vocabulary = ["".join(rng.choice("abcdefghijklmnopqrstuvwxyz") for _ in range(rng.randint(2, 9)))
                  for _ in range(400)]
    return " ".join(rng.choice(vocabulary) for _ in range(count)).encode("ascii")


def first_block_type(compressed):
    return (compressed[0] >> 1) & 3 if compressed else None


def inflate_entries():
    rng = random.Random(11)
    stored = bytes(rng.getrandbits(8) for _ in range(300))
    text_fixed = words(1, 400)
    text_dynamic = words(2, 40000)
    random_level0 = rng.randbytes(70000)
    random_5mb = rng.randbytes(5 << 20)
    pattern = words(3, 2000)
    compressible = (pattern * ((5 << 20) // len(pattern) + 1))[: 5 << 20]
    return [
        entry("stored.bin", stored, 0),
        entry("empty-stored.bin", b"", 0),
        entry("empty-deflate.bin", b"", 8, b"\x03\x00"),
        entry("fixed.txt", text_fixed, 8, raw_deflate(text_fixed, 9, zlib.Z_FIXED)),
        entry("dynamic.txt", text_dynamic, 8, raw_deflate(text_dynamic, 9)),
        entry("level0.bin", random_level0, 8, raw_deflate(random_level0, 0)),
        entry("random-5mb.bin", random_5mb, 8, raw_deflate(random_5mb, 6)),
        entry("compressible-5mb.bin", compressible, 8, raw_deflate(compressible, 9)),
    ]


def write_inflate(out):
    entries = inflate_entries()
    write(out / "inflate" / "inflate.zip", build_zip(entries))
    lines = []
    raw_by_name = {}
    for name, method, compressed, size, crc in entries:
        # Recover the raw bytes through zlib to record their hash (also proves the stream).
        raw = compressed if method == 0 else zlib.decompress(compressed, -15)
        assert len(raw) == size
        raw_by_name[name] = raw
        kind = "stored" if method == 0 else {0: "block0", 1: "block1", 2: "block2"}.get(first_block_type(compressed), "?")
        lines.append("%s\t%d\t%s\t%s" % (name, size, hashlib.sha256(raw).hexdigest(), kind))
    write(out / "inflate" / "entries.txt", "\n".join(lines) + "\n")
    # The bomb records 1000 bytes and its stream inflates to 100 MB of zeros.
    bomb_raw = bytes(100 << 20)
    bomb = raw_deflate(bomb_raw, 9)
    write(out / "inflate" / "bomb.zip", build_zip([("bomb.bin", 8, bomb, 1000, zlib.crc32(bomb_raw[:1000]) & 0xffffffff)]))
    return entries


# ---- signing cases -------------------------------------------------------------

def good_manifest():
    manifest, zip_bytes = sm._fixture_manifest()
    return manifest, zip_bytes


def write_signing(out):
    manifest, _ = good_manifest()
    data = sm._dump(manifest)
    good_sig = sign_text(data)
    folder = out / "signing"
    rows = []

    def case(name, manifest_bytes, sig_text, expect):
        write(folder / (name + ".json"), manifest_bytes)
        write(folder / (name + ".sig"), sig_text)
        rows.append("%s\t%s.json\t%s.sig\t%s" % (name, name, name, expect))

    case("good", data, good_sig, "accept")
    case("good-crlf", data, good_sig[:-1] + "\r\n", "accept")
    case("good-no-newline", data, good_sig[:-1], "accept")
    tampered = data.replace(b"1794000000", b"1794000001")
    assert tampered != data and len(tampered) == len(data)
    case("manifest-byte-flipped", tampered, good_sig, "refuse")
    case("manifest-appended", data + b" ", good_sig, "refuse")
    raw_sig = good_sig.strip()
    for index in (0, 31, 32, 63, 64, 127):
        flipped = list(raw_sig)
        flipped[index] = "0" if flipped[index] != "0" else "1"
        case("sig-flipped-%d" % index, data, "".join(flipped) + "\n", "refuse")
    case("sig-uppercase", data, raw_sig.upper() + "\n", "refuse")
    case("sig-short", data, raw_sig[:-1] + "\n", "refuse")
    case("sig-long", data, raw_sig + "0\n", "refuse")
    case("sig-two-newlines", data, raw_sig + "\n\n", "refuse")
    case("sig-leading-space", data, " " + raw_sig + "\n", "refuse")
    case("sig-non-hex", data, "g" + raw_sig[1:] + "\n", "refuse")
    case("sig-empty", data, "", "refuse")
    case("sig-zero", data, "0" * 128 + "\n", "refuse")
    case("sig-r-is-n", data, ("%064x" % sm.N) + raw_sig[64:] + "\n", "refuse")
    case("sig-other-message", data, sign_text(data + b" "), "refuse")
    case("unknown-key", data, sign_text(data, OTHER_PRIVATE), "refuse")
    write(out / "signing.txt", "\n".join(rows) + "\n")
    return rows


# ---- schema shapes -------------------------------------------------------------

def shape_cases():
    base, _ = good_manifest()
    launcher = {"version": "0.1.0", "asset": "ed-up-launcher.exe", "sha256": "0" * 64, "size": 1}
    cases = []

    def add(name, data):
        cases.append((name, data if isinstance(data, bytes) else data.encode("ascii")))

    def mutate(name, change):
        clone = copy.deepcopy(base)
        change(clone)
        add(name, sm._dump(clone))

    text = sm._dump(base).decode("ascii")
    add("valid", sm._dump(base))
    add("valid-launcher", sm._dump(dict(base, launcher=launcher)))
    assert '"size": 8\n' in text
    add("valid-minus-zero-size", text.replace('"size": 8\n', '"size": -0\n', 1))
    add("valid-escaped-version", text.replace('"version": "1.0.0"', '"version": "\\u0031.0.0"', 1))
    add("valid-escaped-path", text.replace('"path": "d3d11.dll"', '"path": "\\u0064\\u0033d11.dll"', 1))
    add("valid-compact", json.dumps(base, separators=(",", ":")))
    for item in sm.ITEMS:
        mutate("item-" + item, lambda m, item=item: (m.update(files=m["files"][:1]), m["files"][0].update(item=item)))
    mutate("schema-2", lambda m: m.update(schema=2))
    mutate("schema-true", lambda m: m.update(schema=True))
    mutate("schema-float", lambda m: m.update(schema=1.0))
    for key in ("version", "build", "tag", "min_launcher"):
        mutate(key + "-number", lambda m, key=key: m.update({key: 3}))
        mutate(key + "-empty", lambda m, key=key: m.update({key: ""}))
        mutate(key + "-missing", lambda m, key=key: m.pop(key))
        mutate(key + "-null", lambda m, key=key: m.update({key: None}))
    for index, bad in enumerate((0, -1, "1", 1.5, True, None, 1794000000.0)):
        mutate("issued-%d" % index, lambda m, bad=bad: m.update(issued=bad))
    mutate("issued-missing", lambda m: m.pop("issued"))
    mutate("unknown-key", lambda m: m.update(extra=1))
    mutate("launcher-null", lambda m: m.update(launcher=None))
    mutate("launcher-short", lambda m: m.update(launcher={"version": "1"}))
    mutate("launcher-extra-key", lambda m: m.update(launcher=dict(launcher, more=1)))
    mutate("launcher-sha-upper", lambda m: m.update(launcher=dict(launcher, sha256="A" * 64)))
    mutate("launcher-asset-path", lambda m: m.update(launcher=dict(launcher, asset="a/b.exe")))
    mutate("launcher-asset-dots", lambda m: m.update(launcher=dict(launcher, asset="..")))
    mutate("launcher-size-negative", lambda m: m.update(launcher=dict(launcher, size=-1)))
    mutate("launcher-version-empty", lambda m: m.update(launcher=dict(launcher, version="")))
    mutate("assets-empty", lambda m: m.update(assets=[]))
    mutate("assets-missing", lambda m: m.pop("assets"))
    mutate("assets-not-list", lambda m: m.update(assets={}))
    mutate("files-empty", lambda m: m.update(files=[]))
    mutate("files-missing", lambda m: m.pop("files"))
    mutate("files-not-list", lambda m: m.update(files={}))
    mutate("asset-extra-key", lambda m: m["assets"][0].update(more=1))
    mutate("asset-missing-size", lambda m: m["assets"][0].pop("size"))
    mutate("asset-sha-short", lambda m: m["assets"][0].update(sha256="ab"))
    mutate("asset-sha-upper", lambda m: m["assets"][0].update(sha256="A" * 64))
    mutate("asset-size-text", lambda m: m["assets"][0].update(size="3"))
    mutate("asset-size-negative", lambda m: m["assets"][0].update(size=-5))
    mutate("file-sha-not-hex", lambda m: m["files"][0].update(sha256="g" * 64))
    mutate("file-size-negative", lambda m: m["files"][0].update(size=-1))
    mutate("file-size-float", lambda m: m["files"][0].update(size=1.5))
    mutate("file-extra-key", lambda m: m["files"][0].update(more=1))
    for index, bad in enumerate(("a b.zip", "a/b.zip", "a\\b.zip", "", "..", ".", "...", "\u00e9.zip", "a.zip\n", "a:b.zip", 5)):
        mutate("asset-name-%d" % index, lambda m, bad=bad: m["assets"][0].update(name=bad))
    for index, bad in enumerate(("", ".", "..", "/abs", "a//b", "a/./b", "a/../b", "a/", "a\\b", "C:/x", "a:b",
                                 "a\x01b", "caf\u00e9", "a\x7fb", "../x", 5, None)):
        mutate("path-%d" % index, lambda m, bad=bad: m["files"][0].update(path=bad))
    mutate("path-nested-ok", lambda m: m["files"][1].update(path="Openvr/win64/openvr_api.dll"))
    mutate("item-unknown", lambda m: m["files"][0].update(item="payload"))
    mutate("item-alias-d3d11", lambda m: m["files"][0].update(item="d3d11"))
    mutate("item-duplicate", lambda m: m["files"].append(dict(m["files"][0], path="other.dll")))
    mutate("asset-duplicate", lambda m: m["assets"].append(dict(m["assets"][0])))
    mutate("file-asset-unlisted", lambda m: m["files"][0].update(asset="nowhere.zip"))
    mutate("file-asset-number", lambda m: m["files"][0].update(asset=3))
    add("root-array", "[]")
    add("root-string", '"x"')
    add("root-null", "null")
    add("root-empty", "")
    add("truncated", text[:40])
    add("not-json", "{")
    add("trailing-junk", text + "x")
    add("trailing-value", text.strip() + " {}")
    add("duplicate-key", text.replace('"schema": 1,', '"schema": 1, "schema": 1,', 1))
    add("duplicate-key-nested", text.replace('"size": 8\n', '"size": 8, "size": 8\n', 1))
    add("duplicate-key-escaped", text.replace('"schema": 1,', '"schema": 1, "sch\\u0065ma": 1,', 1))
    add("leading-zero", text.replace('"schema": 1', '"schema": 01', 1))
    add("nan-value", text.replace('"issued": 1794000000', '"issued": NaN', 1))
    add("trailing-comma", text.replace('"schema": 1,', '"schema": 1,,', 1))
    add("single-quotes", text.replace('"schema"', "'schema'", 1))
    add("control-char-in-string", text.replace('"version": "1.0.0"', '"version": "1.\t0.0"', 1))
    add("bom", b"\xef\xbb\xbf" + sm._dump(base))
    add("non-ascii-raw", sm._dump(base).replace(b'"1.0.0"', '"\u00e9"'.encode("utf-8"), 1))
    add("lone-surrogate", text.replace('"version": "1.0.0"', '"version": "\\ud800"', 1))
    return cases


def write_shapes(out):
    rows = []
    for index, (name, data) in enumerate(shape_cases()):
        label = "%03d-%s" % (index, name)
        verdict = "accept" if not sm.load_manifest(data)[1] else "refuse"
        write(out / "shapes" / (label + ".json"), data)
        write(out / "shapes" / (label + ".sig"), sign_text(data))
        rows.append("%s\t%s" % (label, verdict))
    write(out / "shapes.txt", "\n".join(rows) + "\n")
    return rows


# ---- releases ------------------------------------------------------------------

def deterministic_zip(members):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name in sorted(members):
            info = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, members[name])
    return buffer.getvalue()


def release(out, name, version, issued, graphics, min_launcher="0.1.0", launcher_version=None):
    """One release folder: a core zip, a DLSS zip (identical across releases), a
    signed manifest and, if asked, a launcher entry. Returns the folder."""
    folder = out / "releases" / name
    rng = random.Random(5)
    dlss = deterministic_zip({"nvngx_dlss.dll": rng.randbytes(60000)})
    dlss_name = "ed-up-dlss-%s.zip" % hashlib.sha256(dlss).hexdigest()[:12]
    core_members = {"d3d11.dll": graphics, "Openvr/win64/openvr_api.dll": b"runtime " + version.encode(),
                    "edvr.ini": b"[fix]\nversion=" + version.encode() + b"\n"}
    core = deterministic_zip(core_members)
    core_name = "ed-up-core-%s.zip" % version
    files = [
        {"item": "native_graphics", "path": "d3d11.dll", "asset": core_name,
         "sha256": hashlib.sha256(graphics).hexdigest(), "size": len(graphics)},
        {"item": "native_runtime", "path": "Openvr/win64/openvr_api.dll", "asset": core_name,
         "sha256": hashlib.sha256(core_members["Openvr/win64/openvr_api.dll"]).hexdigest(),
         "size": len(core_members["Openvr/win64/openvr_api.dll"])},
        {"item": "ini", "path": "edvr.ini", "asset": core_name,
         "sha256": hashlib.sha256(core_members["edvr.ini"]).hexdigest(), "size": len(core_members["edvr.ini"])},
    ]
    nvngx = zipfile.ZipFile(io.BytesIO(dlss)).read("nvngx_dlss.dll")
    files.append({"item": "ngx", "path": "nvngx_dlss.dll", "asset": dlss_name,
                  "sha256": hashlib.sha256(nvngx).hexdigest(), "size": len(nvngx)})
    manifest = {"schema": 1, "version": version, "build": "v" + version, "tag": "v" + version, "issued": issued,
                "min_launcher": min_launcher,
                "assets": [{"name": core_name, "sha256": hashlib.sha256(core).hexdigest(), "size": len(core)},
                           {"name": dlss_name, "sha256": hashlib.sha256(dlss).hexdigest(), "size": len(dlss)}],
                "files": files}
    write(folder / core_name, core)
    write(folder / dlss_name, dlss)
    if launcher_version:
        exe = b"MZ fake launcher " + launcher_version.encode()
        manifest["launcher"] = {"version": launcher_version, "asset": "ed-up-launcher.exe",
                                "sha256": hashlib.sha256(exe).hexdigest(), "size": len(exe)}
        write(folder / "ed-up-launcher.exe", exe)
    assert not sm.validate_manifest(manifest)
    data = sm._dump(manifest)
    write(folder / sm.MANIFEST_NAME, data)
    write(folder / sm.SIGNATURE_NAME, sign_text(data))
    return folder


def write_releases(out):
    release(out, "relA", "1.0.0", 1794000000, b"graphics one")
    release(out, "relB", "1.1.0", 1794000100, b"graphics two", launcher_version="0.2.0")
    release(out, "relOld", "0.9.0", 1790000000, b"graphics old")
    release(out, "relMin", "1.2.0", 1794000200, b"graphics three", min_launcher="0.3.0")
    release(out, "relMinLauncher", "1.3.0", 1794000300, b"graphics four", min_launcher="0.3.0",
            launcher_version="0.3.0")


def generate(out):
    out = Path(out)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    write(out / "test_key.txt", TEST_PUBLIC)
    write(out / "other_key.txt", OTHER_PUBLIC)
    write_signing(out)
    write_shapes(out)
    write_inflate(out)
    write_releases(out)


# ---- self-test -----------------------------------------------------------------

def self_test():
    assert TEST_PUBLIC not in "".join(k.hex() for k in sm.header_keys()), "the test key is in update_keys.h"
    assert OTHER_PUBLIC not in "".join(k.hex() for k in sm.header_keys())
    with tempfile.TemporaryDirectory(prefix="edvr-launcher-fixtures-") as temp:
        out = Path(temp) / "fx"
        generate(out)

        # Signing cases: the good one verifies with the test key, the others as recorded.
        rows = [line.split("\t") for line in (out / "signing.txt").read_text().splitlines()]
        names = {row[0] for row in rows}
        assert {"good", "unknown-key", "manifest-byte-flipped", "sig-uppercase"} <= names
        public = bytes.fromhex(TEST_PUBLIC)
        good = [r for r in rows if r[0] == "good"][0]
        data = (out / "signing" / good[1]).read_bytes()
        assert sm.p256_verify(public, data, bytes.fromhex((out / "signing" / good[2]).read_text().strip()))
        unknown = [r for r in rows if r[0] == "unknown-key"][0]
        assert not sm.p256_verify(public, data, bytes.fromhex((out / "signing" / unknown[2]).read_text().strip()))
        assert {r[3] for r in rows} == {"accept", "refuse"}

        # Shape cases: both verdicts are present, each file is signed, and the
        # recorded verdict is the Python validator's.
        shapes = [line.split("\t") for line in (out / "shapes.txt").read_text().splitlines()]
        assert len(shapes) > 120, len(shapes)
        verdicts = {v for _, v in shapes}
        assert verdicts == {"accept", "refuse"}, verdicts
        by_name = dict(shapes)
        assert by_name["000-valid"] == "accept" and by_name["001-valid-launcher"] == "accept"
        assert by_name["%03d-bom" % [i for i, (n, _) in enumerate(shape_cases()) if n == "bom"][0]] == "refuse"
        for label, verdict in shapes[::17]:
            body = (out / "shapes" / (label + ".json")).read_bytes()
            assert (verdict == "accept") == (not sm.load_manifest(body)[1])
            assert sm.p256_verify(public, body, bytes.fromhex((out / "shapes" / (label + ".sig")).read_text().strip()))
        item_labels = [l for l in by_name if l.split("-", 1)[1] in ["item-" + i for i in sm.ITEMS]]
        assert len(item_labels) == len(sm.ITEMS)
        assert all(by_name[l] == "accept" for l in item_labels), "an ITEMS entry is not accepted"

        # Inflate fixtures: readable by Python's zipfile, hashes as recorded, all block kinds.
        recorded = [line.split("\t") for line in (out / "inflate" / "entries.txt").read_text().splitlines()]
        with zipfile.ZipFile(out / "inflate" / "inflate.zip") as archive:
            assert archive.testzip() is None
            for name, size, sha, kind in recorded:
                content = archive.read(name)
                assert len(content) == int(size) and hashlib.sha256(content).hexdigest() == sha, name
        kinds = {kind for _, _, _, kind in recorded}
        assert {"stored", "block0", "block1", "block2"} <= kinds, kinds
        assert {r[0] for r in recorded} >= {"empty-stored.bin", "empty-deflate.bin", "random-5mb.bin", "compressible-5mb.bin"}
        sizes = {r[0]: int(r[1]) for r in recorded}
        assert sizes["random-5mb.bin"] == sizes["compressible-5mb.bin"] == 5 << 20
        bomb = (out / "inflate" / "bomb.zip").read_bytes()
        assert len(bomb) < 1 << 20
        stream = bomb[30 + len("bomb.bin"):bomb.index(b"PK\x01\x02")]
        assert len(zlib.decompress(stream, -15)) == 100 << 20

        # Releases: every manifest validates and verifies, every asset and member matches,
        # relA and relB share the DLSS zip, the others differ as the rig expects.
        folders = {p.name: p for p in (out / "releases").iterdir()}
        assert set(folders) == {"relA", "relB", "relOld", "relMin", "relMinLauncher"}
        for name, folder in folders.items():
            lines = []
            assert sm.verify_directory(folder, keys=[public], out=lines.append), (name, lines)
        dlss = lambda f: sorted(p.name for p in f.glob("ed-up-dlss-*.zip"))
        assert dlss(folders["relA"]) == dlss(folders["relB"]) and len(dlss(folders["relA"])) == 1
        a = json.loads((folders["relA"] / sm.MANIFEST_NAME).read_text())
        b = json.loads((folders["relB"] / sm.MANIFEST_NAME).read_text())
        assert a["assets"][1] == b["assets"][1] and a["assets"][0] != b["assets"][0]
        assert "launcher" not in a and b["launcher"]["version"] == "0.2.0"
        assert b["issued"] > a["issued"] > json.loads((folders["relOld"] / sm.MANIFEST_NAME).read_text())["issued"]
        assert json.loads((folders["relMin"] / sm.MANIFEST_NAME).read_text())["min_launcher"] == "0.3.0"
        again = Path(temp) / "again"
        generate(again)
        assert (again / "releases" / "relA" / sm.MANIFEST_NAME).read_bytes() == (folders["relA"] / sm.MANIFEST_NAME).read_bytes(), "not deterministic"
    print("launcher_fixtures: self-test passed")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, help="folder to write the fixtures into (emptied first)")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.out:
        parser.print_help()
        return 2
    generate(args.out)
    print("launcher_fixtures: wrote %s" % args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
