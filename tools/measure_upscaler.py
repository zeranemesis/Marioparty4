"""Measures what a temporal upscaler did, from the frames the engine dumps.

Usage: python tools/measure_upscaler.py <run directory> [<run directory> ...]

AURORA_UPSCALE_DUMP=N makes the upscaler host write, for N consecutive frames
once armed: the frame it was given (upscale_in_NNN.bmp, render size), the frame
it produced (upscale_out_NNN.bmp, window size) and one line per frame in
upscale_dump.txt with the sub-pixel offset the frame was drawn with.

Two runs never show the same picture, so nothing here compares one run with
another. Everything is measured against a reference built from the run's own
input frames: each input pixel is a sample of the scene taken a known fraction
of a pixel off its centre, so N frames of a still picture are N samples per
pixel at known places, and laying them down where they belong on the finer
grid rebuilds the picture at the window's size. That is what a temporal
upscaler does, without the heuristics that let it cope with motion; on what
holds still it is the answer it should reach.

It also settles the sign of the offset without any SDK: samples laid where
they were really taken agree with their neighbours on the same pixel, and laid
down with the offsets negated they do not. The spread of the samples that land
on one pixel is what tells the two apart; the sharpness of the rebuilt picture
does not, at high ratios, where misplaced samples read as detail.

Only what holds still is measured. A block of the picture counts as still when
its mean brightness does not change over the frames dumped.
"""
import os
import sys

import numpy as np
from PIL import Image

LUMA = np.array([0.299, 0.587, 0.114], dtype=np.float32)


def load(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.float32) / 255.0


def read_dump(run):
    """upscale_dump.txt: 'index frameId jitterX jitterY renderW renderH outW outH reset' per line."""
    frames = []
    with open(os.path.join(run, 'upscale_dump.txt'), encoding='utf-8') as handle:
        for line in handle:
            parts = line.split()
            if len(parts) < 9 or parts[0].startswith('#'):
                continue
            frames.append({
                'index': int(parts[0]),
                'frame': int(parts[1]),
                'jitter': (float(parts[2]), float(parts[3])),
                'render': (int(parts[4]), int(parts[5])),
                'output': (int(parts[6]), int(parts[7])),
                'reset': int(parts[8]) != 0,
            })
    return frames


def longest_shot(images):
    """[first, last) of the longest run of frames with no cut in it.

    A cut is a frame that differs from the one before it far more than frames
    of this dump usually do, and by a tenth of the range at least on average.
    """
    luma = [image @ LUMA for image in images]
    change = np.array([np.abs(b - a).mean() for a, b in zip(luma, luma[1:])])
    if len(change) == 0:
        return 0, len(images)
    cuts = [i + 1 for i, c in enumerate(change) if c > 0.1 and c > 4.0 * np.median(change)]
    bounds = [0] + cuts + [len(images)]
    spans = [(bounds[i], bounds[i + 1]) for i in range(len(bounds) - 1)]
    return max(spans, key=lambda span: span[1] - span[0])


def still_blocks(stack, block):
    """True per block whose mean brightness holds still over the stack (frames, h, w, 3)."""
    luma = stack @ LUMA
    frames, h, w = luma.shape
    bh, bw = h // block, w // block
    means = luma[:, :bh * block, :bw * block].reshape(frames, bh, block, bw, block).mean(axis=(2, 4))
    return means.std(axis=0) < 0.0025


def blocks_to_mask(blocks, block, shape, scale_y, scale_x):
    """The block mask of the render-size picture, as a pixel mask of the window-size one."""
    h, w = shape
    ys = np.minimum((np.arange(h) / scale_y / block).astype(int), blocks.shape[0] - 1)
    xs = np.minimum((np.arange(w) / scale_x / block).astype(int), blocks.shape[1] - 1)
    mask = blocks[np.ix_(ys, xs)]
    # A margin: the edge of the picture has no neighbours to rebuild from.
    margin = 8
    mask[:margin] = False
    mask[-margin:] = False
    mask[:, :margin] = False
    mask[:, -margin:] = False
    return mask


def rebuild(inputs, jitters, out_shape, sign):
    """Lays every input sample down where it was taken, on the window's grid.

    An input pixel's centre p shows the scene point that, without the offset,
    would have been drawn at p - sign * offset. Each sample is spread over the
    four pixel centres around its place with tent weights.
    """
    out_h, out_w = out_shape
    in_h, in_w = inputs[0].shape[:2]
    scale_y = out_h / in_h
    scale_x = out_w / in_w
    total = np.zeros((out_h, out_w, 3), dtype=np.float64)
    weight = np.zeros((out_h, out_w), dtype=np.float64)
    # Brightness and its square, for the spread of what lands on each pixel.
    first = np.zeros((out_h, out_w), dtype=np.float64)
    second = np.zeros((out_h, out_w), dtype=np.float64)
    py, px = np.mgrid[0:in_h, 0:in_w]
    for image, (jx, jy) in zip(inputs, jitters):
        luma = image @ LUMA
        # Place in window pixels, then relative to pixel centres.
        qx = (px + 0.5 - sign * jx) * scale_x - 0.5
        qy = (py + 0.5 - sign * jy) * scale_y - 0.5
        x0 = np.floor(qx).astype(int)
        y0 = np.floor(qy).astype(int)
        fx = qx - x0
        fy = qy - y0
        for dy in (0, 1):
            for dx in (0, 1):
                w = (fx if dx else 1.0 - fx) * (fy if dy else 1.0 - fy)
                xi = x0 + dx
                yi = y0 + dy
                ok = (xi >= 0) & (xi < out_w) & (yi >= 0) & (yi < out_h)
                np.add.at(weight, (yi[ok], xi[ok]), w[ok])
                np.add.at(first, (yi[ok], xi[ok]), (w * luma)[ok])
                np.add.at(second, (yi[ok], xi[ok]), (w * luma * luma)[ok])
                for c in range(3):
                    np.add.at(total[:, :, c], (yi[ok], xi[ok]), (w * image[:, :, c])[ok])
    covered = weight > 0.25
    out = np.zeros_like(total)
    out[covered] = total[covered] / weight[covered][:, None]
    spread = np.zeros_like(weight)
    mean = first[covered] / weight[covered]
    spread[covered] = np.sqrt(np.maximum(second[covered] / weight[covered] - mean * mean, 0.0))
    return out.astype(np.float32), covered, spread


def sharpness(image, mask):
    """Mean size of the brightness gradient where the mask holds."""
    luma = image @ LUMA
    gx = np.zeros_like(luma)
    gy = np.zeros_like(luma)
    gx[:, 1:-1] = (luma[:, 2:] - luma[:, :-2]) * 0.5
    gy[1:-1, :] = (luma[2:, :] - luma[:-2, :]) * 0.5
    return float(np.sqrt(gx * gx + gy * gy)[mask].mean())


def psnr(image, reference, mask):
    error = ((image - reference) ** 2)[mask].mean()
    return 99.0 if error <= 0 else float(10.0 * np.log10(1.0 / error))


def measure(run):
    frames = read_dump(run)
    if len(frames) < 4:
        print('%s: %d frames dumped, too few to measure' % (run, len(frames)))
        return False
    render_w, render_h = frames[0]['render']
    out_w, out_h = frames[0]['output']
    frames = [f for f in frames if f['render'] == (render_w, render_h) and f['output'] == (out_w, out_h)]
    inputs = [load(os.path.join(run, 'upscale_in_%03d.bmp' % f['index'])) for f in frames]
    outputs = [load(os.path.join(run, 'upscale_out_%03d.bmp' % f['index'])) for f in frames]
    print('%s' % run)
    # A cut in the middle of the dump leaves nothing still across it: only the
    # longest stretch without one is measured.
    first, last = longest_shot(inputs)
    if (first, last) != (0, len(frames)):
        print('  a cut in the dump: frames %d to %d are measured, the longest stretch without one'
              % (frames[first]['index'], frames[last - 1]['index']))
        frames = frames[first:last]
        inputs = inputs[first:last]
        outputs = outputs[first:last]
        if len(frames) < 8:
            print('  too few frames between cuts to measure')
            return False
    jitters = [f['jitter'] for f in frames]
    scale_y = out_h / render_h
    scale_x = out_w / render_w

    block = 8
    blocks = still_blocks(np.stack(inputs), block)
    mask = blocks_to_mask(blocks, block, (out_h, out_w), scale_y, scale_x)
    moving = blocks_to_mask(~blocks, block, (out_h, out_w), scale_y, scale_x)

    spread = np.ptp(np.array(jitters), axis=0)
    print('  %d frames, %dx%d to %dx%d (%.2f per side); offsets span %.2f x %.2f pixel; %d history resets'
          % (len(frames), render_w, render_h, out_w, out_h, scale_x, spread[0], spread[1],
             sum(1 for f in frames if f['reset'])))

    plain = [np.asarray(Image.fromarray((image * 255.0 + 0.5).astype(np.uint8)).resize(
        (out_w, out_h), Image.BILINEAR), dtype=np.float32) / 255.0 for image in inputs]

    # What moves has no reference to be rebuilt from: a sample taken last frame
    # is of another place. It is measured against the plain enlargement of the
    # same frame instead, which says less -- how much detail the output holds
    # next to it, and how far the two are apart -- but is what tells motion
    # handed over the right way from motion handed over the wrong way: history
    # put in the wrong place is either thrown away or smeared.
    measured = False
    if moving.mean() >= 0.05:
        sharp_out = np.mean([sharpness(o, moving) for o in outputs])
        sharp_plain = np.mean([sharpness(p, moving) for p in plain])
        apart = np.mean([psnr(o, p, moving) for o, p in zip(outputs, plain)])
        print('  in motion: %.1f%% of the picture; sharpness of the output %.5f, %.0f%% of the plain '
              'enlargement of the same frame; the two agree to %.2f dB'
              % (100.0 * moving.mean(), sharp_out, 100.0 * sharp_out / max(sharp_plain, 1e-9), apart))
        measured = True

    print('  still: %.1f%% of the picture' % (100.0 * mask.mean()))
    if mask.mean() < 0.05:
        print('  too little of the picture holds still to rebuild a reference from')
        return measured

    if spread.max() < 0.05:
        print('  no sub-pixel offset: nothing to rebuild a reference from')
        print('  sharpness: output %.5f, plain enlargement %.5f'
              % (np.mean([sharpness(o, mask) for o in outputs]),
                 np.mean([sharpness(p, mask) for p in plain])))
        return True

    reference, covered, spread_ref = rebuild(inputs, jitters, (out_h, out_w), 1.0)
    negated, covered_negated, spread_neg = rebuild(inputs, jitters, (out_h, out_w), -1.0)
    mask &= covered & covered_negated
    sharp_ref = sharpness(reference, mask)
    apart_ref = float(spread_ref[mask].mean())
    apart_neg = float(spread_neg[mask].mean())
    print('  reference rebuilt from the inputs: the samples that land on one pixel are %.5f apart with '
          'the offsets as given, %.5f negated (%s)'
          % (apart_ref, apart_neg,
             'the sign given is the right one' if apart_ref < apart_neg * 0.98
             else 'the NEGATED sign is the right one' if apart_neg < apart_ref * 0.98
             else 'no difference: the sign cannot be told on this picture'))

    def report(name, images):
        stack = np.stack(images)
        flicker = float(stack.std(axis=0).mean(axis=2)[mask].mean())
        print('  %-18s PSNR %.2f dB (negated %.2f), sharpness %.5f (%.0f%% of the reference), '
              'flicker %.5f'
              % (name, np.mean([psnr(i, reference, mask) for i in images]),
                 np.mean([psnr(i, negated, mask) for i in images]),
                 np.mean([sharpness(i, mask) for i in images]),
                 100.0 * np.mean([sharpness(i, mask) for i in images]) / max(sharp_ref, 1e-9),
                 flicker))

    report('upscaler output', outputs)
    report('plain enlargement', plain)
    return True


def main():
    runs = sys.argv[1:]
    if not runs:
        print(__doc__)
        return 2
    ok = True
    for run in runs:
        ok = measure(run) and ok
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
