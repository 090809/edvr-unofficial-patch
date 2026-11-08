#!/usr/bin/env python3
"""Does the private hull layer light like the game? (diag.hull_layer, Step 1; docs/research-own-hull-layer-2026-11-08.md "Step 1 build")

Usage: python tools/hull_layer_compare.py <edvr_logs\\eyes\\eye_<stamp>> [--eye 1] [--tol 0.05] [--clear-depth 0] [--log <edvr_gfx_*.log>]
       python tools/hull_layer_compare.py --self-test

Reads eye_<stamp>_HullHdr.bin (the private HDR after the eye's last private lighting draw), eye_<stamp>_GameHdrLit.bin
(the game's HDR after its last lighting draw, before the sky) and eye_<stamp>_HullDepth.bin (the private depth after
the neighbour filter); --eye 1 reads the _e1 files. Hull pixels are where the private depth is not the clear value
(reversed Z: 0). On them both images hold emissive plus all the deferred lighting, so they should agree, except where
the game's AO (none on the layer) or the hull decals (not re-issued) differ.

Prints the hull pixel count, the fraction within --tol of the game's luma, the median and p95 ratio private/game,
the black fraction (private dark where the game is lit), the best whole-pixel shift within 3 px, and the own-mask
count and filtered pixels from the dump's log line (--log, else the newest edvr_gfx_*.log beside the eyes folder that
names the stamp).
Verdict and exit code: CONFIRMED 0 (median ratio 0.95..1.35 and at least half the hull pixels within --tol),
REFUTED 3 (under 500 hull pixels, more than half black, or a shift that matches 0.2 better than none),
INCONCLUSIVE 1 (also when the game's HDR is itself mostly black: the comparison point is wrong, not the layer),
bad input 2.

A separate-mode dump (diag.hull_layer = separate, Step 2: "Step 2 build") also has eye_<stamp>_GameDepth.bin, the game's
main depth after the eye's G-buffer with the own ship discarded (and _GameGbuf0.bin, its RT0). The game's HDR then has
no hull to compare, so the tool measures the main pass instead, on the private hull pixels: the share where the main
depth is farther than the hull's (reversed Z: smaller; the terrain behind it filled in), the share still at the clear
value (holes: nothing drawn there, or open sky), the share equal to the hull's depth (the hull still in the main pass)
and the share nearer (the cockpit or a neighbour in front). Verdict: TAKEN OUT 0 (at most 5% equal), NOT TAKEN OUT 3.
"""
import argparse
import glob
import os
import re
import struct
import sys

MIN_HULL = 500
SHIFT = 3
MIN_GAME_LIT = 0.05   # the game's HDR lit on less of the hull than this: the comparison point, not the layer
MAX_STILL_HULL = 0.05   # separate mode: the main depth equal to the hull's on more of its pixels than this, the hull is still there


def main_pass(prefix, eye=0, clear_depth=0.0):
    """Separate mode: the main depth against the private hull depth on the private hull pixels, as a dict."""
    import numpy as np
    import eye_inputs
    suffix = '_e1' if eye else ''
    paths = {k: '%s_%s%s.bin' % (prefix, k, suffix) for k in ('HullDepth', 'GameDepth')}
    for k, p in paths.items():
        if not os.path.exists(p):
            raise ValueError('missing %s: %s' % (k, p))
    _, hull = eye_inputs.read(paths['HullDepth'])
    _, main = eye_inputs.read(paths['GameDepth'])
    hull, main = np.asarray(hull, np.float32), np.asarray(main, np.float32)
    if hull.shape != main.shape:
        raise ValueError('HullDepth %s and GameDepth %s are not one eye size' % (hull.shape, main.shape))
    clear = np.float32(clear_depth)
    mask = hull != clear
    n = int(mask.sum())
    out = {'eye': eye, 'mode': 'separate', 'hull_pixels': n}
    if n < MIN_HULL:
        out.update(verdict='NOT TAKEN OUT', why='the private depth holds %d hull pixels (under %d): nothing to measure' % (n, MIN_HULL))
        return out
    h, m = hull[mask], main[mask]
    holes = m == clear
    equal = m == h
    filled = (m < h) & ~holes
    nearer = m > h
    out.update(filled=float(filled.mean()), holes=float(holes.mean()), equal=float(equal.mean()), nearer=float(nearer.mean()))
    if out['equal'] > MAX_STILL_HULL:
        out.update(verdict='NOT TAKEN OUT', why='the main depth still equals the hull\'s on %.1f%% of its pixels: the own ship is in the main pass'
                   % (100 * out['equal']))
    else:
        out.update(verdict='TAKEN OUT', why='the own ship is out of the main pass: %.1f%% of its pixels now hold something farther, %.1f%% are holes'
                   % (100 * out['filled'], 100 * out['holes']))
    return out


def luma(rgb):
    return 0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]


def within_fraction(priv, game, mask, tol):
    import numpy as np
    if not mask.any():
        return 0.0
    rel = np.abs(priv[mask] - game[mask]) / np.maximum(game[mask], 1e-3)
    return float((rel <= tol).mean())


def compare(prefix, eye=0, tol=0.05, clear_depth=0.0):
    """The measurement and verdict as a dict; raises ValueError on missing or mismatched files."""
    import numpy as np
    import eye_inputs
    suffix = '_e1' if eye else ''
    paths = {k: '%s_%s%s.bin' % (prefix, k, suffix) for k in ('HullHdr', 'GameHdrLit', 'HullDepth')}
    for k, p in paths.items():
        if not os.path.exists(p):
            raise ValueError('missing %s: %s' % (k, p))
    _, hull = eye_inputs.read(paths['HullHdr'])
    _, game = eye_inputs.read(paths['GameHdrLit'])
    _, depth = eye_inputs.read(paths['HullDepth'])
    if hull.ndim != 3 or game.ndim != 3 or hull.shape != game.shape or depth.shape != hull.shape[:2]:
        raise ValueError('the three files do not share one eye size (%s, %s, %s)' % (hull.shape, game.shape, depth.shape))
    mask = depth != np.float32(clear_depth)
    out = {'eye': eye, 'tol': tol, 'private_pixels': int(mask.sum())}
    # Where the game shows the hull: the eye run's scene depth (eye_<stamp>_Z.bin, written for eye 0) equals the private
    # depth. Elsewhere the cockpit or canopy is in front in the game's image, and the two HDRs differ by design (the
    # 2026-11-08 20:30 flight: 27% of the private pixels visible, 85-98% of those bit-exact).
    zpath = '%s_Z.bin' % prefix
    if os.path.exists(zpath):
        zmeta, z = eye_inputs.read(zpath)
        if zmeta.get('eye', 0) == eye and np.asarray(z).shape == depth.shape:
            z = np.asarray(z, np.float32)
            mask &= np.abs(z - depth) <= 1e-6 * np.maximum(np.abs(depth), np.float32(1e-9)) + np.float32(1e-9)
            out['visible_only'] = True
    lh, lg = luma(np.nan_to_num(hull)), luma(np.nan_to_num(game))
    n = int(mask.sum())
    out['hull_pixels'] = n
    if n < MIN_HULL:
        out.update(verdict='REFUTED', why='the private depth holds %d hull pixels (under %d): the block was not drawn into it' % (n, MIN_HULL))
        return out
    lit = mask & (lg > 1e-4)
    game_lit = float(lit.sum()) / n
    out['game_lit'] = game_lit
    if game_lit < MIN_GAME_LIT:
        out.update(verdict='INCONCLUSIVE', why='the game\'s HDR is lit on only %.1f%% of the hull pixels: the comparison point is wrong, not the layer'
                   % (100 * game_lit))
        return out
    black = float(((lh <= 1e-6) & lit).sum()) / max(int(lit.sum()), 1)
    ratio = lh[lit] / np.maximum(lg[lit], 1e-6)
    within = within_fraction(lh, lg, mask, tol)
    best, best_frac = (0, 0), within
    h, w = lh.shape
    for dy in range(-SHIFT, SHIFT + 1):
        for dx in range(-SHIFT, SHIFT + 1):
            if (dx, dy) == (0, 0):
                continue
            # the private image moved by (dx, dy): its pixel (x, y) against the game's (x + dx, y + dy)
            ys, yd = slice(max(0, -dy), h - max(0, dy)), slice(max(0, dy), h - max(0, -dy))
            xs, xd = slice(max(0, -dx), w - max(0, dx)), slice(max(0, dx), w - max(0, -dx))
            frac = within_fraction(lh[ys, xs], lg[yd, xd], mask[ys, xs], tol)
            if frac > best_frac:
                best, best_frac = (dx, dy), frac
    out.update(within=within, black=black, median=float(np.median(ratio)) if ratio.size else 0.0,
               p95=float(np.percentile(ratio, 95)) if ratio.size else 0.0, shift=best, shift_within=best_frac)
    if black > 0.5:
        out.update(verdict='REFUTED', why='%.0f%% of the lit hull pixels are black in the private layer: unlit (the lighting bindings)' % (100 * black))
    elif best != (0, 0) and best_frac > within + 0.2:
        out.update(verdict='REFUTED', why='the layer matches %.0f%% shifted by %s against %.0f%% in place: offset' % (100 * best_frac, best, 100 * within))
    elif 0.95 <= out['median'] <= 1.35 and within >= 0.5:
        out.update(verdict='CONFIRMED', why='the private layer lights like the game on the hull (AO and decals aside)')
    else:
        out.update(verdict='INCONCLUSIVE', why='neither black nor offset, but the luma does not agree closely enough')
    return out


LOG_DUMP = re.compile(r"hull layer: dump (?P<stamp>\S+) eye (?P<eye>\d) frame \d+: .*?; own=(?P<own>-?\d+) filtered_px=(?P<filtered>-?\d+)")


def own_from_log(prefix, eye, log=None):
    """(own records, filtered pixels) from the dump's log line, or None: --log, else the edvr_gfx_*.log files beside the eyes folder, newest first."""
    stamp = os.path.basename(prefix)[len('eye_'):] if os.path.basename(prefix).startswith('eye_') else os.path.basename(prefix)
    logs = [log] if log else sorted(glob.glob(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(prefix))), 'edvr_gfx_*.log')),
                                    key=os.path.getmtime, reverse=True)
    for path in logs:
        try:
            text = open(path, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        for m in LOG_DUMP.finditer(text):
            if m.group('stamp') == stamp and int(m.group('eye')) == eye:
                return int(m.group('own')), int(m.group('filtered'))
    return None


def report(r):
    if r.get('mode') == 'separate':
        line = 'hull layer compare (separate mode, the main pass): eye %d, %d private hull pixel(s)' % (r['eye'], r['hull_pixels'])
        if r.get('own') is not None:
            line += ', own mask %d record(s)' % r['own'][0]
        if 'filled' in r:
            line += ('; main depth farther (filled in) %.1f%%, at the clear value (holes) %.1f%%, equal to the hull (still there) %.1f%%, nearer '
                     '(cockpit or neighbour in front) %.1f%%' % (100 * r['filled'], 100 * r['holes'], 100 * r['equal'], 100 * r['nearer']))
        return line + '\n%s: %s' % (r['verdict'], r['why'])
    line = 'hull layer compare: eye %d, %d hull pixel(s)' % (r['eye'], r['hull_pixels'])
    if r.get('visible_only'):
        line += ' the game shows (of %d private)' % r['private_pixels']
    if r.get('own') is not None:
        line += ', own mask %d record(s), %d pixel(s) filtered as neighbours' % r['own']
    if 'game_lit' in r:
        line += ', the game lit on %.1f%% of them' % (100 * r['game_lit'])
    if 'within' in r:
        line += ('; %.1f%% within %.0f%% of the game, ratio private/game median %.3f p95 %.3f, black %.1f%%, best shift %s (%.1f%% within)'
                 % (100 * r['within'], 100 * r['tol'], r['median'], r['p95'], 100 * r['black'], r['shift'], 100 * r['shift_within']))
    return line + '\n%s: %s' % (r['verdict'], r['why'])


def pack_r11g11b10(rgb):
    """float RGB (non-negative, normal or zero) as R11G11B10_FLOAT words: the self-test's encoder."""
    import numpy as np
    words = np.zeros(rgb.shape[:-1], dtype=np.uint32)
    for c, (shift, bits) in enumerate(((0, 6), (11, 6), (22, 5))):
        v = rgb[..., c].astype(np.float64)
        e = np.where(v > 0, np.floor(np.log2(np.where(v > 0, v, 1.0))), -15).astype(np.int64)
        mant = np.where(v > 0, np.round((v / np.exp2(e) - 1.0) * (1 << bits)), 0).astype(np.int64)
        e, mant = np.where(mant == (1 << bits), e + 1, e), np.where(mant == (1 << bits), 0, mant)
        field = np.where(v > 0, ((e + 15) << bits) | mant, 0).astype(np.uint32)
        words |= field << shift
    return words


def self_test():
    import tempfile
    try:
        import numpy as np
    except ImportError:
        print('hull layer compare self-test skipped: NumPy is not installed')
        return 0
    failures = []

    def write(path, fmt, eye, payload, w, h, px):
        with open(path, 'wb') as f:
            f.write(b'EDVRTEX1' + struct.pack('<9I', 1, w, h, fmt, w * px, 7, eye, 0, 0) + payload)

    def case(name, priv_rgb, game_rgb, depth, want, eye=0, scene_z=None):
        with tempfile.TemporaryDirectory() as temp:
            prefix = os.path.join(temp, 'eye_123456789001')
            sfx = '_e1' if eye else ''
            h, w = depth.shape
            write(prefix + '_HullHdr%s.bin' % sfx, 26, eye, pack_r11g11b10(priv_rgb).tobytes(), w, h, 4)
            write(prefix + '_GameHdrLit%s.bin' % sfx, 26, eye, pack_r11g11b10(game_rgb).tobytes(), w, h, 4)
            d = np.zeros((h, w, 2), dtype=np.float32)
            d[..., 0] = depth
            write(prefix + '_HullDepth%s.bin' % sfx, 19, eye, d.tobytes(), w, h, 8)
            if scene_z is not None:
                write(prefix + '_Z.bin', 41, eye, scene_z.astype(np.float32).tobytes(), w, h, 4)
            r = compare(prefix, eye=eye)
            if r['verdict'] != want:
                failures.append('%s: %s, wanted %s (%r)' % (name, r['verdict'], want, r))

    h, w = 48, 64
    yy, xx = np.mgrid[0:h, 0:w]
    # A textured lit scene: luma varies by more than the tolerance between neighbours, so a shift is visible.
    base = 0.2 + 0.8 * ((xx * 7 + yy * 13) % 11) / 10.0
    game = np.stack([base, base * 0.9, base * 0.8], axis=-1)
    depth = np.zeros((h, w), dtype=np.float32)
    depth[8:40, 10:60] = 0.25   # 1600 hull pixels
    hull = np.where(depth[..., None] > 0, game, 0.0)
    case('the same light on the hull', hull, game, depth, 'CONFIRMED')
    case('the same light, eye 1', hull, game, depth, 'CONFIRMED', eye=1)
    case('a black layer', np.zeros_like(game), game, depth, 'REFUTED')
    shifted = np.where(depth[..., None] > 0, np.roll(np.roll(game, -2, axis=1), 1, axis=0), 0.0)
    case('a layer two pixels off', shifted, game, depth, 'REFUTED')
    case('an empty private depth', hull, game, np.zeros_like(depth), 'REFUTED')
    case('a layer at half the light', hull * 0.5, game, depth, 'INCONCLUSIVE')
    case('the game black too (the wrong comparison point)', np.zeros_like(game), np.zeros_like(game), depth, 'INCONCLUSIVE')
    # The cockpit in front of most of the hull in the game's image (reversed Z: nearer is larger): its pixels differ by
    # design. With the scene depth the comparison keeps the hull the game shows and confirms; without it, it cannot.
    cockpit = depth > 0
    cockpit[8:40, 10:44] = False
    occluded_game = np.where(cockpit[..., None], game, 0.05)
    scene_z = np.where(cockpit, depth, 0.9).astype(np.float32)
    case('a cockpit in front, scene depth given', hull, occluded_game, depth, 'CONFIRMED', scene_z=scene_z)
    case('a cockpit in front, no scene depth', hull, occluded_game, depth, 'INCONCLUSIVE')
    # The own-mask count comes from the dump's log line beside the eyes folder.
    with tempfile.TemporaryDirectory() as temp:
        os.makedirs(os.path.join(temp, 'eyes'))
        with open(os.path.join(temp, 'edvr_gfx_20261108_120000.log'), 'w', encoding='utf-8') as f:
            f.write('[12:00:01.000] hull layer: dump 123456789001 eye 1 frame 9: HullHdr 64x48 format 26 written, GameHdrLit written, HullDepth written; '
                    'own=54 filtered_px=321 seed=3/3 hull_draws=84 light_draws=11\n')
        got = own_from_log(os.path.join(temp, 'eyes', 'eye_123456789001'), 1)
        if got != (54, 321):
            failures.append('the own count from the log: %r, wanted (54, 321)' % (got,))
        if own_from_log(os.path.join(temp, 'eyes', 'eye_123456789001'), 0) is not None:
            failures.append('the own count of eye 0 was read off eye 1\'s line')
    # Separate mode: the main depth with the hull discarded (terrain farther, sky at the clear value) against one that kept it.
    def separate_case(name, main_depth, want, eye=0):
        with tempfile.TemporaryDirectory() as temp:
            prefix = os.path.join(temp, 'eye_123456789001')
            sfx = '_e1' if eye else ''
            for label, plane in (('HullDepth', depth), ('GameDepth', main_depth)):
                d = np.zeros((h, w, 2), dtype=np.float32)
                d[..., 0] = plane
                write(prefix + '_%s%s.bin' % (label, sfx), 19, eye, d.tobytes(), w, h, 8)
            r = main_pass(prefix, eye=eye)
            if r['verdict'] != want:
                failures.append('%s: %s, wanted %s (%r)' % (name, r['verdict'], want, r))
            return r
    terrain = np.where(xx < 35, 0.1, 0.0).astype(np.float32)   # the terrain behind the hull's left part, open sky on the right
    got = separate_case('the hull taken out of the main pass', terrain, 'TAKEN OUT')
    want = {'filled': float((terrain[8:40, 10:60] > 0).mean()), 'holes': float((terrain[8:40, 10:60] == 0).mean())}
    if abs(got.get('filled', -1) - want['filled']) > 1e-9 or abs(got.get('holes', -1) - want['holes']) > 1e-9:
        failures.append('the filled and hole shares: %r, wanted %r' % (got, want))
    separate_case('the hull taken out, eye 1', terrain, 'TAKEN OUT', eye=1)
    separate_case('the hull still in the main pass', np.maximum(depth, terrain), 'NOT TAKEN OUT')
    # A missing file is bad input.
    with tempfile.TemporaryDirectory() as temp:
        try:
            compare(os.path.join(temp, 'eye_x'))
            failures.append('a missing file was accepted')
        except ValueError:
            pass
    for f in failures:
        print('hull layer compare self-test FAILED: ' + f)
    if not failures:
        print('hull layer compare self-test passed (14 cases)')
    return 1 if failures else 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('prefix', nargs='?', help='the eye run prefix: <edvr_logs>\\eyes\\eye_<stamp>')
    p.add_argument('--eye', type=int, default=0, choices=(0, 1))
    p.add_argument('--tol', type=float, default=0.05, help='relative luma tolerance (default 0.05: R11G11B10 rounds to about 1.6%%)')
    p.add_argument('--clear-depth', type=float, default=0.0, help='the private depth\'s clear value (reversed Z: 0)')
    p.add_argument('--log', help='the graphics log holding the dump\'s line (default: the newest edvr_gfx_*.log beside the eyes folder)')
    p.add_argument('--self-test', action='store_true')
    a = p.parse_args(argv)
    if a.self_test:
        return self_test()
    if not a.prefix:
        p.error('give the eye run prefix, or --self-test')
    try:
        separate = os.path.exists('%s_GameDepth%s.bin' % (a.prefix, '_e1' if a.eye else ''))
        r = main_pass(a.prefix, eye=a.eye, clear_depth=a.clear_depth) if separate else compare(a.prefix, eye=a.eye, tol=a.tol, clear_depth=a.clear_depth)
        r['own'] = own_from_log(a.prefix, a.eye, a.log)
    except ValueError as e:
        print('hull layer compare: %s' % e)
        return 2
    print(report(r))
    return {'CONFIRMED': 0, 'TAKEN OUT': 0, 'REFUTED': 3, 'NOT TAKEN OUT': 3}.get(r['verdict'], 1)


if __name__ == '__main__':
    sys.exit(main())
