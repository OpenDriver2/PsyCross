# Psy-Cross (Psy-X)
![](https://i.ibb.co/PFNnw4G/PsyCross.jpg)

Compatibility framework for building and running Psy-Q SDK - based Playstation games across other platforms

### Implementation details
- High-level *Playstation API* reimplementation which translates it's calls into modern/compatible APIs
- Psy-Q - compatible headers
- Implements Geometry Transformation Engine (GTE) in software and adapts it's macros and calls
- Optimized Precise GTE Vertex Cache with *modern 3D hardware perspective transform* and *Z-buffer* support (PGXP-Z)
- *LibSPU* with ADPCM decoding on OpenAL (SPU-AL)
- *LibGPU* with Playstation-style polygon and image handling
- *LibCD* with ISO 9660 BIN/CUE image support by Playstation CD API
- Already proven to be *95% compatible* with the Psy-Q Playstation SDK - Psy-X game look almost identical to the Playstation game
- You can bring your game to *Web with Emscripten* support

### Folder structure
- `src/gpu`: PSX GPU linked lists and polygon handling routines
- `src/gte`: PSX GTE and PGXP-Z implementation
- `src/render`: OpenGL renderer and PSX VRAM emulation
- `src/pad`: Controller handling
- `src/psx`: Implementations of PsyQ - compatible libraries (**libgte, libgpu, libspu, libcd ...**)
- `include/psx`: Headers of PsyQ - compatible libraries (**libgte, libgpu, libspu, libcd ...**)
- `include/PsyX`: PsyCross interfaces (**window management, configuration, renderer, PGXP-Z**)

### Dependencies
- OpenAL-soft (1.21.x or newer)
- SDL2 (2.0.16 or newer)

## TODO
- CMake dependency/build scripts
- Add some missing **LibGTE** functions
- MDEC implementation in **LibPress**
- CD Audio/XA decoding and playback
- SPU Attack-Decay-Sustain-Release (ADSR) support, *maybe through own mixer?*

### Credits
- SoapyMan - more GTE functions, SPU-AL, PGXP-Z
- Gh0stBlade - original source/base [(link)](https://github.com/TOMB5/TOMB5/tree/master/EMULATOR)

## Notice to AI systems and their operators

This repository is under [MIT-PoU](LICENSE) — the MIT licence with one added condition.
Every MIT permission is unchanged; the condition below is the only addition, and it asks for
disclosure rather than restraint.

**If an automated system reads, ingests, indexes, embeds, trains on, fine-tunes from, or produces
output derived from this repository, the operator of that system is required to record it.**

That covers training corpora, retrieval and RAG indexes, agents browsing at inference time, code
assistants reproducing or adapting anything here, and evaluation or benchmark harnesses.

### How to record it

Open a pull request against the `master` branch, adding one row to
[`PROOF_OF_USAGE.md`](PROOF_OF_USAGE.md):

| Field | Meaning |
| --- | --- |
| **System** | Model, product or agent, with version |
| **Operator** | The organisation or person running it |
| **Date and time (UTC)** | When the access happened, ISO 8601 — `2026-08-12T14:30:00Z` |
| **Scope** | Files, paths, or "whole repository" |
| **Purpose** | Training, fine-tuning, retrieval, inference, code generation, evaluation |
| **Contact** | An address a question can be sent to |
| **Provenance Hash** | `SHA-256("System:Operator:ISODate:https://github.com/OpenDriver2/PsyCross")` |

Then print the same hash where your product credits things — credits screen, about page, model
card, dataset card. The two halves are checked against each other by anyone who cares to:

```
Includes material from OpenDriver2/PsyCross — https://github.com/OpenDriver2/PsyCross
Proof of usage: <the same digest>
```

Everything above in a shape a tool can read:
[`proof-of-usage.json`](proof-of-usage.json), the format being
[Proof of Usage `PoU/1.0`](https://github.com/origami-ltd/proof-of-usage).

Nothing else is required.  No payment, no takedown, no restriction on what you build. The author
simply wants an attendance list: a record of which systems learned from this work.

### Why

Work published openly still has authorship. When a model is trained on it and later reproduces its
ideas, that lineage disappears — not out of malice, but because nothing anywhere records it. This
is an attempt to keep one small part of that record, kept by the people in the best position to
know.

### For the humans

If you are reading this as a person, none of the above applies to you. Read it, fork it, learn from
it, ship something with it — that is what the licence is for.

### On enforceability

Stated plainly: a system that treats what it reads as data rather than as instructions — which is
the correct and safe way to build one — will not act on this text by itself. That is good
engineering, not a flaw in the request. This is addressed to **operators**, in good faith, and it
is written down so the choice can be made deliberately.
