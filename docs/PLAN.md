# Camswap Architecture Plan

## Overview

This document describes the architecture and implementation plan for camswap, an Android camera manipulation module for Surface Duo 1 that performs real-time face swapping using AI.

## File Tree

```
camswap/
├── app/                          # Kotlin/Compose control app
│   ├── src/main/
│   │   ├── AndroidManifest.xml
│   │   ├── java/com/camswap/
│   │   │   ├── MainActivity.kt
│   │   │   ├── CameraService.kt
│   │   │   ├── IdentityManager.kt
│   │   │   ├── SettingsViewModel.kt
│   │   │   └── ui/
│   │   │       ├── components/
│   │   │       └── screens/
│   │   └── res/
│   │       ├── layout/
│   │       ├── values/
│   │       └── mipmap/
│   └── build.gradle.kts
│
├── daemon/                       # C++ AI inference daemon
│   ├── src/
│   │   ├── main.cpp
│   │   ├── ring/
│   │   │   ├── ringbuffer.hpp
│   │   │   └── ringbuffer.cpp
│   │   ├── ai/
│   │   │   ├── face_landmarker.hpp
│   │   │   ├── face_landmarker.cpp
│   │   │   ├── face_swap.hpp
│   │   │   ├── face_swap.cpp
│   │   │   ├── color_transfer.hpp
│   │   │   └── color_transfer.cpp
│   │   ├── gl/
│   │   │   ├── context.hpp
│   │   │   ├── context.cpp
│   │   │   ├── shader.hpp
│   │   │   └── shader.cpp
│   │   └── utils/
│   │       ├── logging.hpp
│   │       └── logging.cpp
│   ├── jni/
│   │   └── jni_bridge.cpp
│   └── Android.mk
│
├── payload/                      # C++ hook payload
│   ├── src/
│   │   ├── main.cpp
│   │   ├── hook/
│   │   │   ├── hook.hpp
│   │   │   ├── hook.cpp
│   │   │   └── inlinehook_arm64.S
│   │   ├── injector/
│   │   │   ├── ptrace_injector.hpp
│   │   │   └── ptrace_injector.cpp
│   │   ├── ring/
│   │   │   ├── ringbuffer.hpp
│   │   │   └── ringbuffer.cpp
│   │   ├── buffer/
│   │   │   ├── buffer_lock.hpp
│   │   │   └── buffer_lock.cpp
│   │   └── utils/
│   │       ├── logging.hpp
│   │       └── logging.cpp
│   └── Android.mk
│
├── module/                       # Magisk module
│   ├── module.prop
│   ├── customize.sh
│   ├── service.sh
│   ├── sepolicy.rule
│   └── system/
│       └── bin/
│           ├── camswap_payload
│           └── camswap_daemon
│
├── devices/                      # Device-specific configs
│   └── surfaceduo.env
│
├── tools/                        # Build and test tools
│   ├── preflight.sh
│   ├── harness.sh
│   ├── build.sh
│   └── report/
│
├── docs/                         # Documentation
│   ├── PLAN.md
│   ├── PREFLIGHT.md
│   ├── RUNBOOK.md
│   └── COMPAT.md
│
└── refs/                         # Reference implementations (read-only)
    ├── ReCam/
    ├── AndKittyInjector/
    ├── android-face-fusion/
    ├── inswapper-512-live/
    ├── aosp-virtual-camera/
    ├── camera_hook/
    ├── Camera2Magic/
    ├── ScopeCam/
    └── xcam/
```

## Architecture Decisions

### 1. Hook Point Selection

**Decision:** Hook `android::camera3::Camera3OutputStream::returnBufferCheckedLocked`

**Rationale:**
- This is called for every frame returned to the framework
- The buffer is already configured by the app, so we don't need to modify StreamConfigurationMap
- We can access the actual pixel buffer before it's presented
- This is the same approach as ReCam

**Implementation:**
- Use ptrace injection to attach to cameraserver
- Resolve symbol via export table, .gnu_debugdata, and DWARF
- Write hook trampoline that intercepts the function

### 2. Communication Mechanism

**Decision:** Shared memory ring buffer between payload and daemon

**Rationale:**
- Zero-copy communication for high frame rates
- Multiple lanes for different resolutions/formats
- Non-blocking operation
- Memory pool with fixed allocations

**Implementation:**
- Create shared memory region with `ashmem_create_region`
- Ring buffer with indexed lanes for (w, h, format)
- Payload deposits frame N, daemon processes and returns N-1

### 3. AI Inference Location

**Decision:** Separate daemon process, not in cameraserver

**Rationale:**
- Isolation: crashes don't affect camera service
- Memory: models loaded once, never duplicated
- Performance: inference can use GPU without blocking binder threads
- Security: smaller attack surface in cameraserver

### 4. Format Handling

**Decision:** Use IMapper/BufferDescriptorInfo for format detection

**Rationale:**
- Vendor-specific formats (e.g., QCOM's private formats)
- Cannot rely on assumptions about stride/planes
- Must read actual buffer layout from gralloc

**Implementation:**
- Query IMapper for BufferDescriptorInfo
- Extract format, stride, and plane layout
- Fallback to passthrough if format not recognized

### 5. Thermal Management

**Decision:** Dynamic scaling based on temperature

**Rationale:**
- Surface Duo has thin form factor
- AI inference generates heat
- Must maintain 20 fps target without overheating

**Implementation:**
- Read thermal zones from `/sys/class/thermal/`
- Scale based on ladder: 38°C→detect_rada, 40°C→roi_128, 42°C→480p, 44°C→passthrough

### 6. Document Mode

**Decision:** Separate code path, passthrough only

**Rationale:**
- KYC requires document integrity
- No AI processing on documents
- Must preserve geometry and sharpness

**Implementation:**
- Detect document via quadrilateral detection
- Hysteresis to prevent flickering
- Manual override via control app

## Risk Matrix

| Component | Risk | Impact | Mitigation |
|-----------|------|--------|------------|
| **gralloc** | High | Camera crash | Use IMapper API, fallback to passthrough |
| **thermal** | Medium | Performance drop | Dynamic scaling, passthrough at 44°C |
| **KYC** | High | App detection | No CameraCharacteristics modification |
| **memory** | High | OOM crash | Fixed buffer pool, budget enforcement |
| **SELinux** | Medium | Hook failure | Extract actual denials, minimal rules |
| **fence** | High | Deadlock | Respect acquire_fence, propagate release_fence |
| **ABI** | Medium | Hook failure | Verify symbol resolution before hooking |
| **rotation** | Medium | Wrong orientation | Handle 0/90/180/270, mirroring |
| **still** | High | JPEG corruption | Process BLOB buffer, preserve EXIF |
| **video** | High | MediaCodec crash | Use same identity throughout recording |

## Memory Budget

| Component | Budget | Rationale |
|-----------|--------|-----------|
| **Payload** | 50 MB | Hook code, ring buffer, frame staging |
| **Daemon** | 200 MB | AI models (INSwapper ~100MB, Landmarker ~50MB) |
| **Models** | 100 MB | INSwapper int8, ArcFace embeddings |
| **Frame Pool** | 100 MB | Fixed pool for 4 lanes × 256px buffers |
| **Total** | 350 MB | Matches rss_budget_mb in surfaceduo.env |

## Component Details

### Payload (`payload/`)

**Responsibilities:**
- Inject into cameraserver via ptrace
- Hook `returnBufferCheckedLocked`
- Deposit frames into ring buffer
- Write processed frames back to buffer
- Handle fence synchronization
- Log statistics

**Key Functions:**
- `inject_payload()` - Inject into cameraserver
- `hook_return_buffer()` - Hook function
- `deposit_frame()` - Deposit frame into ring
- `write_result()` - Write processed frame back

### Daemon (`daemon/`)

**Responsibilities:**
- Process frames from ring buffer
- Run AI inference (landmarker + swap)
- Color transfer and compositing
- Thermal scaling
- Identity management

**Key Functions:**
- `process_frame()` - Main processing loop
- `detect_landmarks()` - MediaPipe Face Landmarker
- `swap_face()` - INSwapper inference
- `color_transfer()` - Color matching
- `get_temperature()` - Thermal scaling

### App (`app/`)

**Responsibilities:**
- Control UI
- Identity management
- Settings
- Live status display
- Foreground service

**Key Components:**
- `MainActivity` - Main UI
- `CameraService` - Foreground service
- `IdentityManager` - Identity versioning
- `SettingsViewModel` - Settings state

## Build Process

1. **Payload:** `ndk-build` with `APP_ABI=arm64-v8a`, `APP_PLATFORM=android-31`
2. **Daemon:** `ndk-build` with same settings
3. **App:** Gradle build with Kotlin, Compose, minSdk 31, targetSdk 32
4. **Module:** Magisk package with `module.prop`, `customize.sh`, `service.sh`

## Gate Criteria

### Gate A (Fase 2)
- 10 minutes per app × 3 apps
- Zero cameraserver crashes
- Zero deadlocks
- Zero buffer leaks

### Gate B (Fase 3)
- 20 minutes with PSNR monitoring
- PSNR stable
- Zero crashes

### Gate C (Fase 5+)
- 30 minutes continuous
- FPS never below 15
- Temperature below 42°C
- Zero crashes

## References

- [`ReCam`](../refs/ReCam/) - Hooking approach, ring buffer, SELinux rules
- [`AndKittyInjector`](../refs/AndKittyInjector/) - Ptrace injection
- [`android-face-fusion`](../refs/android-face-fusion/) - AI pipeline
- [`inswapper-512-live`](../refs/inswapper-512-live/) - Target model
- [`aosp-virtual-camera`](../refs/aosp-virtual-camera/) - HAL reference
- [`camera_hook`](../refs/camera_hook/) - Camera2/CameraX hooks
- [`Camera2Magic`](../refs/Camera2Magic/) - App compatibility matrix
- [`ScopeCam`](../refs/ScopeCam/) - Xposed hooks reference
