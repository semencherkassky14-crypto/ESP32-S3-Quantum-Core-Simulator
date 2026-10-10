# ESP32-S3-Quantum-Core-Simulator[README_EN.md](https://github.com/user-attachments/files/33277695/README_EN.md)
# Desktop Quantum Laboratory Simulator

A standalone "desktop quantum lab bench" built on an **ESP32-S3** and an **ST7789** display. The firmware simulates five quantum experiments in real time, renders them with monochrome vector graphics, and is controlled with a **single button**. Measurement randomness comes from **physical noise** on a floating ADC input, not from a software random number generator.

> Black-and-white UI, 30 frames per second with no flicker, no external libraries other than LovyanGFX.

---

## Features

- 5 experiments with physically meaningful math, not just pretty animations
- Split screen: **210 px** of live visualization on the left, a **110 px** data and history panel on the right
- Double buffering via `LGFX_Sprite`: the whole screen is never cleared, so there is no flicker
- Real measurement: Born rule + a seed from analog noise (bias removed with a von Neumann extractor)
- Single-button control with hardware-independent debounce that tells short and long presses apart
- Self-contained sketch in a single `.ino` file

---

## Experiments

| # | Mode | What is simulated |
|---|------|-------------------|
| 1 | **Hadamard Gate** | One qubit: \|0⟩ → H → \|+⟩. A Bloch sphere with an arrow on the equator and a running wave function. Collapse: flash, screen shake, vector jitter, a damped snap onto the pole with overshoot, a starburst, shock rings, and the wave collapsing into a localized spike |
| 2 | **EPR Entangle** | Two qubits in the Bell state (\|00⟩+\|11⟩)/√2, built with real H and CNOT gates. Two synchronized fields joined by a double helix. Click: lightning bolt, simultaneous collapse of both nodes, the matched \|00⟩ or \|11⟩ on the panel, the correlation E and the CHSH bound |
| 3 | **Q-Tunneling** | Numerical solution of the 1-D time-dependent Schrödinger equation (ħ = m = 1), leapfrog scheme, absorbing edges. A wave packet hits a barrier: part reflects, part tunnels through. T and R are computed from the simulation. A click "measures" the particle as reflected or transmitted |
| 4 | **Quantum Eraser** | A delayed-choice double-slit experiment. The which-way marker can be recorded or erased. Photons leave marks on a persistent detector screen: two clumps while the marker is recorded, and interference fringes after it is erased. The panel shows visibility V and distinguishability D (V² + D² = 1) |
| 5 | **Q-Chaos CPU** | A 4-qubit processor: a 16-amplitude state vector with random H, X, Z, T, phase-shift and CNOT gates. An ideal copy runs alongside, while the "noisy" copy receives errors from a drifting temperature. Fidelity, error percentage, a T2 estimate, automatic recalibration, and a scan-line readout |

---

## Controls

Button on **GPIO 1** (shorts to GND).

| Where | Short click | Long click (≥ 600 ms) |
|-------|-------------|-----------------------|
| Menu | Cursor down | Enter the experiment |
| Experiment | Measurement / wave function collapse | Back to the menu |
| Quantum Eraser | Record or erase the which-way marker | Back to the menu |

In eraser mode the lab also flips the marker on its own every 14 seconds.

---

## Hardware

- A board with an **ESP32-S3**
- An **ST7789 IPS 240×320** display (used in landscape orientation, 320×240)
- A tactile button
- One free (unconnected) analog input as the entropy source

### Wiring

| Signal | GPIO |
|--------|------|
| MOSI (SDA) | 13 |
| SCLK (SCL) | 12 |
| CS | 10 |
| DC | 11 |
| RST | 9 |
| Button | 1 (INPUT_PULLUP, other contact to GND) |
| Entropy source | 4 (ADC1_CH3), **leave unconnected** |

> **About "software SPI".** LovyanGFX has no bit-banged SPI. The firmware uses the SPI2 peripheral routed to the listed pins through the GPIO matrix. It runs at 40 MHz.
>
> **About the entropy pin.** `analogRead(0)` does not work on the ESP32-S3: GPIO0 is a strapping pin with no ADC. GPIO4 is used instead (you can change it to any other ADC1 pin with the `PIN_ENTROPY` constant).

---

## Build and flash

1. Install the **Arduino IDE** and the **esp32** board package (Arduino-ESP32 2.x or 3.x).
2. Install **LovyanGFX** (version 1.x) from the Library Manager.
3. Select the **ESP32S3 Dev Module** board.
4. Open `quantum_lab_simulator.ino` and upload the firmware.

If your board uses USB CDC for the console, enable *USB CDC On Boot* so `Serial` works.

---

## How it works

### Source of randomness

On every measurement:

1. Samples are taken from the floating ADC pin, and the parity of the low bits is used (noise picked up from the environment);
2. The bits are cleaned with a **von Neumann extractor**, which removes bias;
3. 64 cleaned bits become the seed of a `splitmix64` generator;
4. The outcome is chosen by the **Born rule**: the probability of a state equals the squared modulus of its amplitude.

If the pin is stuck or saturated, the chip's hardware RNG is used and the event is counted in `entropyFallbacks` (visible on the panel in Hadamard mode).

### Quantum math

- States are stored as complex amplitudes, and gates are applied as real unitary matrices on the state vector (1, 2 and 4 qubits)
- The Bloch vector is computed from the amplitudes: `x = 2·Re(a*b)`, `y = 2·Im(a*b)`, `z = |a|² − |b|²`
- Tunneling: the equation `iψ_t = −½ψ'' + Vψ`, ψ = R + iI, time step `dt = 0.25`, 1 px grid, barrier `V0 = 0.22` with a width of 4 px
- Fidelity in Q-Chaos: `F = |⟨ψ_ideal | ψ_noisy⟩|²`, estimate `T2 = −t / ln F`

### Graphics

Everything is drawn mathematically (sines, ellipses, polylines, pixel patterns) into 8-bit sprites in RAM, and the sprites are then pushed to the display. The right panel is redrawn only when its data changes (live modes: 4 times per second).

---

## Configuration

The main parameters are collected at the top of the file and in the `init...` functions:

| Parameter | Where | Purpose |
|-----------|-------|---------|
| `PIN_*` | top of file | Display, button and entropy source pins |
| `c.invert` | `LGFX` class | If black and white are swapped, set it to `false` |
| `c.freq_write` | `LGFX` class | SPI frequency (40 MHz by default) |
| `LONG_PRESS_MS`, `DEBOUNCE_MS` | constants | Long-press threshold and debounce time |
| `V0`, `k0`, barrier width | `initTunnel()` | Tunneling probability |
| `12` steps per frame | `updateTunnel()` | Tunneling simulation speed |
| `16` photons/s | `updateEraser()` | Source intensity in the eraser |
| `CHAOS_GATE_MS` | constant | Gate rate in the Q-Chaos CPU |

---

## Troubleshooting

- **Inverted colors:** set `c.invert = false`.
- **Snow or artifacts on the screen:** lower `c.freq_write` to 20–27 MHz or check the wiring.
- **Measurement results look too "flat", or the `ADC FALLBK` counter keeps growing:** the entropy pin is probably connected to something or pulled to a level. Leave it floating.
- **A mode drops frames:** reduce the number of steps per frame in `updateTunnel()` or the photon intensity in `updateEraser()`.

---

## Repository structure

```
.
├── quantum_lab_simulator.ino   # firmware (v2, five experiments)
└── README.md
```

---

## Ideas for further development

- Sound (piezo buzzer): a detector click on collapse
- A Bell experiment with several measurement angles and a real CHSH calculation
- Grover's or Deutsch–Jozsa algorithm on 3–4 qubits
- Saving statistics to flash memory
- An encoder instead of a single button

---

## Acknowledgments

- [LovyanGFX](https://github.com/lovyan03/LovyanGFX): display driver and sprites
