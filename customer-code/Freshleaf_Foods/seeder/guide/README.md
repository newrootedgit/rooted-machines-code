# GUIDE project — Laser Gate Seeder, Conveyor Mount (TE-FX Flushmount)

The HMI half of this machine. `tabletop_seeder_poll.py` addresses the Touch
Encoder by raw screen and variable ID, so **this project and that script are one
contract split across two files**. Change a screen or variable number here and
poll does not error — it silently stops working. Change both together.

| File | What it is |
| --- | --- |
| `…_export.zip` | The flashable artifact. This is what goes on the encoder. |
| `…_guide.zip` | The GUIDE project source. Open this to edit. |

Both are as of **2026-08-19**, the revision that added the Shut Down button.
Deployed to the CM5 at `100.119.67.117` the same day.

```
export  sha256 06cf6a514756c9645b13a7af831e70017c8b8be5646d7bf38ade830608d66e7d  (257646 bytes)
guide   sha256 59c857fbc767fa56bbd59896bcff892472b688e59af306d187078b3d5cc8a2e0  (487122 bytes)
```

The **export** hash is the one that means anything — it is the artifact that was
flashed, and it is reproducible. Do not expect the **guide** hash to be stable:
`project.guide` inside it is encrypted and GUIDE re-encrypts on every save, so
saving the same project twice yields two entirely different blobs of identical
length. Two guide zips that differ are not evidence the project changed, and two
that match cannot happen.

Practical consequence: **git gives this directory backup, not review.** Nothing
can diff an encrypted project. The date and the summary below are the only
record of what a revision actually contains — keep them accurate or the history
is worthless.

## Flashing it

From the Pi, over SSH — no Windows machine needed on site:

```bash
sudo systemctl stop seeder_poll.service     # REQUIRED: poll holds the HID device open
/home/rooted/te-cli/venv/bin/te ls
/home/rooted/te-cli/venv/bin/te update --hid /home/rooted/seeder_project.zip
sudo systemctl start seeder_poll.service
/home/rooted/te-cli/venv/bin/te ls          # Proj Info build number must have changed
```

`te update --all`'s help text is a copy-paste bug — it describes `--can`. Use
`--hid`.

## Screen and variable map

Every ID poll depends on. The constant name is what to change in the script if
the project renumbers something.

### Screen 2 — Shut Down

| Var | Type | Poll constant | Purpose |
| --- | --- | --- | --- |
| 5 | **numeric** | `SHUTDOWN_BTN_PRESS_VAR` | Button latch. GUIDE Action="Set value" writes 1; poll writes 0 back to re-arm. |
| 2 | **string** | `SHUTDOWN_STATUS_VAR` | Progress text: `Shut Down` idle → `Shutting down…` → `HALT FAILED`. |

Var 5 must be numeric — `safe_get_var` calls `.to_int()` and a string makes the
read throw, which the branch swallows as "not pressed". The button then does
nothing, silently.

`SHUTDOWN_TEXT_IDLE` must match the static label on the var 2 field, or the text
visibly vanishes a moment after the operator opens the screen.

### Screen 10 — Variety selection

| Var | Type | Poll constant | Purpose |
| --- | --- | --- | --- |
| 1 | numeric | — | Selection index, scrolled by the encoder. Clamp to 1–20 in GUIDE. |
| 7 | string | `VARIETY_NAME_VAR` | Variety name, mirrored from var 1 by poll. |

### Screen 18 — Run screen

| Var | Type | Poll constant | Purpose |
| --- | --- | --- | --- |
| 1 | string | `RUNNING_VARIETY_VAR` | Loaded variety name. |
| 2 | string | `STATE_STATUS_VAR` | `Active` / `Paused`, mirrors `ready_to_run`. |
| 5 | **numeric** | `STATE_BTN_PRESS_VAR` | Pause/Activate latch, same pattern as screen 2 var 5. |

Screen 18 var 2 and screen 2 var 2 share an id and are unrelated.

### Screen 19 — Calibration

| Var | Type | Poll constant | Purpose |
| --- | --- | --- | --- |
| 3 | string | `CAL_STATUS_VAR` | Progress text. **Clips at ~11 characters.** |
| 4 | **numeric** | `CAL_TRIGGER_VAR` | Calibrate latch. |

### Value entry screens — all var 1, numeric

| Screen | Field | Range |
| --- | --- | --- |
| 6 | Roller speed | 0 … 50 |
| 11 | Irrigation delay | −100 … 100 |
| 12 | Irrigation duration | −100 … 100 |
| 13 | Misting delay | −100 … 100 |
| 14 | Misting duration | −100 … 100 |
| 15 | Roller delay | −100 … 100 |
| 16 | Roller duration | −100 … 100 |

The delay and duration fields are **offsets and legitimately go negative**. A
GUIDE widget limited to 0 and above silently destroys half of every operator's
range. Confirm the widget limits match the `variable_ranges` block in
`TE_Variable_Values.json` — poll validates against the file, not against its own
defaults.

`belt_speed` has a declared range but **no screen** — the conveyor runs off its
own control, not the encoder.

### Navigation-only screens

| Screen | Purpose |
| --- | --- |
| 9 | Save confirmation — poll reads screens 6/10–16 and writes the preset. |
| 17 | Operator variety confirmation — poll saves `active_variety`, jumps to 18. |

## When editing this project

1. Keep every ID above, or change the matching constant in
   `tabletop_seeder_poll.py` in the same commit.
2. Numeric latches must stay numeric.
3. Re-export, replace **both** zips here, update the hashes and date above, and
   say in the commit message what changed on the HMI — that message is the only
   diff anyone will ever get.
4. Flash, then walk screens 2, 10, 18, 19 and confirm poll still drives each.
