#!/usr/bin/env python3
"""Does the private hull layer light like the game? (diag.hull_layer, Step 1; docs/research-own-hull-layer-2026-11-08.md "Step 1 build")

Usage: python tools/hull_layer_compare.py <edvr_logs\\eyes\\eye_<stamp>> [--eye 1] [--tol 0.05] [--clear-depth 0]
       python tools/hull_layer_compare.py --self-test

Reads eye_<stamp>_HullHdr.bin (the private HDR right after the private sun draw), eye_<stamp>_GameHdrSun.bin (the
game's HDR right after its own sun draw) and eye_<stamp>_HullDepth.bin (the private depth); --eye 1 reads the _e1
files. Hull pixels are where the private depth is not the clear value (reversed Z: 0). On them both images hold
emissive plus sun and ambient only, so they should agree, except where the game's AO (none on the layer) or the hull
decals (not re-issued in Step 1) differ.

Prints the hull pixel count, the fraction within --tol of the game's luma, the median and p95 ratio private/game,
the black fraction (private dark where the game is lit), and the best whole-pixel shift within 3 px.
Verdict and exit code: CONFIRMED 0 (median ratio 0.95..1.35 and at least half the hull pixels within --tol),
REFUTED 3 (under 500 hull pixels, more than half black, or a shift that matches 0.2 better than none),
INCONCLUSIVE 1, bad input 2.
"""
import argparse
import os
import struct
import sys

MIN_HULL = 500
SHIFT = 3


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
    paths = {k: '%s_%s%s.bin' % (prefix, k, suffix) for k in ('HullHdr', 'GameHdrSun', 'HullDepth')}
    for k, p in paths.items():
        if not os.path.exists(p):
            raise ValueError('missing %s: %s' % (k, p))
    _, hull = eye_inputs.read(paths['HullHdr'])
    _, game = eye_inputs.read(paths['GameHdrSun'])
    _, depth = eye_inputs.read(paths['HullDepth'])
    if hull.ndim != 3 or game.ndim != 3 or hull.shape != game.shape or depth.shape != hull.shape[:2]:
        raise ValueError('the three files do not share one eye size (%s, %s, %s)' % (hull.shape, game.shape, depth.shape))
    mask = depth != np.float32(clear_depth)
    lh, lg = luma(np.nan_to_num(hull)), luma(np.nan_to_num(game))
    n = int(mask.sum())
    out = {'hull_pixels': n, 'eye': eye, 'tol': tol}
    if n < MIN_HULL:
        out.update(verdict='REFUTED', why='the private depth holds %d hull pixels (under %d): the block was not drawn into it' % (n, MIN_HULL))
        return out
    lit = mask & (lg > 1e-4)
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


def report(r):
    line = 'hull layer compare: eye %d, %d hull pixel(s)' % (r['eye'], r['hull_pixels'])
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

    def case(name, priv_rgb, game_rgb, depth, want, eye=0):
        with tempfile.TemporaryDirectory() as temp:
            prefix = os.path.join(temp, 'eye_123456789001')
            sfx = '_e1' if eye else ''
            h, w = depth.shape
            write(prefix + '_HullHdr%s.bin' % sfx, 26, eye, pack_r11g11b10(priv_rgb).tobytes(), w, h, 4)
            write(prefix + '_GameHdrSun%s.bin' % sfx, 26, eye, pack_r11g11b10(game_rgb).tobytes(), w, h, 4)
            d = np.zeros((h, w, 2), dtype=np.float32)
            d[..., 0] = depth
            write(prefix + '_HullDepth%s.bin' % sfx, 19, eye, d.tobytes(), w, h, 8)
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
        print('hull layer compare self-test passed (7 cases)')
    return 1 if failures else 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('prefix', nargs='?', help='the eye run prefix: <edvr_logs>\\eyes\\eye_<stamp>')
    p.add_argument('--eye', type=int, default=0, choices=(0, 1))
    p.add_argument('--tol', type=float, default=0.05, help='relative luma tolerance (default 0.05: R11G11B10 rounds to about 1.6%%)')
    p.add_argument('--clear-depth', type=float, default=0.0, help='the private depth\'s clear value (reversed Z: 0)')
    p.add_argument('--self-test', action='store_true')
    a = p.parse_args(argv)
    if a.self_test:
        return self_test()
    if not a.prefix:
        p.error('give the eye run prefix, or --self-test')
    try:
        r = compare(a.prefix, eye=a.eye, tol=a.tol, clear_depth=a.clear_depth)
    except ValueError as e:
        print('hull layer compare: %s' % e)
        return 2
    print(report(r))
    return {'CONFIRMED': 0, 'REFUTED': 3}.get(r['verdict'], 1)


if __name__ == '__main__':
    sys.exit(main())
