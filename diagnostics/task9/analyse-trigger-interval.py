from pathlib import Path
import re, json

p = Path(__file__).parent
r = json.loads((p / "results-SQUARE20K_ON.json").read_text())
raw = (p / "board-square20k-on.log").read_bytes()
log = raw.decode("utf-16" if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else "utf-8-sig")

def number(label):
    match = re.search(label + r"(0x[0-9a-fA-F]+|[0-9]+)", log)
    assert match, label
    return int(match[1], 0)

a, s = r["adc"], r["scope"]
first = number("TRIGGER_ON_START_BLOCKS_")
delta = a["blocks_completed"] - first
summary = {
    "code_version": "206b98a",
    "host_interval_ms": number("TRIGGER_ON_OBSERVED_HOST_MS_"),
    "start_blocks": first, "end_blocks": a["blocks_completed"],
    "delta_completed_blocks": delta, "samples_in_completed_blocks": delta * 1024,
    "equivalent_seconds_from_previous_fs_range": [delta * 1024 / (243.2 * 2048), delta * 1024 / (242.7 * 2048)],
    "mode_start": 1, "mode_end": s["trigger_mode"],
    "buttons_start": number("TRIGGER_ON_START_BUTTON_"), "buttons_end": s["button_presses"],
    "delta_matches": s["trigger_matches"] - number("TRIGGER_ON_START_MATCHES_"),
    "delta_frames": s["frames"] - number("TRIGGER_ON_START_FRAMES_"),
    "waiting": s["trigger_waiting"], "reload_timeouts": s["reload_timeouts"],
    "adc": {k: a[k] for k in ("state", "blocks_completed", "blocks_processed", "blocks_dropped", "queue_overflow_count", "stale_block_count", "ambiguous_irq_count", "sequence_error_count", "adc_overrun_count", "dma_error_count", "guard_errors", "hardfault_count", "cfsr", "hfsr")},
    "graphics": {name: {k: v for k, v in r[name].items() if any(t in k for t in ("error_count", "timeout_count", "underrun", "cfsr", "hfsr", "hardfault_count"))} for name in ("lcd", "dma2d")}
}
(p / "trigger-interval.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
print(json.dumps(summary, indent=2))
