# Graphics

What this covers: how a guest game gets a Direct3D device and a swapchain inside the app
container, how its frames reach the screen, and what the console's D3D12 driver will and will not
accept.
Where the code is: the "graphics bed" in `src/loader/guest_loader.cpp` (device, proxies, present,
D3D12 hooks, pipeline-state machinery), `src/loader/gh_d3d9.h` + `gh_d3d9rt.h` + `gh_d3dx9.h`
(Direct3D 9), and `corpus/shims/opengl32_stub.cpp`.

Related: [overview](overview.md) · [window and input](window-input.md) ·
[DXIL backporter](dxil-backporter.md) · [memory](memory.md) · [x86 D3D9](x86-d3d9.md) ·
[knobs](../reference/knobs.md) · [diagnostics](../reference/diagnostics.md) ·
[known limits](../reference/known-limits.md)
How it was found: [first-pixels-first-games.md](../writeups/first-pixels-first-games.md) · [spiderman-first-frame.md](../writeups/spiderman-first-frame.md) · [spiderman-gpu-hang.md](../writeups/spiderman-gpu-hang.md) · [shader-cache-and-bake.md](../writeups/shader-cache-and-bake.md)

---

## 1. The model in one paragraph

The process owns exactly one real `ID3D12Device`, one direct command queue and one swapchain
bound to the app's `CoreWindow`. Every guest, whatever API it asks for, ends up rendering with
that device into a stable off-screen render target that the loader owns. At present time the
loader copies that target into the real back buffer and presents. The guest never sees the real
swapchain: it sees hand-written DXGI proxy objects that answer the questions a desktop game asks
(adapters, outputs, display modes, back buffers) with values that make sense for a game.

| Guest API | What the guest receives | Where it renders |
|---|---|---|
| Direct3D 11 | A D3D11On12 device layered on the shared D3D12 device | `g_gfx.guestRT` (a D3D11 texture) |
| Direct3D 12 | The shared D3D12 device itself | `g_gfx.guestRT12` (a D3D12 resource) |
| Direct3D 9 (x64) | A loader-written `IDirect3D9` runtime over `d3d9on12.dll` | A 9on12 render target handed over as `guestRT12` |
| Direct3D 9 (x86) | The same runtime through the x86 COM layer | See [x86-d3d9.md](x86-d3d9.md) |
| OpenGL | A stub `opengl32.dll` that fails context creation | Nothing; the engine falls back to D3D11 |

All shared state lives in one struct, `g_gfx` (`struct Gfx`, `src/loader/guest_loader.cpp`).

### Why D3D11On12 and not a plain D3D11 device

A D3D11 device created the ordinary way inside the container lands on a legacy path that tops
out at feature level 10_1 with no compute shaders. A D3D12 device with D3D11On12 layered on top
gives feature level 11_1 with compute, and the D3D12 device can present to a `CoreWindow`.
That is the only reason the bridge exists.

---

## 2. Building the bridge

`EnsureBridge()` is called lazily, from the guest's own thread, the first time a guest asks for
anything graphical. Console and command-line guests never trigger it. It marshals
`BuildBridgeOnUi()` onto the UI thread through the dispatcher, because the swapchain must be
created against the `CoreWindow` on the thread that owns it.

`BuildBridgeOnUi()` does, in order:

1. `GhDdiInstall()` — driver-level patches that must be in place before the first device exists
   (the shader-model cap, section 9).
2. Optional switches: experimental shader models (`gh_exp_sm`), the D3D12 debug layer
   (`gh_d3d12_debug`).
3. `D3D12CreateDevice` at feature level 11_0, then a probe for the highest level the device
   supports, stored in `g_gfx.d12Level`.
4. One direct command queue (`g_gfx.queue`).
5. Swapchain via `CreateSwapChainForCoreWindow`, sized from `CoreWindow.Bounds()` (fallback
   1920x1080), format `DXGI_FORMAT_R8G8B8A8_UNORM`.
6. `D3D11On12CreateDevice` over the D3D12 device with exactly one queue and
   `D3D11_CREATE_DEVICE_BGRA_SUPPORT`.
7. The real back buffers wrapped for D3D11 (`g_gfx.wrapped[]`), and the stable guest render
   target `g_gfx.guestRT`.
8. Hooks on the D3D11 device's `CreateRenderTargetView` and `CreateShaderResourceView`.

Things that were learned the hard way and are encoded here:

| Choice | Reason |
|---|---|
| Swapchain is RGBA8, not BGRA8 | Unity creates an RGBA render-target view on the back buffer; with BGRA that call failed |
| `guestRT` is `R8G8B8A8_TYPELESS` | The guest can then create either a UNORM or a UNORM_SRGB view (Unity's linear colour space needs the latter) |
| Exactly one queue passed to D3D11On12 | A two-queue array returns `E_INVALIDARG` on the console |
| Flip model `FLIP_SEQUENTIAL` by default | `gh_sc_flip` 2 selects `FLIP_DISCARD` |
| Two back buffers by default | `gh_sc_buffers` (clamped to 8, `kGfxFrames`) |

Swapchain placement knobs, all probes rather than play settings: `gh_sc_dedicated` puts the
swapchain on its own queue (`g_gfx.scQueue`), `gh_sc_guestq` rebinds it to the guest's queue
(1) or defers creating it until the first present (2, `GhCreateDeferredSwap`).

### Guest render-target format

Some engines ask for a back buffer that is not RGBA8 (Unreal asks for `R10G10B10A2` or
`B8G8R8A8`). `GhMatchGuestRTFormat()` re-creates `guestRT` in the requested format family
(`GhGuestRTFamily()`), and at present time `GhConvertGuestRT()` converts with one full-screen
draw on the guest's immediate context, saving and restoring the state it touches.

---

## 3. How a guest gets a device

### Direct3D 11

`Bed_D3D11CreateDevice` and `Bed_D3D11CreateDeviceAndSwapChain`:

1. `EnsureBridge()`.
2. `GhPickFeatureLevel()` picks the first level in the guest's list that the bridge supports
   (a NULL list means the D3D11 default order starting at 11_0).
3. `ReturnBridgedDevice()` hands back the bridged device and immediate context. Every call
   returns the same device; there is one D3D11 device per process.

One refusal: a second device created with `D3D11_CREATE_DEVICE_VIDEO_SUPPORT` gets
`DXGI_ERROR_UNSUPPORTED`. Unity creates a dedicated video device that way and falls back
cleanly when refused. Sharing the bridged device for video crashed on the console model that
actually performs hardware decode.

### Direct3D 12

`Bed_D3D12CreateDevice`: if the requested feature level is at or below `g_gfx.d12Level`, the
guest is given the bridge's own device (after `GhHookD12Device()` has patched its vtable, section
8). If `QueryInterface` for the requested interface fails, the bed falls through to creating a
real second device and logs that it is not presentable; this is a diagnostic path, not a working
one.

The first D3D12 guest present creates `g_gfx.guestRT12`, the resource `SwapProxy::GetBuffer`
hands out.

### DXGI factories

`Bed_CreateDXGIFactory`, `Bed_CreateDXGIFactory1` and `Bed_CreateDXGIFactory2` all return
`MakeFactoryProxy()`.

---

## 4. The DXGI proxies

| Proxy | Implements | What it changes |
|---|---|---|
| `FactoryProxy` | `IDXGIFactory7` | Adapter enumeration is delegated to the real factory and wrapped. Every swapchain-creation method returns a `SwapProxy` instead of touching the real swapchain. |
| `AdapterProxy` | `IDXGIAdapter4` | Wraps the real adapter so that `EnumOutputs` can return an `OutputProxy`. |
| `OutputProxy` | `IDXGIOutput6` | Supplies a sane display-mode list and desktop bounds. The real output reports a single 0x0 @ 60 mode on the console, and at least one title takes that as the monitor size. |
| `SwapProxy` | `IDXGISwapChain4` | The guest's swapchain. See below. |

`DiagAdapter()` logs one `DXGI` line per adapter the guest looks at (description, vendor,
device, memory).

### The single-vtable rule

Every proxy is a hand-laid COM object with **one vtable** that implements the most-derived
interface, **one reference count**, and `QueryInterface` returning **the same pointer** for
every interface in the inheritance chain.

Do not write these with `winrt::implements` or any helper that gives each interface its own
sub-object. A real DXGI object returns the same pointer for `IDXGIAdapter`, `IDXGIAdapter1`
and so on, and games rely on it: one title obtained `IDXGIAdapter1`, kept the pointer as
`IDXGIAdapter`, and released it through the other interface. With per-interface sub-objects
that release ran against the wrong offset and corrupted a vtable.

Proxies are allocated from a bump arena (`GhProxyArenaAlloc`) and are never freed or reused.
Third-party interposer DLLs release objects long after the game has, and a dangling call into
recycled memory is harder to diagnose than a few kilobytes of leak.

The same rule applies to `GhPsoProxy` (section 10) and to every object in the D3D9 runtime.

### SwapProxy

| Method | Behaviour |
|---|---|
| `GetBuffer(i)` | Returns the stable guest render target for every index. |
| `GetCurrentBackBufferIndex` | 0 (rotates through the count only under the `gh_bb_rotate` probe). |
| `GetDesc*` | `FillSwapProxyDescs()` reports the description the guest asked for, not the real swapchain's. |
| `GetFrameLatencyWaitableObject` | An always-signalled handle. |
| `GetDevice` | Prefers the guest's D3D12 device when there is one. |
| `Present` / `Present1` | `GfxPresent()` (section 5), except for `DXGI_PRESENT_TEST`. |
| `ResizeBuffers`, `SetFullscreenState`, ... | Answered by the proxy; the real swapchain is not touched. |

`DXGI_PRESENT_TEST` is honoured: the call returns without presenting. One title issues a test
present before every real one; when the flag was dropped, each test became a second copy,
present and vblank wait, halving the frame rate. `gh_present_test` 0 restores the old
behaviour for comparison.

---

## 5. The present path

`GfxPresent()` in `src/loader/guest_loader.cpp` is the only place a frame reaches the screen.

```
guest Present
  -> SwapProxy::Present -> GfxPresent
       stamp present counters, frame-time tick
       lock g_swapDance (timed, 3 s) then g_d11Mutex
       [D3D12 guest] guest queue Signal(guestFence); our queue Wait
       acquire wrapped back buffer (D3D11On12)
         D3D12 guest : wrap guestRT12, CopyResource into the back buffer
         D3D11 guest : GhConvertGuestRT (format differs) or CopyResource(guestRT)
       launcher overlay blit, software cursor
       release wrapped resources, Flush
       swap->Present(sync, 0)
       [D3D12 guest] our queue Signal(copyFence); guest queue Wait
       FRAME diagnostics line
```

Details:

- **Locking.** `g_swapDance` is a timed mutex. If it cannot be taken in 3 seconds the frame is
  dropped and logged, not queued. Lock order is always `g_swapDance` then `g_d11Mutex`.
- **Present sync (D3D12 guests).** The guest renders on its own queue, the copy runs on ours.
  A fence pair orders them: the guest queue signals and our queue waits before the copy, then
  our queue signals and the guest queue waits after the present. `gh_present_sync` selects
  both (1, default), neither (0), only guest-to-ours (2) or only ours-to-guest (3).
  D3D9 guests share `g_gfx.queue` and need no fence pair.
- **Copy, not scale.** The copy is `CopyResource`, which requires identical dimensions. A guest
  render target smaller than the swapchain is not stretched. Treat sub-native guest resolution
  as unsupported in the present path; see [known limits](../reference/known-limits.md).
- **Software cursor.** There is no hardware cursor in the container. When a guest shows the
  cursor, `DrawCursorOverlay()` draws a small cross from two textures made by
  `EnsureCursorTextures()`.
- **Overlay.** The launcher's in-game overlay is blitted here (`OverlayBlit`), after the guest
  frame and before the present.
- **`gh_d12_blit` 1** is a probe path that copies with a plain D3D12 command list
  (`g_gfx.blitList`) and skips the D3D11On12 hop, the overlay and the cursor. Default off.
- **`gh_present_noop` 1** returns success without doing anything; used to isolate hangs.

### Keep-alive presenter

`GhKeepAlivePresent()` lets the UI pump present a status frame about once a second while the
guest has not yet produced one, so a long first load is not a black screen. It is off unless
`gh_keepalive` is set (a value above 1 is the interval in milliseconds). It only ever
try-locks `g_swapDance`, so it cannot block a real present.

A second mode, resuming status frames when a running guest goes silent, is gated by
`gh_keepalive_gap` and is off by default: it measured no benefit and it painted "loading"
frames over a game's own long map-load screen.

### Frame-rate and frame diagnostics

| Source | What it is |
|---|---|
| `GfxPresent n=… hr=…` | Logged on the first present and every 300th. |
| `FRAME n=…` | Per-present line with command-list, heap, resource, PSO and timing counters. The resource counters are cumulative, not live. |
| `gh_blackbox.bin` | A small memory-mapped record with a present counter (`gh_blackbox`, sampled every `gh_blackbox_ms`). This is the reliable on-screen frame-rate source. |
| `gh_snap.txt` → `gh_snap.bmp` | Dropping the trigger file into LocalState captures a 480x270 frame grab (`GhSnapMaybe`). |

A game's own frame-rate log line is often a long average and should not be used to judge a
change. Count presents.

---

## 6. Direct3D 9 for x64 guests

The container has no `d3d9.dll`. It does have `d3d9on12.dll`, the mapping layer Microsoft ships
for running D3D9 on D3D12, which exposes a driver (DDI) interface rather than the application
API. The loader provides the application API on top of it.

| File | Role |
|---|---|
| `src/loader/d3d9on12ddi.h` | Declarations for the 9on12 DDI. |
| `src/loader/gh_d3d9.h` (`ghd9`) | Loads and pins `d3d9on12.dll`, opens the adapter through the private DDI table using the shared `g_gfx.d12` device and `g_gfx.queue`, supplies the shader-signing callback (`ghd9::SignDxbc`). |
| `src/loader/gh_d3d9rt.h` | The `IDirect3D9` / `IDirect3DDevice9` runtime: resources, state, draws, present. |
| `src/loader/gh_d3dx9.h` (`ghdx`) | The handful of `d3dx9_42` functions Source's shader API imports: math helpers, the matrix stack, and `D3DXCompileShader` forwarded to the game's own `d3dcompiler_47`. |

Entry points are `Bed_Direct3DCreate9`, `Bed_Direct3DCreate9Ex`, the `Bed_D3DPERF_*` no-ops
and the `Bed_D3DX*` functions, registered in the shim table.

Points worth knowing before changing this code:

- 9on12 translates D3D9 shader bytecode to DXIL and wants it signed. The signing DLL it would
  normally load is absent, so `ghd9::SignDxbc` computes the container hash with
  `ghbake::DxilHash` (the same hash described in
  [dxil-backporter.md](dxil-backporter.md#5-container-signing)).
- The device function table has null entries for the oldest create/flush/present functions.
  The runtime uses the newer forms (`CreateResource2`, `Flush1`, `Present1`).
- Runtime objects follow the single-vtable rule and keep the application reference count
  separate from the internal bind count, as D3D9 requires.
- One recursive lock guards the whole runtime.
- There is no fixed-function pipeline. The one exception is a generated `ps_2_0` shader that
  emulates texture stage 0 when a game draws with no pixel shader (`gh_d9_ffps`). Draws with
  no usable shaders are skipped and counted.
- Present blits the D3D9 back buffer to a 9on12 render target that matches the swapchain,
  flushes, and hands it to `GfxPresent()` as `guestRT12`.
- Caps report vertex and pixel shader model 3.0.
- Diagnostics are `D9` lines; a probe mode (`gh_d9probe.txt`) logs `D9P` lines.

The 32-bit use of this runtime, including argument marshalling and the x86 COM layer, is in
[x86-d3d9.md](x86-d3d9.md).

## 7. OpenGL

`corpus/shims/opengl32_stub.cpp` builds an `opengl32.dll` that exports six `wgl*` functions
which all return NULL or FALSE. Its only purpose is to let `UnityPlayer.dll` resolve its
imports; Unity then fails to create a GL context and uses D3D11. There is no OpenGL
implementation.

---

## 8. D3D12 device hooks

`GhHookD12Device()` patches the shared device's vtable once and keeps the originals in
`g_d12vtOrig[]`. Because there is a single device, the patch covers D3D12 guests, the
D3D11On12 layer and the D3D9on12 layer alike.

| Hooked group | Hook (examples) | Purpose |
|---|---|---|
| Queues, allocators, lists | `Hook_D12CreateCQ` | Queue-type promotion (section 9), queue pool, submission heartbeat |
| Pipeline state | `Hook_D12CreateGPS`, `Hook_D12CreateCPS`, `Hook_D12CreatePSS` | Section 10 |
| Feature queries | `Hook_D12CheckFeature` | Shader-model answer (`gh_fake_sm`), logging of what the guest asks |
| Root signatures | `Hook_D12CreateRS` | Appends the bindless descriptor ranges (section 11) |
| Descriptor heaps and views | CBV/SRV/UAV/RTV/DSV/sampler creates, `CopyDescriptors*` | Descriptor bookkeeping for the buffer-size table and diagnostics |
| Memory | `CreateCommittedResource`, `CreateHeap`, `CreatePlacedResource`, `CreateReservedResource`, `MakeResident`, `Evict`, `OpenExistingHeapFromAddress` | Accounting and the paging machinery; see [memory.md](memory.md) |
| Fences, command signatures | `CreateFence`, `CreateCommandSignature` | Hang diagnostics |

The hook also logs the device's architecture and format support, and starts
`GhStartRemovalWatch()`: a thread that polls `GetDeviceRemovedReason` every 100 ms and writes
the `GPUHEARTBEAT` line.

---

## 9. What the console driver will and will not do

These are behaviours observed on Xbox One and Series consoles in Dev Mode. Each row says what
the loader does about it. None of them is documented by the platform; treat each as a
measurement, and re-measure after a system update.

### Shader models and compilers

| Behaviour | Consequence | Handling |
|---|---|---|
| The device reports shader model 5.1 as its maximum. The number is a default in the D3D12 runtime, not a property of the GPU. | SM6 games refuse to start. | `GhDdiPatchShaderModelCap()` raises the runtime's cap before the first device is created (`gh_ddi_sm`, 0 = leave alone, 60–68 = report that model). Found by pattern search at run time. `gh_fake_sm` can separately adjust what `CheckFeatureSupport` tells the guest. |
| The driver's shader compiler has two back ends. The newer (DXIL) one lives in a DLL whose initialisation fails in the container, and the compiler calls into it unchecked. | Every `CreateGraphicsPipelineState` crashed. | `GhPatchScDllDxilQuery()` makes the compiler answer "the new back end does not handle this stage", so the older back end is used. This default is what makes D3D12 pipelines work at all. |
| The DXIL back end can be brought up by mapping its DLL with the loader's own PE mapper. | Required for any SM6 shader. | `gh_scdxil`: 0 off, 1 bring it up and report, 2 live. `GhScdxilBringUp()` runs before the guest starts. |
| The DXIL back end understands DXIL up to version 1.5 (shader model 6.5). The three SM 6.6 handle-creation opcodes crash it. | A crash, not a refusal. | The [DXIL backporter](dxil-backporter.md) lowers them. |
| Some wave and quad intrinsics remove the device at **any** shader model (lane count, active ballot, active-op reductions, prefix ops, quad read). Others (first lane, lane index, any-true, read-first) compile. | A 6.2 shader can kill the device; shader model is not a safety gate. | Lowered or refused before the driver sees them. |
| A compile failure in the DXIL back end is not an error return. It removes the device. | See next row. | Pipelines are screened before the driver call (`gh_dxil_screen`). |
| A removed device poisons the process: a new `D3D12CreateDevice` also fails. | There is no recovery from one bad shader. | Bad shaders must be refused or replaced before they reach the driver. |
| LLVM `switch` in DXIL is refused by the compiler front end. | Device removal. | Lowered to a compare/branch chain. |
| The driver compiles one pipeline at a time behind a lock. | Multi-threaded pipeline creation gains nothing. | None; expect long first loads. |

### Pipelines, queues, copies

| Behaviour | Consequence | Handling |
|---|---|---|
| `GetCachedBlob()` and `CreatePipelineLibrary()` remove the device, although the shader-cache feature flags are advertised. | No D3D12-level pipeline persistence. | Not used. The driver's own compile cache is used instead (section 10). |
| Creating a COPY-type command queue hangs the device with no work submitted. The next call may report `E_OUTOFMEMORY`, which games show as "out of video memory". | Fatal for engines that use a copy queue for uploads. | `gh_copy_as_direct` (default 1) creates COPY queues, allocators and lists as DIRECT. 2 also promotes COMPUTE. 3 keeps COMPUTE real but serves it from a pool of DIRECT queues (`GhQPoolServe`). |
| Copying into one plane of a multi-plane depth-stencil from a single-plane source dereferences NULL in the driver. | Crash. | Driver bug; avoid the copy. |
| A pipeline with a NULL bytecode pointer for a stage that the D3D11On12 layer hands down faults in the driver. | Open; seen with one Unity title. | None yet. |
| Enabling GPU-based validation makes device creation fail on every adapter. | The debug layer is usable only without it. | `gh_d3d12_debug` 1 only. |

### Shader results that differ from a desktop GPU

| Behaviour | Handling |
|---|---|
| `GetDimensions` on a buffer returns a constant per view class instead of the element count. The stride is right. Out-of-bounds reads are still safely clamped by hardware, so loops bounded by the count just run very long. | The loader publishes real counts in a table and a shader pass reads from it ([dxil-backporter.md](dxil-backporter.md#bufdimspy-and-tbdimspy)). |
| `GetDimensions` on a texture with a constant mip argument of 0, through a view whose most-detailed mip is above 0, returns the full resource size. A run-time mip argument returns the right answer. | A shader pass rewrites the constant form to the run-time form (`gh_texlod0`). |
| `SV_VertexID` / `SV_InstanceID` exclude the draw's base offsets. | This is correct D3D12 behaviour; the console is right. Listed because it was once mistaken for a deviation. Verify "truth" on a desktop D3D12 runtime before building a fix. |

### Memory

| Behaviour | Handling |
|---|---|
| The container has a commit ceiling of roughly 5 GB and no OS page file. D3D12 heaps are billed at creation; `Evict` does not return commit. | [memory.md](memory.md) |
| `OpenExistingHeapFromAddress` is advertised but refused. | Not used. |
| Placed resources own no memory of their own. | Accounting counts heaps, not placed resources. |
| Reserved (tiled) resources bill nothing until mapped and whole-resource remaps work, but remapping or unmapping individual tiles leaves stale reads. | `GhTiledProbe()` (`gh_tiled_probe`) measures this; tiled resources are not used for paging. |

---

## 10. Pipeline-state machinery

All three creation entry points are hooked: `Hook_D12CreateGPS` (graphics),
`Hook_D12CreateCPS` (compute) and `Hook_D12CreatePSS` (the stream form on `ID3D12Device2`).
The stages below run in this order for each create.

| # | Stage | Function / knob | What it does |
|---|---|---|---|
| 1 | Dedup | `gh_pso_dedup` (default 2048 entries, LRU) | Returns the existing pipeline for an identical description. The key includes the root-signature pointer. Logs `PSODEDUP`. |
| 2 | Record | `gh_pso_record`, `GhPsoRecordGfx` / `GhPsoRecordCs` | Appends the full description to a file for offline replay. |
| 3 | Deny list | `GhPsoDenied()`, active when `gh_pso_guard` ≥ 2 | Refuses a pipeline whose shader key is listed. Keys are the digest of the **original** shader. |
| 4 | Substitution | `GhSubstShader()`, `gh_shader_sub` | Swaps each stage's bytecode for its lowered form if one exists. Logs `SHADERSUB`. |
| 5 | Screen | `gh_dxil_screen` | Refuses, with a clean `E_FAIL`, any pipeline that still carries constructs the driver cannot compile. |
| 6 | Fault latch | `gh_pso_faults` | After this many driver faults, stop calling the driver for the rest of the run. |
| 7 | Guarded driver call | `GhTryCreateGPS()`, `GhCallCpsGuarded()` | The real create inside an SEH guard. A fault logs `FAULTED`. |
| 8 | Stand-in | `GhNoopComputeShader()`, `GhNoopVertexShader()`, `gh_pso_standin` | Optionally returns a do-nothing pipeline instead of NULL. |
| 9 | Pager wrap | `GhPagerWrap()`, `GhPsoProxy` | Wraps the result so the pipeline's memory can be released and re-created on demand. |

`PSOSPLIT` lines time the stages of any create slower than `gh_pso_split_ms`.
`gh_pso_serialize` puts one lock over all creation paths; it is a probe and off by default.

### Guarding and stand-ins

`gh_pso_guard`:

| Value | Behaviour |
|---|---|
| 0 | Default. Driver faults propagate. |
| 1 | A driver fault is caught and the create returns `E_FAIL`. |
| 2 | As 1, plus the deny list is applied, and a refused or faulted compute pipeline is replaced by a no-op compute shader. |

Stand-ins exist because some engines ignore the `HRESULT` from pipeline creation and
dereference the null pointer. The no-op compute shader is compiled on the device with the
game's `d3dcompiler_47`; the no-op vertex shader collapses every vertex and is paired with a
NULL pixel shader. `gh_pso_standin` additionally controls whether draws bound to a stand-in
are dropped (1, 2) or allowed to run (3); it is a diagnostic lever.

### Deny list

Sources, highest priority first:

1. `gh_pso_deny.txt` in LocalState: 32-hex-digit keys, any separators.
2. The `psoDeny` array in the title's configuration file (`corpus/titles/*.ghtitle.json`).
3. A built-in list in `GhPsoDenied()`.

A denied pipeline is a missing effect, not a fix. Each entry should say what it costs. Prefer a
corpus-wide lowering ([dxil-backporter.md](dxil-backporter.md#9-adding-a-lowering)) over a new
deny entry.

### Record and replay

With `gh_pso_record` 1 the hooks append every create to `gh_psorec.bin` in LocalState: ordinal,
kind, shader keys, the full description, the input layout and the serialized root signature.
The record is taken before any rewriting. Shaders that have no lowered form on disk are saved
beside it.

Replay runs from the launcher without the game (`gh_dxil_ctl` 7). It reads the record and
calls the original creation functions in the recorded order. This turns "the game crashed four
minutes into loading" into a loop of a few seconds, and it is how most driver behaviours in
section 9 were isolated.

`GhDumpFailedShader()` writes the bytecode of any refused shader for offline inspection.
`GhProbePSO()` compiles a trivial vertex/pixel pair on the device as a sanity check.

### Pipeline pager (summary)

A compiled pipeline holds driver memory that counts against the container's commit ceiling.
`GhPsoProxy` is a single-vtable `ID3D12PipelineState` the guest holds in place of the real
object. `GhPagerTrim()` can release the real pipeline; `GhPagerMaterialise()` re-creates it
from the saved description when a command list binds it. Released objects go through a
quarantine (`GhPagerQuarPush` / `GhPagerQuarDrain`) so in-flight command lists stay valid.
Policy, measurements and the limits of this approach are in [memory.md](memory.md).

### The driver's compile cache

The driver memoizes shader compiles through a system DLL with a small on-disk key/value store.
Left alone it is close to useless for a large game: the store's default capacity is tiny and it
lives on a volume that is about 3 GB in total and shared by every installed package. When that
volume fills, the loader's own diagnostics stop being written.

The loader interposes on that DLL's exports (thunks dispatch through `GhScDispatch`;
`GhScacheCensus()` reports hit and miss counts; log lines begin `SCACHE`). `gh_scache`:

| Value | Behaviour |
|---|---|
| 1 | Intercept and count only. |
| 2 | Also enlarge the store when the driver creates it (`gh_scache_x` multiplier). |
| 3 | Default. Also relocate the store to the development-files volume, which has room. |

The setting is the same for every title because the store is process-global. Calls into the
cache are guarded: the first fault disables the cache for the rest of the run (a miss is always
correct, only slower), leaves a marker, and the next launch moves the damaged store aside.

Anything that changes a root signature changes every cache key that depends on it. After
turning on a feature that appends root parameters, expect one slow load while the cache
re-warms.

---

## 11. Root signatures and the bindless ranges

Shader model 6.6 lets a shader index the descriptor heap directly. The backporter rewrites
that into indexing ordinary unbounded descriptor arrays in reserved register spaces
([dxil-backporter.md](dxil-backporter.md#heaptierpy)). The shader's root signature must then
declare those arrays, and the game's root signature does not.

`Hook_D12CreateRS` with `GhAppendBindlessRanges()` (`gh_rs_bindless`) deserializes the guest's
root signature, **appends** two descriptor tables and re-serializes:

- one table holding every CBV/SRV/UAV range from `kGhBindlessSpaces[]`
  (`src/loader/gh_dxil_spaces.h`, generated), all aliased at descriptor offset 0;
- one table holding the sampler ranges.

Parameters are appended, never inserted, so the game's own root parameter indices are
unchanged. Sampler ranges must not carry data-volatility flags or the whole signature is
rejected.

`GhBindBindlessTables()` binds both tables to the start of the currently set heaps whenever a
root signature or descriptor heap is set on a command list, and the record is cleared in
`Hook_CLReset`. Because the tables start at the heap base, an array index in the lowered
shader equals the heap index in the original.

---

## 12. DirectStorage

`Bed_DStorageGetFactory` hands out the real DirectStorage factory. DirectStorage's GPU
decompression path creates its own compute pipeline from a DXIL 6.5 shader that the console
driver cannot compile, which removes the device. `gh_ds_nogpu`:

| Value | Behaviour |
|---|---|
| 0 | Default; leave DirectStorage alone. |
| 1 | Before the first factory, configure DirectStorage to disable GPU decompression and the metacommand path. |
| 2 | Disable only the metacommand path. |
| 3 | Leave the configuration alone and refuse DirectStorage's own compute pipeline when it is created, so it falls back to CPU decompression. |

---

## 13. Diagnosing a GPU hang (summary)

A hang shows up as a fence that never completes. The log lines that matter:

| Line | Meaning |
|---|---|
| `GPUSUBMIT` | A command-list batch was executed on a queue. |
| `GPUSIGNAL` / `GPUWAIT` | A fence was signalled / a thread started waiting on one. |
| `GPUHEARTBEAT` | Once a second: executed-list count, time since the last submission, completed versus signalled value per fence. Repeats every 5 s when idle. |
| `CLOP` | Per-command-list operation trace when armed (`gh_cl_trace_*`). |
| `D3D12MSG` | Debug-layer messages, when `gh_d3d12_debug` is set. |
| `DBGSTR` | Strings the driver itself writes to the debugger channel. |

One trap: the queue a thread is blocked on is not necessarily the queue that caused the hang.
Compare completed and signalled values across all fences before blaming one.

Bisection knobs (`gh_skip_submit`, `gh_present_noop`, `gh_present_sync`, `gh_d12_blit`,
`gh_copy_as_direct`) and the full procedure are in
[diagnostics.md](../reference/diagnostics.md).

---

## 14. Known gaps

- The present path does not scale; the guest must render at the swapchain size.
- No HDR output; the swapchain is 8-bit RGBA.
- No hardware cursor.
- One D3D11 device per process; a second "video" device is refused.
- No D3D12 pipeline library or cached-blob persistence.
- No fixed-function D3D9 beyond texture stage 0.
- No OpenGL or Vulkan.
- A source comment above `FillSwapProxyDescs()` still describes the real swapchain as a fixed
  1920x1080 BGRA one. The code sizes it from the window and uses RGBA8; the code is right.
