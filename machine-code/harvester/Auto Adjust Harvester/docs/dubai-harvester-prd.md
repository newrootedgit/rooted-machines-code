# Dubai Harvester (ReFarm/GigaFarm) - Product Requirements Document

**Project:** Custom Tabletop Harvester for ReFarm GigaFarm (Dubai)
**Customer:** ReFarm / Christof Industries
**Contacts:** Valentin Kotzmaier (V.Kotzmaier@christof.com), Aljaz Klasinc (A.Klasinc@christof.com)
**Contract:** Custom Machine Development Agreement (Signed)
**Last Updated:** 2025-12-10

---

## 1. Overview

This document defines the software requirements for the Dubai Harvester, a custom tabletop harvesting machine built for ReFarm's GigaFarm facility. The machine is based on Rooted Robotics' standard harvester platform with key additions: motorized blade height adjustment, air blower/knife systems, and password-protected preset management.

---

## 2. Hardware Architecture

### 2.1 System Diagram

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           RASPBERRY PI                                   │
│  ┌──────────────────┐    ┌──────────────────┐    ┌──────────────────┐  │
│  │ Touch Encoder    │    │ JSON State File  │    │ TCP Server       │  │
│  │ Poll Script      │───▶│ (presets, ready) │◀───│ (serves state)   │  │
│  └──────────────────┘    └──────────────────┘    └────────┬─────────┘  │
│           │                                                │            │
│           │ USB                                            │ TCP/8888   │
│           ▼                                                ▼            │
│  ┌──────────────────┐                            ┌──────────────────┐  │
│  │ Grayhill Touch   │                            │ Ethernet         │  │
│  │ Encoder (HMI)    │                            │ 192.168.10.1     │  │
│  └──────────────────┘                            └────────┬─────────┘  │
└───────────────────────────────────────────────────────────┼────────────┘
                                                            │
                                                            │ Ethernet
                                                            ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                           CLEARCORE (192.168.10.2)                       │
│                                                                          │
│  ┌─────────────┐   Main Loop (Arduino-style)                            │
│  │ TCP Client  │──▶ Poll Pi for: ready_to_run, preset values            │
│  └─────────────┘                                                         │
│                                                                          │
│  INPUTS (Direct I/O):              OUTPUTS (Direct Control):            │
│  • Paddle position sensor (DI6)    • Belt motor (M1)                    │
│  • Height limit switch upper (DI7) • Blade motor (M2)                   │
│  • Height limit switch lower (DI8) • Height adjustment motor (M3)       │
│  • E-stop (built-in)               • Pre-harvest blower relay (IO0)     │
│                                    • Post-harvest air knife relay (IO1) │
│                                                                          │
│  INTERNAL LOGIC:                                                         │
│  • Air sequencing state machine (triggered by paddle sensor)            │
│  • Height limit enforcement (stop motor at limits)                      │
│  • E-stop handling                                                       │
└─────────────────────────────────────────────────────────────────────────┘
```

### 2.2 Data Flow

| Direction | Data | Purpose |
|-----------|------|---------|
| Pi → ClearCore | Preset values, ready_to_run flag | Machine configuration |
| ClearCore → Pi | Currently nothing | Future: sensor/status logging |

**Note:** ClearCore handles all real-time control locally (air sequencing, limit switches, E-stop). Pi is configuration source only.

### 2.3 Component Details

| Component | Connection | Purpose |
|-----------|------------|---------|
| Grayhill Touch Encoder | USB to Raspberry Pi | HMI interface for operators |
| Raspberry Pi | Ethernet to ClearCore, WiFi to Internet | Central hub, runs application software, enables remote updates |
| ClearCore (Teknic) | Direct motor control + relay outputs | Controls belt motor, blade motor, height motor, air system relays |

### 2.4 Hardware Specifications (from SOW)

- Tray size: 310mm x 530mm (Danish format)
- Blades: 5 straight + 5 wavy stainless steel
- Power: 220VAC, single-phase, 50Hz

### 2.5 Key Contacts

| Role | Name | Email |
|------|------|-------|
| Grayhill Engineering | Raj Patel | (contact via Grayhill support) |
| Grayhill Support | Thomas Sifuentez | tom_sifuentez@grayhill.com |
| Grayhill Software | Robert Obrochta | robert_obrochta@grayhill.com |

### 2.6 ClearCore Resources

| Resource | URL |
|----------|-----|
| Arduino Wrapper | https://teknic.com/products/io-motion-controller/clearcore-arduino-wrapper/ |
| Arduino API Reference | https://teknic-inc.github.io/ClearCore-library/ArduinoRef.html |
| User Manual (PDF) | https://teknic.com/files/downloads/clearcore_user_manual.pdf |
| GitHub Repository | https://github.com/Teknic-Inc/ClearCore-Arduino-wrapper |

---

## 3. Delta from Standard Harvester

| Feature | Standard Harvester | Dubai Machine |
|---------|-------------------|---------------|
| Belt speed control | Exists | Same |
| Blade speed control | Exists | Same |
| Blade height adjustment | Manual | **NEW: Motorized via lead screw** |
| Pre-harvest blower | None | **NEW: ClearCore/relay controlled** |
| Post-harvest air knife | None | **NEW: ClearCore/relay controlled** |
| Paddle position sensor | None | **NEW: Triggers air sequencing** |
| Height limit switches | None | **NEW: 2x for blade height range** |
| Crop presets | Exists | Same + **password protection** |
| E-stop | Exists | Same |
| Remote updates | In progress | Include |
| Web app recipe editing | In progress | Include (expanded scope) |

---

## 4. Contractual Requirements (Must Have)

These features are explicitly specified in the signed Statement of Work (Attachment A).

### 4.1 Crop Preset System

**Description:** Programmable crop presets that store machine settings for different crop types.

| Requirement | Details |
|-------------|---------|
| Preset selection | Operators can scroll through saved presets and select one to load |
| Preset parameters | Belt speed, blade speed, blade height, air system settings |
| Preset storage | Saved on Raspberry Pi, pushed to Touch Encoder on startup |

### 4.2 Password Protection

**Description:** Restrict preset editing to authorized users only.

| Requirement | Details |
|-------------|---------|
| Protected actions | Create, edit, delete presets |
| Unprotected actions | Select preset, run machine |
| Password type | Single password for all admin functions |
| Lockout policy | No lockout after failed attempts |
| Implementation | Hidden admin screens on Touch Encoder, accessible only via host command after password validation on Raspberry Pi |

**Technical Reference (Grayhill meeting 2024-10-16):**
> "There's no amount of taps, swipes, encoder movements that'll get you there without passing through this gate." - Thomas Sifuentez

Password entry screen validates input on Raspberry Pi. On success, Pi sends `set screen` command to unlock hidden admin menus.

### 4.3 Motorized Blade Height Adjustment

**Description:** Motor-controlled blade height via lead screw mechanism.

| Requirement | Details |
|-------------|---------|
| Control method | Motor driven, controlled via preset selection |
| Range limits | 2x limit switches define upper and lower bounds |
| Interface | Height value set in preset, not manually adjustable during operation |
| Motor control | ClearCore controls height adjustment motor |

### 4.4 Air System Sequencing

**Description:** Coordinated control of pre-harvest blower and post-harvest air knife to avoid interference.

| System | Purpose | Behavior |
|--------|---------|----------|
| Pre-harvest blower | Lifts leaves before cutting | ON as tray approaches blade |
| Post-harvest air knife | Separates cut product | ON after tray front passes blade |

| Requirement | Details |
|-------------|---------|
| Trigger | Paddle position sensor detects tray position |
| Sequencing | Blower and air knife never run simultaneously |
| Control | ClearCore directly or via relay |
| Timing | Calculated relative to belt speed |

**Contract Language (SOW Attachment A):**
> "To avoid interference between the post-harvest air knife and the pre-harvest blower we plan to turn on the blower as trays approach the blade, then turn it off and turn on the post-harvest one the front of the tray has passed through the blade."

**State Machine:**
```
IDLE --> BLOWER_ON --> BLOWER_OFF/AIRKNIFE_ON --> AIRKNIFE_OFF --> IDLE
         (tray approaching)  (tray past blade)      (tray cleared)
```

#### Design Consideration: Configurable Air Logic

Evaluate whether the customer should be able to configure the air system logic per preset:
- Which blowers run (blower only, air knife only, both, neither)
- Timing adjustments (delay before/after trigger)
- On/off behavior per crop type

**If configurable:**
- Preset parameters must include air system settings
- HMI screens needed for air configuration
- Validation logic required to prevent unsafe configurations (e.g., both running simultaneously)
- Consider predefined "modes" (e.g., Full Sequence, Blower Only, Air Knife Only, Disabled)

**Recommendation:** Discuss with ReFarm whether flexibility is needed or if fixed sequencing logic is acceptable.

### 4.5 Sensor Integration

| Sensor | Purpose | Connection |
|--------|---------|------------|
| Paddle position sensor | Triggers air system state changes | ClearCore input |
| Height limit switch (upper) | Prevents blade from exceeding upper bound | ClearCore input |
| Height limit switch (lower) | Prevents blade from exceeding lower bound | ClearCore input |

### 4.6 E-stop Integration

- Existing functionality from standard harvester
- E-stop cuts power to motors immediately
- No software changes required

---

## 5. Expanded Scope (Goals, Not Contractually Required)

These features are being developed for the broader product line and should be included in this project where feasible.

### 5.1 Remote Access & Monitoring

| Feature | Details |
|---------|---------|
| Network connectivity | Raspberry Pi connects to internet via WiFi |
| Remote software updates | OTA updates via AWS (Vishal's work in progress) |
| Status monitoring | Future: Machine status visible remotely |

**Technical Foundation:**
- Raspberry Pi update agent tested successfully (Dec 2025)
- Atomicity and rollback mechanisms being implemented
- AWS infrastructure for staging/production deployments

### 5.2 Web App Preset Management

| Feature | Details |
|---------|---------|
| View presets | See all saved crop presets from web app |
| Edit presets | Modify preset parameters remotely |
| Create/delete presets | Full CRUD operations from web app |
| Sync to machine | Push changes to Raspberry Pi, then to Touch Encoder |

**Technical Reference (Grayhill SDK):**
- Grayhill provides SDK for programmatic control
- Guide Application for building HMI screens
- Python library available on GitHub for communication
- Remote HMI updates possible via SDK (per Dec 2 meeting with Vishal)

---

## 6. Future Considerations (Not in Current Scope)

### 6.1 QR/RFID Automatic Crop Detection

| Concept | Details |
|---------|---------|
| Idea | External reader device communicates wirelessly to Raspberry Pi |
| Benefit | Auto-select preset based on tray QR code or RFID tag |
| Architecture | Reader → WiFi/Bluetooth → Raspberry Pi → Auto-load preset |
| Status | Not in current scope, but hardware architecture supports future addition |

### 6.2 Data Logging & Analytics

- Machine usage statistics
- Crop throughput tracking
- Maintenance scheduling based on usage
- Not in current scope

---

## 7. Technical Implementation Notes

### 7.1 Existing Codebase Analysis

**Repos Analyzed (READ ONLY - do not modify directly):**

| Repo | Location | Purpose |
|------|----------|---------|
| `Pi_tabletop_seeder` | `C:\Users\Max\software_projects\Pi_tabletop_seeder\` | Primary Pi reference - poll script + TCP server |
| `tabeltop_seeder` | `C:\Users\Max\software_projects\tabeltop_seeder\` | Older version with ClearCore + Pi + Touch Encoder |
| `raspberry-pi-setup` | `C:\Users\Max\software_projects\raspberry-pi-setup\` | Setup scripts, systemd services |
| `remote-updates-AWS` | `C:\Users\Max\software_projects\remote-updates-AWS\` | AWS OTA update agent (C++) |

### 7.2 Reusable Code from Pi_tabletop_seeder

**`tabletop_seeder_poll.py` (727 lines) - Direct reuse patterns:**

| Function | Purpose | Reuse for Dubai |
|----------|---------|-----------------|
| `discover_te_blocking()` | Finds Touch Encoder via USB | ✅ Direct reuse |
| `safe_get_var(screen_id, var_id)` | Reads variable with retries, switches screen on ERROR | ✅ Direct reuse |
| `set_variable(screen_id, var_id, value)` | Sets variable and verifies | ✅ Direct reuse |
| `locked_atomic_write_json()` | Thread-safe JSON persistence | ✅ Direct reuse |
| `save_variety_data()` | Persists preset params to JSON | ✅ Adapt for expanded schema |
| `restore_vars_if_reset()` | Pushes saved state to encoder at startup | ✅ Direct reuse |
| `monitor_touch_encoder_loop()` | Main loop watching screens | ✅ Adapt screen IDs |

**`tabletop_seeder_tcp_server.py` (261 lines) - Direct reuse:**

| Component | Purpose | Reuse for Dubai |
|-----------|---------|-----------------|
| TCP server on `192.168.10.1:8888` | Serves state to ClearCore | ✅ Direct reuse |
| CSV payload format | `ready_to_run,preset_values...` | ✅ Expand for new fields |
| `load_state()` | Reads JSON, returns flat dict | ✅ Direct reuse |

### 7.3 Reusable Patterns from ClearCore Seeder

**`tray_seeder_watering_misting_te_pi.ino` (405 lines) - Key patterns:**

**Main Loop Structure (Arduino-style):**
```cpp
void loop() {
    // 1. Poll TCP server for settings
    pollServerForSettings();

    // 2. Calculate timing based on belt speed + geometry
    float irrigation_start_time = (distance_irrigation_start / belt_speed) * 1000;

    // 3. Watch trigger, start sequence when ready
    if (ready_to_run_flag && digitalRead(DI6) == HIGH) {
        start_sequence();
    }

    // 4. Run time-based state machine
    run_sequence_state_machine();

    // 5. Wait for end sensor, stop belt
    if (sequence_active && digitalRead(DI7) == HIGH) {
        stop_belt();
    }
}
```

**Time-Based Sequencing Pattern:**
```cpp
// EXACT pattern needed for Dubai air sequencing!
float distance_to_trigger = 0.635 - tray_length;  // meters
float trigger_time_ms = (distance_to_trigger / belt_speed) * 1000;

if (elapsed_time >= trigger_time_ms && state == WAITING_FOR_TRIGGER) {
    activate_output();
    state = OUTPUT_ACTIVE;
}
```

**Motor Control Helpers:**
```cpp
void BeltMoveVelocity(float velocity) {
    ConnectorM1.MoveVelocity(velocity);
    while (!ConnectorM1.StepsComplete() && !ConnectorM1.AlertReg().bit.MotionCanceledInAlert) {
        // Wait for completion or alert
    }
}
```

**TCP Client Reconnect:**
```cpp
void pollServerForSettings() {
    if (!client.connected()) {
        client.connect(serverIp, PORT_NUM);
    }
    if (client.connected()) {
        client.println("GET_SETTINGS");
        // Read response...
    }
}
```

### 7.4 Dubai ClearCore Pin Mapping (Proposed)

| Pin | Type | Purpose |
|-----|------|---------|
| `ConnectorM1` | Motor | Belt Motor |
| `ConnectorM2` | Motor | Blade Motor |
| `ConnectorM3` | Motor | Height Adjustment Motor |
| `DI6` | Digital Input | Paddle Position Sensor (air trigger) |
| `DI7` | Digital Input | Height Limit Switch (Upper) |
| `DI8` | Digital Input | Height Limit Switch (Lower) |
| `IO0` | Relay Output | Pre-harvest Blower |
| `IO1` | Relay Output | Post-harvest Air Knife |

### 7.5 Dubai Preset Schema (Expanded from Seeder)

**Current Seeder Schema:**
```json
{
  "ready_to_run": false,
  "active_variety": 1,
  "1": {
    "roller_speed": 50,
    "belt_speed": 30,
    "irrigation_delay": 5,
    "irrigation_duration": 10,
    "misting_delay": 0,
    "misting_duration": 5,
    "roller_delay": 2,
    "roller_duration": 8
  }
}
```

**Dubai Harvester Schema (Proposed):**
```json
{
  "ready_to_run": false,
  "active_preset": 1,
  "password_hash": "<hashed_password>",
  "presets": {
    "1": {
      "name": "Lettuce",
      "belt_speed": 30,
      "blade_speed": 2,
      "blade_height": 45,
      "blower_enabled": true,
      "airknife_enabled": true,
      "blower_lead_time_ms": 500,
      "airknife_delay_ms": 200
    },
    "2": {
      "name": "Spinach",
      "belt_speed": 25,
      "blade_speed": 3,
      "blade_height": 30,
      "blower_enabled": true,
      "airknife_enabled": true,
      "blower_lead_time_ms": 600,
      "airknife_delay_ms": 250
    }
  }
}
```

### 7.6 Password Screen Implementation

Per Grayhill meeting (2024-10-16):

1. Create hidden admin menu screens in Guide Application
2. These screens are NOT navigable via normal taps/swipes
3. Create password entry screen with touch zones for digits
4. On submit, Raspberry Pi validates password
5. If valid, Pi sends `set screen X` command to Touch Encoder
6. User is now in admin menus
7. Exit returns to normal operation screens

**Key Quote:**
> "There's no amount of taps, swipes, encoder movements that'll get you there without passing through this gate." - Thomas Sifuentez, Grayhill

### 7.7 Variable Persistence

Touch Encoder does not natively persist values through power cycle. Solution:

1. Store all preset data on Raspberry Pi (file system or database)
2. On startup, Pi sends saved values to Touch Encoder
3. On preset change, Pi saves to storage AND sends to Touch Encoder
4. Touch Encoder is display/input only, not source of truth

### 7.8 Air Sequencing Logic (ClearCore Implementation)

```cpp
// Dubai harvester air sequencing - runs on ClearCore (not Pi!)
// Adapted from seeder's time-based sequencing pattern

enum AirState { IDLE, BLOWER_ON, AIRKNIFE_ON };
AirState airState = IDLE;

// Settings from Pi (via TCP)
bool blower_enabled = true;
bool airknife_enabled = true;
float blower_lead_time_ms = 500;  // Start blower this many ms before blade
float airknife_delay_ms = 200;     // Start air knife this many ms after tray passes

// Geometry constants (measure on actual machine)
float paddle_to_blade_distance = 0.15;  // meters
float tray_length = 0.53;               // meters (Danish format)

void loop() {
    // ... poll TCP, get settings ...

    // Paddle sensor triggered - tray approaching
    if (digitalRead(DI6) == HIGH && airState == IDLE) {
        sequence_start_time = millis();

        // Calculate when to start blower based on belt speed
        float time_to_blade = (paddle_to_blade_distance / belt_speed) * 1000;
        blower_on_time = sequence_start_time + time_to_blade - blower_lead_time_ms;
        airknife_on_time = sequence_start_time + time_to_blade + airknife_delay_ms;

        airState = BLOWER_ON;  // Mark sequence active
    }

    unsigned long now = millis();

    // Blower control
    if (blower_enabled && now >= blower_on_time && now < airknife_on_time) {
        digitalWrite(IO0, HIGH);  // Blower ON
    } else {
        digitalWrite(IO0, LOW);   // Blower OFF
    }

    // Air knife control (never simultaneous with blower)
    if (airknife_enabled && now >= airknife_on_time && now < airknife_off_time) {
        digitalWrite(IO1, HIGH);  // Air knife ON
    } else {
        digitalWrite(IO1, LOW);   // Air knife OFF
    }

    // Sequence complete when tray clears
    if (/* tray cleared detection */) {
        airState = IDLE;
    }
}
```

### 7.9 Grayhill Resources

| Resource | Location |
|----------|----------|
| SDK | Grayhill developer portal |
| Guide Application | Download from Grayhill (GUI for building HMI screens) |
| Python library | GitHub (from Grayhill - `te-cli`) |
| Documentation | docs.grayhill.com (touch encoder section) |

---

## 8. Open Questions

1. **Air system configurability:** Does ReFarm need per-preset air logic configuration, or is fixed sequencing acceptable?

2. **Preset count:** Is there a maximum number of presets needed? (Affects storage and UI design)

3. **Password management:** Should the password be changeable by the customer? If so, how?

4. **Web app authentication:** How should web app access be secured? (Separate from HMI password?)

5. **Offline operation:** How should the machine behave if WiFi is unavailable? (Should be fully functional offline)

---

## 9. Acceptance Criteria

### Contractual (Must Pass)

- [ ] Operator can select from saved crop presets
- [ ] Preset loads correct belt speed, blade speed, blade height
- [ ] Password required to access preset editing
- [ ] Blade height adjusts to preset value within limit switch bounds
- [ ] Pre-harvest blower activates as tray approaches blade
- [ ] Post-harvest air knife activates after tray passes blade
- [ ] Blower and air knife never run simultaneously
- [ ] E-stop immediately halts all motor operation

### Expanded Scope (Goals)

- [ ] Machine connects to internet via WiFi
- [ ] Software can be updated remotely
- [ ] Presets can be viewed/edited from web app
- [ ] Web app changes sync to machine

---

## 10. Implementation To-Do List

### Phase 1: Setup & Preparation

- [ ] **1.1** Set up development environment
  - Install ClearCore Arduino wrapper and libraries
  - Install Grayhill Guide Application
  - Set up Pi development environment (te-cli, Python dependencies)

- [ ] **1.2** Create Dubai harvester repos (fork from seeder templates)
  - `dubai_harvester_pi/` - Pi poll script + TCP server
  - `dubai_harvester_clearcore/` - ClearCore firmware
  - `dubai_harvester_hmi/` - Touch Encoder Guide project

- [ ] **1.3** Get hardware measurements from mechanical team
  - Paddle sensor to blade distance
  - Belt pulley diameter / steps-per-mm
  - Blade height motor range (mm)
  - Confirm pin mapping with electrical schematic

### Phase 2: ClearCore Firmware

- [ ] **2.1** Port seeder ClearCore base structure
  - TCP client connection to Pi
  - Main loop structure
  - Motor control helpers

- [ ] **2.2** Implement motor control
  - Belt motor (M1) - velocity control
  - Blade motor (M2) - speed levels (clean/low/high)
  - Height motor (M3) - position control with limit switch safety

- [ ] **2.3** Implement height limit switch logic
  - DI7 upper limit - stop motor immediately
  - DI8 lower limit - stop motor immediately
  - Prevent movement past limits in either direction

- [ ] **2.4** Implement air sequencing state machine
  - Paddle sensor (DI6) trigger detection
  - Time-based blower ON/OFF (IO0)
  - Time-based air knife ON/OFF (IO1)
  - Ensure mutual exclusion (never both on)

- [ ] **2.5** Implement TCP message parsing
  - Parse expanded preset values from Pi
  - Extract: belt_speed, blade_speed, blade_height, air settings
  - Handle ready_to_run flag

### Phase 3: Raspberry Pi Software

- [ ] **3.1** Fork and adapt poll script from seeder
  - Update JSON schema for Dubai presets
  - Add new screen/variable mappings
  - Implement password validation logic

- [ ] **3.2** Implement password screen handling
  - Watch for password entry screen
  - Validate password against stored hash
  - Send `set screen X` command on success
  - Return to normal screens on failure/exit

- [ ] **3.3** Fork and adapt TCP server
  - Expand CSV payload for new fields
  - Add air system settings to payload
  - Test with ClearCore parsing

- [ ] **3.4** Update JSON persistence
  - Expanded preset schema (Section 7.5)
  - Password hash storage
  - Migration from old schema if needed

### Phase 4: Touch Encoder HMI

- [ ] **4.1** Design screen flow in Guide Application
  - Main operation screens (select preset, run)
  - Password entry screen
  - Hidden admin screens (create/edit/delete presets)
  - Preset parameter screens (belt, blade, height, air)

- [ ] **4.2** Create password entry screen
  - Numeric keypad touch zones
  - Submit button reports to Pi
  - Clear/cancel functionality

- [ ] **4.3** Create admin preset editor screens
  - Belt speed adjustment (20 levels)
  - Blade speed selection (3 levels)
  - Blade height adjustment
  - Air system toggles and timing
  - Save/cancel buttons

- [ ] **4.4** Configure event notifications
  - Enable "report to host" for all interactive elements
  - Map screen IDs and variable IDs
  - Document mapping in code comments

### Phase 5: Integration & Testing

- [ ] **5.1** Bench test components individually
  - Pi ↔ Touch Encoder communication
  - Pi ↔ ClearCore TCP communication
  - ClearCore motor control without machine

- [ ] **5.2** Integration test on machine
  - Full preset selection flow
  - Password protection flow
  - All motor movements
  - Air sequencing with actual sensors

- [ ] **5.3** Test edge cases
  - E-stop behavior
  - Height limit switch enforcement
  - Power cycle recovery (preset persistence)
  - Invalid password attempts

- [ ] **5.4** Document testing results
  - Acceptance criteria checklist (Section 9)
  - Any deviations or issues found
  - Performance measurements

### Phase 6: Remote Updates & Web App (Expanded Scope)

- [ ] **6.1** Integrate AWS OTA agent
  - Deploy remote-updates-AWS agent to Pi
  - Test update workflow
  - Configure staging/production channels

- [ ] **6.2** Web app preset management (if time permits)
  - API endpoint for preset CRUD
  - Sync mechanism to Pi JSON file
  - Authentication for web app

### Phase 7: Deployment & Handoff

- [ ] **7.1** Finalize documentation
  - Operator manual
  - Admin/setup guide
  - Troubleshooting guide

- [ ] **7.2** Create deployment package
  - Pi image or setup scripts
  - ClearCore firmware binary
  - Touch Encoder project export

- [ ] **7.3** Ship and commission
  - Physical installation at GigaFarm
  - On-site testing and calibration
  - Operator training

---

## 11. Revision History

| Date | Version | Changes | Author |
|------|---------|---------|--------|
| 2025-12-10 | 1.0 | Initial PRD created from contract, meetings, and stakeholder input | Claude Code |
| 2025-12-10 | 1.1 | Added detailed architecture diagram, codebase analysis, reusable patterns, and implementation to-do list | Claude Code |
