# Metal rendering compatibility

MetalPatch provides runtime Metal rendering compatibility for Picasso on Sequoia, using the implementation and data from [steve12311/NootedRed pre-release.15](https://github.com/steve12311/NootedRed/tree/690e6c528e59adfa0f0daf12628bf2f1bb4cf8bb) with a compatibility profile for the 24H23 shared cache.

## Activation and scope

The patch activates automatically with NootedRed on PCI `15D8` (Picasso) and Darwin 24 (Sequoia). No enabling boot argument is required. Other devices and macOS versions are excluded by the runtime gate. `-NRedNoFVRC` disables the verified-resolution cache for diagnostic purposes; it does not disable MetalPatch.

The module patches private process mappings through the shared-region hook. It can affect WindowServer and other processes using the matching graphics drivers, including Chrome. It does not modify Apple files, the root volume, SIP, AMFI, NVRAM or EFI. Code-signing policy may reject modified executable mappings.

## Implementation

`NootedRed/MetalPatch.hpp` exposes `MetalPatch::init(KernelPatcher&)`; `NRed.cpp` calls it after device discovery. All private helpers and data live under `NootedRed/MetalPatch/` and the `MetalPatch` namespace.

| Component | Purpose |
|---|---|
| `MetalPatch.cpp` | Kernel hook, per-process patch orchestration and diagnostics |
| `SharedCache.hpp`, `CacheResolver.hpp` | Bounded Mach-O/cache reads, full-function matching and relative-reference verification |
| `Resolver.hpp` | Build the Metal/SubmitGuard patch plans and select the exact-cache compatibility profile |
| `Memory.hpp`, `Transaction.hpp` | Private copy-on-write pages, trampoline ownership, writes, readback, protection restoration and rollback |
| `PatchPlan.hpp`, `Runtime.hpp` | Verified plans, Metal feature dependencies and installation state |
| `ResolutionCache.hpp` | Cache verified resolution results with mapping identity checks and full-validation fallback |
| `Data/` | Imported payloads/reference metadata and the local Sequoia compatibility profile |

The patch provides shared resource-descriptor initialization (SRD), conditional LegacyBlend/DualQuad correction, single-shader-engine compute scratch sizing, and SubmitGuard. Native synchronization is used. SubmitGuard must validate/install before the render transaction proceeds. Unknown or mixed code states are rejected. The local compatibility profile does not relax these checks.

The module does not provide video acceleration. Resolver metadata for additional upstream versions is used by shared resolver code and regression fixtures; runtime activation is limited to Picasso/Sequoia.

## 24H23 reference adjustment

For shared-cache UUID `fb6a36be0ccd3835ae8ce600a980e3a3`, `Data/Sequoia24H23.hpp` resolves Metal.framework's `_OBJC_IVAR_$_MTLIOAccelResource._res` at RVA `0x36ACB080`. The inspected target is `0x7ff843e0e080`, containing `0x20`.

This reference profile applies only to the exact UUID and Picasso. Full-function and reference validation remain mandatory. Other caches use the upstream profile and may be rejected. Compatibility must be checked after macOS updates.

## Diagnostics

```sh
ioreg -lw0 | grep -E 'NRed(ChromiumMetal|Display|ImmediateSync|SubmitGuard|LegacyBlend|ComputeScratch|SrdShared|MetalChrome)'
```

`NRedChromiumMetalRevision=1` and `NRedDisplayFixEnabled=1` confirm activation and hook installation. They do not establish patch application. Application evidence includes `NRedImmediateSyncDynamicApplied=1`, `NRedDisplayVPFPlanVerified=1`, positive SRD/LegacyBlend/Scratch/SubmitGuard counters, and WindowServer-specific properties. The three Metal group states use value 6 for Installed. Group bits are 2/3/4, so `NRedDisplayPlannerRequested=28` represents the three Metal groups.

`NRedMetalChromeVerified` and `NRedMetalChromeLastPID` record successful transaction verification for process names beginning with `Google Chrome`. Counters saturate at 16; they do not count distinct GPU contexts or prove a particular rendering backend. Correlate the PID with `chrome://gpu` and the browser command line.

`NRedImmediateSyncDisabled=1` indicates that the immediate-sync modification is disabled; MetalPatch uses native synchronization. This property does not indicate that the Metal module is disabled. Rejection/stage flags are sticky across process invocations. Inspect per-process success evidence and logs rather than treating every recorded rejection as a current Chrome failure. Write, restore or rollback failures require investigation.

## Build and validation

From the repository root:

```sh
~/.local/bin/xmake f -m releasedbg
~/.local/bin/xmake -y
python3 tests/MetalPatch/test_installed_cache.py
python3 tests/MetalPatch/test_bridge.py --sequoia --sanitize
python3 tests/MetalPatch/test_transaction.py --sanitize
python3 tests/MetalPatch/test_blend.py
```

These are CPU-only/offline tests; they do not install kernel hooks or submit GPU work. The installed-cache probe reads the current local cache and checks the production resolver at two simulated slides, including incorrect-UUID and binding rejection. The bridge tests execute production hook bodies against mocked Mach APIs, including relocated functions and rollback. The transaction harness exercises mixed states, readback and protection failures; the blending harness executes original/patched instruction blocks against synthetic memory.

## Provenance

The implementation and data derive from commit `690e6c528e59adfa0f0daf12628bf2f1bb4cf8bb` of `steve12311/NootedRed`, under the source project's license. This fork includes a Picasso/Sequoia activation gate, an exact-cache reference profile and Chrome verification diagnostics.
