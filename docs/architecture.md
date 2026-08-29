# System architecture - high-level view

Target picture for the project: one flight core embedded everywhere, and
external processes that only speak the network protocol. This document stays
deliberately high level; it will be refined along with the code.

## Overview

```mermaid
flowchart LR
    subgraph flight["Flight executables - flight-core + platform composition"]
        FW["firmware<br/>STM32F405"]
        DS["drone_sim<br/>desktop (and target)"]
    end

    ESP["ESP32 bridge<br/>UART <-> UDP WiFi"]
    GODOT["Godot simulator<br/>(plant only)"]
    HUB["hub<br/>discovery, decoding, web pages"]
    PAGES["web pages<br/>control, plots"]
    BUS(("transport<br/>frames over UDP"))

    FW <--> ESP
    DS <-->|"sim link: SensorFrame / ActuatorFrame<br/>real-time or lockstep"| GODOT
    ESP <-->|"framed UART stream<br/>(bare packets, for now)"| HUB
    DS <--> BUS
    BUS <--> HUB
    HUB <-->|"HTTP + WebSocket<br/>one port"| PAGES
```

- **A single output bus**: the transport (`software/components/transport/`)
  carries every packet between the flight processes and the ground tools as
  a frame with a source and destination node; telemetry is a broadcast
  frame any number of nodes read simultaneously, commands are unicasts to
  the node that beaconed. The board still reaches the hub as bare packets
  through the ESP32 bridge until it migrates.
- **Godot and the hub never link flight-core**: they only know `protocol/`
  (packed structs, versioned from the first byte). The web pages only know
  the hub's JSON over WebSocket/HTTP, never the wire.
- The sim link's **lockstep** mode (the simulator waits for the motor
  response before advancing its physics) buys determinism, faster-than-real-
  time runs and debugger single-stepping.

## Anatomy of a flight executable

Each executable = the same core + a set of services picked in its `main`
(explicit composition root, injection by reference, no singleton):

```mermaid
flowchart TB
    MAIN["main - composition root"]
    CORE["FlightCore::step(SensorFrame) -> ActuatorFrame<br/>synchronous, single-threaded, data-paced"]

    subgraph platform["platform - pure virtual interfaces"]
        SRC["AbsSensorSource<br/>(blocking, the single wait point)"]
        SINK["AbsMotorSink"]
        TEL["AbsTelemetrySender"]
        CMD["AbsCommandReceiver"]
        CLK["AbsClock<br/>(internal to platform)"]
    end

    MAIN -->|"builds and injects"| CORE
    MAIN -->|"instantiates one variant:<br/>stm32 / sim"| platform
    SRC -->|"waitFrame()"| CORE
    CORE -->|"push()"| SINK
    CORE -.-> TEL
```

Structuring principles:

- **flight-core is pure**: no dynamic allocation, no exceptions/RTTI, no
  clock access - the timestamp travels inside the `SensorFrame`, stamped by
  platform at acquisition time.
- **The RC kill switch** is a field of the frame, handled at the very top of
  `step()`.
- **Quaternions everywhere** (arbitrary attitude), `float` everywhere,
  `-Wdouble-promotion` as an error: comparable execution between desktop and
  Cortex-M4F.
- Platform variants are **composable** (e.g. sim-on-target: stm32 clock +
  UART transport + sim sensor source).
