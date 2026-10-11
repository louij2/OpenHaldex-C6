# Gen5 0AY (generation 51): "every forced mode stays FWD"

Status: **under investigation, no fix released.** The candidate fixes change how
a real car's AWD engages, so they are not going into a release until someone has
driven them.

## The report

An Audi S1 (PQ chassis, EA888.3, Gen5 0AY — generation 51) on V9.00.0:

- forcing **any** mode leaves the car front-wheel drive;
- the engagement gauge shows lock anyway;
- disabling steering lock changes nothing;
- unplugging the controller (stock Haldex) feels correct.

## Ruled out

**Steering scale.** `apply_steering_scale()` in
`src/OpenHaldexC6_Calculations.cpp` gates on

```c
const bool supported = (haldexGeneration == 2 || haldexGeneration == 4 ||
                        haldexGeneration == 50 || haldexGeneration == 52);
if (!supported) return 100.0f;
```

Generation 51 is absent, so steering scale never acted on this car at all. The
reporter also disabled it with no change.

**Frame-edit plumbing.** `frameEditGenIdx(51)` returns `FE_GEN_51`, and that
generation's default mask `0x0F` (bits 0–3) is exactly the four lock-modulated
frames: Motor_1 (0x280), Bremse_3 (0x4A0), Bremse_4 (0x2A0), LW_1 (0x0C2). The
command path is wired correctly.

## Candidate 1 — the V9 lock-gate regression (most likely)

Every mode except Stock and FWD passes through `lock_enabled()`:

```c
static inline bool lock_enabled()
{
  const bool throttle_ok = (state.pedal_threshold == 0) || (int(received_pedal_value) >= state.pedal_threshold);
  const bool under_ok = (disengageUnderSpeed == 0) || (received_vehicle_speed >= disengageUnderSpeed);
  const bool above_ok = (disengageAboveSpeed == 0) || (received_vehicle_speed <= disengageAboveSpeed);
  return throttle_ok && under_ok && above_ok;
}
```

V9.00.0's own changelog records the behaviour change:

> "Disengage Under/Above Speed" + "Minimum Throttle" now gate EVERY lock path:
> force-mode triggers (TC/hazard/ext button) previously bypassed the gate in
> get_lock_target_adjustment() so lock_target read 100% below the cut-off;
> **Expert mode previously bypassed it entirely.**

So a user who had a non-zero *Disengage Under Speed* or *Minimum Throttle* and
relied on Expert or a force mode got lock on 8.00.3 and **zero lock on 9.00.0**,
in every mode, with settings that look unchanged. That matches the report
exactly, including why it survives every mode switch.

The change itself is defensible — the gate arguably *should* apply everywhere.
The fault is that it silently inverted behaviour for existing saved settings and
gives no indication why lock is being withheld.

**Test:** set *Disengage Under Speed*, *Disengage Above Speed* and *Minimum
Throttle Before Lock* all to 0 and retry. If lock returns, this is it.

**Likely fix:** surface the gate on the dashboard — when lock is zero because
`lock_enabled()` is false, say so and name the gate responsible, rather than
showing a bare 0. Possibly also warn once on upgrade when a non-zero gate is
inherited.

## Candidate 2 — the engagement gauge reads the wrong byte

Generation 51 decodes engagement from Allrad_1 (0x2C0) **byte 1**, mapped
128..255, snapping ≥99 % to 100 %:

```c
case 51:
  if (rx_message_hdx.identifier == HALDEX_ID) {
    received_haldex_engagement_raw = rx_message_hdx.data[1];
    received_haldex_engagement = map(received_haldex_engagement_raw, 128, 255, 0, 100);
```

But generation 2 reads the *rear* clutch from **byte 4** of the same frame, and
its comment says byte 1 is something else entirely:

```c
case 2:
  // SG_ Kupplungssteifigkeit_Hinten__Ist : 32|8@1+ (0.7874,0) [0|100] "%" -> byte 4
  // Byte 1 is centre-clutch torque-rate (Nm/min), unrelated to engagement.
```

Generation 51's own comment concedes the DBC calls byte 1
`Kupplungssteifigkeit_Mitte`. The two decodes are mutually exclusive and the
byte ranges differ (0..127 vs 128..255), which suggests byte 1 was adopted
empirically because it happened to sit in that range.

If byte 1 is not rear engagement, the gauge can read locked while the rear
clutch is open — **"the gauge locks up but the car stays FWD"**. That makes the
gauge useless as evidence here, and it also feeds `MODE_STOCK`, which returns
`received_haldex_engagement` as the lock target.

**Needs:** a SavvyCAN capture of 0x2C0 while cycling modes, to see which byte
tracks actual engagement. Do not change the decode on a guess — it alters Stock
passthrough behaviour.

## Candidate 3 — gen 51 lost lock modulation that gen 4 has

On the same PQ bus, generation 4 modulates three more bytes than generation 51:

| Frame | Gen 4 | Gen 51 |
|---|---|---|
| Bremse_1 (0x1A0) byte 4 | `get_lock_target_adjusted_value(0xFE)` | `0xFE` constant |
| Bremse_1 (0x1A0) byte 5 | `get_lock_target_adjusted_value(0xFE)` | `0xFE` constant |
| Bremse_2 (0x5A0) byte 4 | `get_lock_target_adjusted_value(0x7F)` | `0x7F` constant |

Generation 51's edits appear to have been copied from the standalone bench frame
generator (which uses literals, since there is no lock to modulate on a bench)
rather than from generation 4's passthrough edits. Gen 51's Bremse_1 is also off
by default (bit 4, outside mask `0x0F`) where gen 4's is on (bit 2, inside
`0x3F`). So gen 51 commands lock through three frames where gen 4 uses five.

Note Bremse_3's spoofed wheel speeds are byte-identical between gen 4 and gen
51, so those are not a gen-51 regression — though it is worth noting that all
PQ generations replace the real four-wheel speeds with a fixed ~10 km/h pattern
in which the rears (0xD2) read faster than the fronts (0xB6/0xCC).

## Order of work

1. Confirm or kill candidate 1 — it is a settings change, costs nothing, and
   explains the report on its own.
2. If lock returns, ship the gate-visibility fix (safe: UI and diagnostics only).
3. Only then touch candidates 2 and 3, each needing a CAN capture and a road
   test, one change at a time.
