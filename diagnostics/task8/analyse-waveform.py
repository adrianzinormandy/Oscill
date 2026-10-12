"""Offline raw ADC analysis; Fs/VREF arguments are supplied estimates, not measurements."""
import argparse
import json
import statistics
import struct
from pathlib import Path


def analyse(samples, fs_hz, vref_v):
    if len(samples) < 2 or any(x > 4095 for x in samples):
        raise ValueError('Expected at least two right-aligned 12-bit ADC samples')
    mean = statistics.mean(samples)
    pp = max(samples) - min(samples)
    hysteresis = max(2.0, pp * 0.05)
    armed = False
    crossings = []
    for i in range(1, len(samples)):
        if samples[i - 1] <= mean - hysteresis:
            armed = True
        if armed and samples[i - 1] < mean <= samples[i]:
            crossings.append(i - 1 + (mean - samples[i - 1]) /
                             (samples[i] - samples[i - 1]))
            armed = False
    periods = [b - a for a, b in zip(crossings, crossings[1:])]
    period = statistics.mean(periods) if len(periods) >= 3 else None
    period_cv = statistics.pstdev(periods) / period if period else None
    # A noise-only/DC record must not silently become a frequency measurement.
    valid = period is not None and period_cv <= 0.10 and pp >= 16
    result = {
        'samples': len(samples), 'min_code': min(samples), 'max_code': max(samples),
        'mean_code': mean, 'rms_ac_code': statistics.pstdev(samples),
        'rms_total_code': (sum(x * x for x in samples) / len(samples)) ** 0.5,
        'peak_to_peak_code': pp, 'rising_crossings': len(crossings),
        'hysteresis_code': hysteresis, 'period_samples': period,
        'period_cv': period_cv, 'periodic_estimate_valid': valid,
        'fs_argument_hz': fs_hz, 'vref_argument_v': vref_v,
        'frequency_estimate_hz': fs_hz / period if valid else None,
        'note': 'Simple crossing estimate, not proof of sample continuity or calibrated amplitude'
    }
    for key in ('min', 'max', 'mean', 'rms_ac', 'rms_total', 'peak_to_peak'):
        result[key + '_v_estimate'] = result[key + '_code'] * vref_v / 4096
    return result


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('binary', type=Path)
    ap.add_argument('--fs-hz', type=float, required=True)
    ap.add_argument('--vref-v', type=float, required=True)
    ap.add_argument('--next-write-index', type=int, help='Rotate a stopped, fully filled DMA ring')
    ap.add_argument('--json-out', type=Path)
    ap.add_argument('--csv-out', type=Path)
    args = ap.parse_args()
    raw = args.binary.read_bytes()
    if len(raw) % 2 or args.fs_hz <= 0 or args.vref_v <= 0:
        ap.error('Expected even binary length and positive Fs/VREF')
    samples = list(struct.unpack('<' + 'H' * (len(raw) // 2), raw))
    if args.next_write_index is not None:
        if not 0 <= args.next_write_index < len(samples):
            ap.error('Ring index is outside buffer')
        n = args.next_write_index
        samples = samples[n:] + samples[:n]
    result = analyse(samples, args.fs_hz, args.vref_v)
    text = json.dumps(result, indent=2) + '\n'
    print(text, end='')
    if args.json_out:
        args.json_out.write_text(text, encoding='utf-8')
    if args.csv_out:
        args.csv_out.write_text('index,adc_code\n' + ''.join(
            f'{i},{v}\n' for i, v in enumerate(samples)), encoding='ascii')
