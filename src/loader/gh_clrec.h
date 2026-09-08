#pragma once
// #158. A faithful command-list recorder, so the guest's OWN commands -- draws
// included -- can be executed from a loader-owned command list with their ordering
// intact.
//
// Why a recorder at all, having argued against one twice: the allocator substitution
// (gh_cl_reloc) moved the guest's whole stream into our memory and changed nothing, so
// allocator ownership is excluded and the remaining variable is the LIST OBJECT. The
// only way to put the game's real draws into a different list object is to record and
// replay them, and the only way to do that safely is to know the exact set of methods
// the game calls. gh_cl_census answered that: 38 vtable slots, of which 31 are stream
// commands, and DrawIndexedInstanced is never called at all.
//
// The census is also the miss detector. Every slot below is one the census SAW; re-run
// the census with recording on and any slot that appears here uncaptured is a bug, not
// a result. Overflow is counted and reported, and a run that overflows is void rather
// than negative -- the lesson from the two builds that silently disabled themselves.
//
// Ordering is preserved exactly: ops are appended in call order and replayed in call
// order into one loader-owned list per submission. Nothing is hoisted, reordered or
// coalesced, which is the flaw that made the barrier relocation's full-render arm
// ambiguous.

// The breadcrumb helpers live further down guest_loader.cpp. GhBcBarrierBefore's default
// argument is deliberately NOT repeated here: a later declaration may add a default, but
// repeating it is an error.
static LONG GhBreadcrumbBefore(ID3D12GraphicsCommandList* cl);
static void GhBreadcrumbAfter(ID3D12GraphicsCommandList* cl, LONG g);
static LONG GhBcBarrierBefore(ID3D12GraphicsCommandList* cl, LONG knownType);
static void GhBcBarrierAfter(ID3D12GraphicsCommandList* cl, LONG k);
static void GhBarrBcNote(LONG k, ID3D12GraphicsCommandList* self, UINT n,
                         const D3D12_RESOURCE_BARRIER* arr);

struct GhRecOp {
    UINT16 slot;
    UINT16 nA;        // primary array count
    UINT16 nB;        // secondary array count
    UINT16 flags;
    UINT32 off;       // arena offset of the primary blob, or GH_REC_NOOFF
    UINT32 off2;      // arena offset of the secondary blob
    UINT64 a[6];      // scalar / pointer arguments
};
static const UINT32 GH_REC_NOOFF = 0xFFFFFFFFu;
static const LONG   GH_REC_MAXOPS  = 1 << 18;   // 262144 ops per list
static const LONG   GH_REC_MAXAREN = 32 << 20;  // 32 MB of argument bytes per list

static volatile LONG g_recOverflow = 0, g_recOps = 0, g_recLists = 0, g_recReplays = 0;

// Append one op. Returns nullptr on overflow, which is loud and voids the run.
static GhRecOp* GhRecPush(GhCLNote* n, UINT16 slot)
{
    if (!n) return nullptr;
    if (n->ropN >= n->ropCap){
        const LONG want = n->ropCap ? n->ropCap * 2 : 4096;
        if (want > GH_REC_MAXOPS){
            if (InterlockedIncrement(&g_recOverflow) == 1)
                Diag("CLREC op overflow -- this run is VOID, not a null result");
            return nullptr;
        }
        GhRecOp* p = (GhRecOp*)realloc(n->rops, (size_t)want * sizeof(GhRecOp));
        if (!p){
            if (InterlockedIncrement(&g_recOverflow) == 1)
                Diag("CLREC op realloc failed -- this run is VOID");
            return nullptr;
        }
        n->rops = p; n->ropCap = want;
    }
    GhRecOp* o = &n->rops[n->ropN++];
    memset(o, 0, sizeof *o);
    o->slot = slot; o->off = GH_REC_NOOFF; o->off2 = GH_REC_NOOFF;
    InterlockedIncrement(&g_recOps);
    return o;
}

// Copy an argument array into the list's arena. Offsets, not pointers, so growing the
// arena can never invalidate an op that already referenced it.
static UINT32 GhRecBlob(GhCLNote* n, const void* p, size_t bytes)
{
    if (!p || !bytes) return GH_REC_NOOFF;
    const LONG need = (LONG)((bytes + 15) & ~(size_t)15);
    if (n->rarN + need > n->rarCap){
        LONG want = n->rarCap ? n->rarCap * 2 : 65536;
        while (want < n->rarN + need) want *= 2;
        if (want > GH_REC_MAXAREN){
            if (InterlockedIncrement(&g_recOverflow) == 1)
                Diag("CLREC arena overflow -- this run is VOID, not a null result");
            return GH_REC_NOOFF;
        }
        BYTE* q = (BYTE*)realloc(n->rar, (size_t)want);
        if (!q){
            if (InterlockedIncrement(&g_recOverflow) == 1)
                Diag("CLREC arena realloc failed -- this run is VOID");
            return GH_REC_NOOFF;
        }
        n->rar = q; n->rarCap = want;
    }
    const UINT32 off = (UINT32)n->rarN;
    memcpy(n->rar + n->rarN, p, bytes);
    n->rarN += need;
    return off;
}
static void* GhRecAt(GhCLNote* n, UINT32 off)
{ return off == GH_REC_NOOFF ? nullptr : (void*)(n->rar + off); }

// gh_cl_rec: 1 = record every stream command of every guest DIRECT list and replay it,
// in order, from one loader-owned list per submission. The guest's own lists are then
// submitted empty alongside ours, so the ONLY thing that changed is which list object
// the commands are in -- the fourth leg, draws included.
static unsigned GhClRec()
{ static const unsigned v = GhCfgNum(L"gh_cl_rec.txt", "GH_CL_REC", 0); return v; }

// gh_cl_dup: K = execute every guest submission K+1 times. The recorder captures the
// stream as usual but does NOT take it away from the guest -- our loader-owned list is
// K extra copies running alongside the guest's own, which still carries its own
// commands and its own breadcrumbs.
//
// This is the gh_pso_clone method pointed at the surviving hypothesis. Four rounds of
// localisation came back negative (not the list, not the allocator, not the draw, not
// the barrier), and the only framing left standing is "dose-dependent in total issued
// work". If that is right the death must move earlier in proportion to K; if it does
// not move at all, it was never a dose and the trigger is positional in the game's
// own program.
//
// Bit 8 (256) additionally drops barriers from the duplicate. Only for the case where
// re-running a submission's transitions leaves a lying StateBefore -- a different bug
// from the hang, and one that would void the run rather than answer it.
static unsigned GhClDupRaw()
{ static const unsigned v = GhCfgNum(L"gh_cl_dup.txt", "GH_CL_DUP", 0); return v; }
static unsigned GhClDup()    { return GhClDupRaw() & 0xFF; }
static bool     GhClDupNoBarr(){ return (GhClDupRaw() & 256) != 0; }

// gh_sub_census: record the guest's stream and MEASURE it, replaying and suppressing
// nothing. GREENHOUSE.md:10806 names "a DMA buffer's allocation or patch-location list"
// as the standard structure of the capacity it is hunting -- a fixed kernel-side table,
// filled per distinct thing a command buffer references, reclaimed on a boundary the game
// never reaches. Nobody ever counted one: "allocation list", "patch location" and
// "unique resources per submission" have zero hits in the whole record. This counts them.
//
// The point of doing it through the recorder is that it changes NOTHING: the capture path
// is already proven behaviour-neutral, GhRecKeep below lets every command through to the
// guest's own list, and GhRecBuild is not called. A measurement that alters the stream
// would just manufacture another regime artifact, which is what 0.0.283.0 retired.
static unsigned GhSubCensus()
{ static const unsigned v = (unsigned)GhCfgNum(L"gh_sub_census.txt", "GH_SUB_CENSUS", 0); return v; }

// gh_cl_split = N: gh_cl_rec, but the submission's stream is cut into N consecutive
// pieces, each in its own loader-owned command list, submitted in order on the guest's
// own queue. Content, order and queue are identical; only per-command-buffer occupancy
// changes. Every "density" experiment on the record instead REMOVED work, and all of them
// ran in the barriers-only regime that 0.0.283.0 declared non-predictive.
//
// The prediction is quantitative, which is the point: if the capacity being exhausted is
// per command buffer -- a DMA buffer's allocation or patch-location list is the standard
// structure of that shape -- survival scales with N. If it does not move, the capacity is
// per frame or per queue and the theory dies cleanly instead of being re-litigated.
//
// N = 1 is exactly gh_cl_rec=1 and is the required same-build control, since that is a
// known-dying configuration (ecl 36 against the control's 38).
static const int kSplitMax = 32;
static unsigned GhClSplit()
{ static const unsigned v = (unsigned)GhCfgNum(L"gh_cl_split.txt", "GH_CL_SPLIT", 0); return v; }

// One gate for every capture site. Recording applies to DIRECT lists we track; anything
// else keeps its own commands, which is always safe.
static GhCLNote* GhRecOn(ID3D12GraphicsCommandList* self)
{
    if (!GhClRec() && !GhClDup() && !GhSubCensus() && !GhClSplit()) return nullptr;
    GhCLNote* n = GhCLFind(self);
    if (!n || n->type != D3D12_COMMAND_LIST_TYPE_DIRECT) return nullptr;
    if (n->recIsOurs) return nullptr;          // never record our own replay list
    return n;
}

// true = the guest's list must NOT get this command (gh_cl_rec, relocation).
// false = capture it and let it through as well (gh_cl_dup, duplication, gh_sub_census).
// Only relocation takes a command away; stated positively so a third capture-only mode
// cannot accidentally inherit the suppression by being "not dup".
static bool GhRecKeep() { return !GhClDup() && (GhClRec() != 0 || GhClSplit() != 0); }

#define GH_REC_BEGIN(SLOT) \
    GhCLNote* _n = GhRecOn(self); if (!_n) return false; \
    GhRecOp* _o = GhRecPush(_n, (UINT16)(SLOT)); if (!_o) return false;

static bool GhRecCap12(ID3D12GraphicsCommandList* self, UINT a, UINT b, UINT c, UINT d)
{ GH_REC_BEGIN(12) _o->a[0]=a; _o->a[1]=b; _o->a[2]=c; _o->a[3]=d; return GhRecKeep(); }

static bool GhRecCap14(ID3D12GraphicsCommandList* self, UINT x, UINT y, UINT z)
{ GH_REC_BEGIN(14) _o->a[0]=x; _o->a[1]=y; _o->a[2]=z; return GhRecKeep(); }

static bool GhRecCap15(ID3D12GraphicsCommandList* self, ID3D12Resource* d, UINT64 do_,
                       ID3D12Resource* s, UINT64 so, UINT64 nb)
{ GH_REC_BEGIN(15) _o->a[0]=(UINT64)(uintptr_t)d; _o->a[1]=do_;
  _o->a[2]=(UINT64)(uintptr_t)s; _o->a[3]=so; _o->a[4]=nb; return GhRecKeep(); }

// Hook_CLCopyTextureRegion and Hook_CLExecIndirect declare their arguments as void*:
// they only ever forwarded them. Take the same types and re-type them here.
static bool GhRecCap16(ID3D12GraphicsCommandList* self, const void* dv,
                       UINT x, UINT y, UINT z, const void* sv, const void* boxv)
{ const D3D12_TEXTURE_COPY_LOCATION* d = (const D3D12_TEXTURE_COPY_LOCATION*)dv;
  const D3D12_TEXTURE_COPY_LOCATION* s = (const D3D12_TEXTURE_COPY_LOCATION*)sv;
  const D3D12_BOX* box = (const D3D12_BOX*)boxv;
  GH_REC_BEGIN(16)
  _o->a[0]=x; _o->a[1]=y; _o->a[2]=z;
  D3D12_TEXTURE_COPY_LOCATION two[2]{};
  if (d) two[0] = *d; if (s) two[1] = *s;
  _o->flags = (UINT16)((d?1:0) | (s?2:0));
  _o->off  = GhRecBlob(_n, two, sizeof two);
  _o->off2 = box ? GhRecBlob(_n, box, sizeof *box) : GH_REC_NOOFF;
  return GhRecKeep(); }

static bool GhRecCap17(ID3D12GraphicsCommandList* self, ID3D12Resource* d, ID3D12Resource* s)
{ GH_REC_BEGIN(17) _o->a[0]=(UINT64)(uintptr_t)d; _o->a[1]=(UINT64)(uintptr_t)s; return GhRecKeep(); }

static bool GhRecCap20(ID3D12GraphicsCommandList* self, D3D12_PRIMITIVE_TOPOLOGY t)
{ GH_REC_BEGIN(20) _o->a[0]=(UINT64)t; return GhRecKeep(); }

static bool GhRecCap21(ID3D12GraphicsCommandList* self, UINT n, const D3D12_VIEWPORT* v)
{ GH_REC_BEGIN(21) _o->nA=(UINT16)n; _o->off=GhRecBlob(_n, v, (size_t)n*sizeof(*v)); return GhRecKeep(); }

static bool GhRecCap22(ID3D12GraphicsCommandList* self, UINT n, const D3D12_RECT* r)
{ GH_REC_BEGIN(22) _o->nA=(UINT16)n; _o->off=GhRecBlob(_n, r, (size_t)n*sizeof(*r)); return GhRecKeep(); }

static bool GhRecCap23(ID3D12GraphicsCommandList* self, const FLOAT bf[4])
{ GH_REC_BEGIN(23) _o->off=GhRecBlob(_n, bf, 4*sizeof(FLOAT)); return GhRecKeep(); }

static bool GhRecCap24(ID3D12GraphicsCommandList* self, UINT r)
{ GH_REC_BEGIN(24) _o->a[0]=r; return GhRecKeep(); }

static bool GhRecCap25(ID3D12GraphicsCommandList* self, ID3D12PipelineState* p)
{ GH_REC_BEGIN(25) _o->a[0]=(UINT64)(uintptr_t)p; return GhRecKeep(); }

static bool GhRecCap26(ID3D12GraphicsCommandList* self, UINT n, const D3D12_RESOURCE_BARRIER* b)
{ GH_REC_BEGIN(26) _o->nA=(UINT16)n; _o->off=GhRecBlob(_n, b, (size_t)n*sizeof(*b)); return GhRecKeep(); }

static bool GhRecCap28(ID3D12GraphicsCommandList* self, UINT n, ID3D12DescriptorHeap* const* h)
{ GH_REC_BEGIN(28) _o->nA=(UINT16)n; _o->off=GhRecBlob(_n, h, (size_t)n*sizeof(*h)); return GhRecKeep(); }

static bool GhRecCap29(ID3D12GraphicsCommandList* self, ID3D12RootSignature* r)
{ GH_REC_BEGIN(29) _o->a[0]=(UINT64)(uintptr_t)r; return GhRecKeep(); }

static bool GhRecCap30(ID3D12GraphicsCommandList* self, ID3D12RootSignature* r)
{ GH_REC_BEGIN(30) _o->a[0]=(UINT64)(uintptr_t)r; return GhRecKeep(); }

static bool GhRecCap31(ID3D12GraphicsCommandList* self, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h)
{ GH_REC_BEGIN(31) _o->a[0]=i; _o->a[1]=h.ptr; return GhRecKeep(); }

static bool GhRecCap32(ID3D12GraphicsCommandList* self, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h)
{ GH_REC_BEGIN(32) _o->a[0]=i; _o->a[1]=h.ptr; return GhRecKeep(); }

static bool GhRecCap38(ID3D12GraphicsCommandList* self, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va)
{ GH_REC_BEGIN(38) _o->a[0]=i; _o->a[1]=va; return GhRecKeep(); }

static bool GhRecCap43(ID3D12GraphicsCommandList* self, const D3D12_INDEX_BUFFER_VIEW* v)
{ GH_REC_BEGIN(43) _o->off = v ? GhRecBlob(_n, v, sizeof *v) : GH_REC_NOOFF; return GhRecKeep(); }

static bool GhRecCap44(ID3D12GraphicsCommandList* self, UINT start, UINT n,
                       const D3D12_VERTEX_BUFFER_VIEW* v)
{ GH_REC_BEGIN(44) _o->a[0]=start; _o->nA=(UINT16)n;
  _o->off = GhRecBlob(_n, v, (size_t)n*sizeof(*v)); return GhRecKeep(); }

static bool GhRecCap46(ID3D12GraphicsCommandList* self, UINT n,
                       const D3D12_CPU_DESCRIPTOR_HANDLE* rtv, BOOL single,
                       const D3D12_CPU_DESCRIPTOR_HANDLE* dsv)
{ GH_REC_BEGIN(46) _o->nA=(UINT16)n; _o->a[0]=(UINT64)single;
  _o->off  = GhRecBlob(_n, rtv, (size_t)(single ? 1u : n)*sizeof(*rtv));
  _o->off2 = dsv ? GhRecBlob(_n, dsv, sizeof *dsv) : GH_REC_NOOFF;
  return GhRecKeep(); }

static bool GhRecCap47(ID3D12GraphicsCommandList* self, D3D12_CPU_DESCRIPTOR_HANDLE h,
                       D3D12_CLEAR_FLAGS f, FLOAT depth, UINT8 stencil, UINT nr,
                       const D3D12_RECT* r)
{ GH_REC_BEGIN(47) _o->a[0]=h.ptr; _o->a[1]=(UINT64)f; _o->a[3]=stencil; _o->nA=(UINT16)nr;
  memcpy(&_o->a[2], &depth, sizeof depth);
  _o->off = GhRecBlob(_n, r, (size_t)nr*sizeof(*r)); return GhRecKeep(); }

static bool GhRecCap48(ID3D12GraphicsCommandList* self, D3D12_CPU_DESCRIPTOR_HANDLE h,
                       const FLOAT c[4], UINT nr, const D3D12_RECT* r)
{ GH_REC_BEGIN(48) _o->a[0]=h.ptr; _o->nA=(UINT16)nr;
  _o->off  = GhRecBlob(_n, c, 4*sizeof(FLOAT));
  _o->off2 = GhRecBlob(_n, r, (size_t)nr*sizeof(*r)); return GhRecKeep(); }

static bool GhRecCap49(ID3D12GraphicsCommandList* self, D3D12_GPU_DESCRIPTOR_HANDLE g,
                       D3D12_CPU_DESCRIPTOR_HANDLE c, ID3D12Resource* res,
                       const UINT v[4], UINT nr, const D3D12_RECT* r)
{ GH_REC_BEGIN(49) _o->a[0]=g.ptr; _o->a[1]=c.ptr; _o->a[2]=(UINT64)(uintptr_t)res;
  _o->nA=(UINT16)nr;
  _o->off  = GhRecBlob(_n, v, 4*sizeof(UINT));
  _o->off2 = GhRecBlob(_n, r, (size_t)nr*sizeof(*r)); return GhRecKeep(); }

static bool GhRecCap51(ID3D12GraphicsCommandList* self, ID3D12Resource* res,
                       const D3D12_DISCARD_REGION* reg)
{ GH_REC_BEGIN(51) _o->a[0]=(UINT64)(uintptr_t)res;
  if (reg){
      // Two levels: the region struct AND the rect array it points at.
      _o->off  = GhRecBlob(_n, reg, sizeof *reg);
      _o->off2 = GhRecBlob(_n, reg->pRects, (size_t)reg->NumRects*sizeof(D3D12_RECT));
      _o->nA   = (UINT16)reg->NumRects;
      _o->flags = 1;
  }
  return GhRecKeep(); }

static bool GhRecCap53(ID3D12GraphicsCommandList* self, ID3D12QueryHeap* h,
                       D3D12_QUERY_TYPE t, UINT i)
{ GH_REC_BEGIN(53) _o->a[0]=(UINT64)(uintptr_t)h; _o->a[1]=(UINT64)t; _o->a[2]=i; return GhRecKeep(); }

static bool GhRecCap54(ID3D12GraphicsCommandList* self, ID3D12QueryHeap* h, D3D12_QUERY_TYPE t,
                       UINT start, UINT n, ID3D12Resource* dst, UINT64 off)
{ GH_REC_BEGIN(54) _o->a[0]=(UINT64)(uintptr_t)h; _o->a[1]=(UINT64)t; _o->a[2]=start;
  _o->a[3]=n; _o->a[4]=(UINT64)(uintptr_t)dst; _o->a[5]=off; return GhRecKeep(); }

static bool GhRecCap55(ID3D12GraphicsCommandList* self, ID3D12Resource* buf, UINT64 off,
                       D3D12_PREDICATION_OP op)
{ GH_REC_BEGIN(55) _o->a[0]=(UINT64)(uintptr_t)buf; _o->a[1]=off; _o->a[2]=(UINT64)op; return GhRecKeep(); }

static bool GhRecCap57(ID3D12GraphicsCommandList* self, UINT meta, const void* data, UINT size)
{ GH_REC_BEGIN(57) _o->a[0]=meta; _o->nA=(UINT16)(size > 0xFFFF ? 0xFFFF : size);
  _o->off = GhRecBlob(_n, data, _o->nA); return GhRecKeep(); }

static bool GhRecCap58(ID3D12GraphicsCommandList* self)
{ GH_REC_BEGIN(58) return GhRecKeep(); }

static bool GhRecCap59(ID3D12GraphicsCommandList* self, void* sig, UINT maxN,
                       ID3D12Resource* args, UINT64 argOff, ID3D12Resource* cnt, UINT64 cntOff)
{ GH_REC_BEGIN(59) _o->a[0]=(UINT64)(uintptr_t)sig; _o->a[1]=maxN;
  _o->a[2]=(UINT64)(uintptr_t)args; _o->a[3]=argOff;
  _o->a[4]=(UINT64)(uintptr_t)cnt; _o->a[5]=cntOff; return GhRecKeep(); }

#undef GH_REC_BEGIN

// Replay one guest list's recorded stream into a loader-owned list, in call order.
// Every call goes through GhClOrig so it bypasses our own hooks: the replay list shares
// the guest list's vtable, so calling the methods normally would re-enter the capture
// path and record the stream into itself.
template <typename F> static F GhRecFn(ID3D12GraphicsCommandList* d, int slot)
{ return (F)GhClOrig(d, slot); }

static LONG GhRecReplayExtra(ID3D12GraphicsCommandList* d, GhCLNote* n, const struct GhRecOp& o);
static volatile LONG g_recDupOps = 0, g_recDupCopies = 0;

// [from, to) selects a slice of this list's stream; to < 0 means "to the end". The slice
// exists for gh_cl_split, which cuts one submission's stream across several loader-owned
// lists without changing its content, its order or its queue.
static LONG GhRecReplay(ID3D12GraphicsCommandList* d, GhCLNote* n, bool dup = false,
                        LONG from = 0, LONG to = -1)
{
    if (!d || !n || n->ropN <= 0) return 0;
    const LONG end = (to < 0 || to > n->ropN) ? n->ropN : to;
    if (from < 0) from = 0;
    if (from >= end) return 0;
    const bool noBarr = dup && GhClDupNoBarr();
    for (LONG i = from; i < end; ++i){
        const GhRecOp& o = n->rops[i];
        if (noBarr && o.slot == 26) continue;
        // #158. The breadcrumb has to travel WITH the command. GhBreadcrumbBefore writes
        // its marker into whichever list it is handed, so leaving the stamp at the guest
        // hook site while the command moves here makes the breadcrumb describe the
        // guest's empty list -- it reads "fully consumed" no matter what the GPU did.
        // The guest-side stamps are suppressed while recording (see GhBreadcrumbBefore),
        // and re-issued here, around the op they actually belong to.
        const bool isWork = !dup &&
            (o.slot == 12 || o.slot == 13 || o.slot == 14 || o.slot == 59);
        const LONG bcg = isWork ? GhBreadcrumbBefore(d) : 0;
        LONG bck = 0;
        if (o.slot == 26 && !dup){
            bck = GhBcBarrierBefore(d, D3D12_COMMAND_LIST_TYPE_DIRECT);
            GhBarrBcNote(bck, d, o.nA, (const D3D12_RESOURCE_BARRIER*)GhRecAt(n, o.off));
        }
        switch (o.slot){
        case 12: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT,UINT)>(d,12)
                    (d,(UINT)o.a[0],(UINT)o.a[1],(UINT)o.a[2],(UINT)o.a[3]); break;
        case 14: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT)>(d,14)
                    (d,(UINT)o.a[0],(UINT)o.a[1],(UINT)o.a[2]); break;
        case 15: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,ID3D12Resource*,UINT64,
                                          ID3D12Resource*,UINT64,UINT64)>(d,15)
                    (d,(ID3D12Resource*)(uintptr_t)o.a[0],o.a[1],
                       (ID3D12Resource*)(uintptr_t)o.a[2],o.a[3],o.a[4]); break;
        case 16: {
            const D3D12_TEXTURE_COPY_LOCATION* two =
                (const D3D12_TEXTURE_COPY_LOCATION*)GhRecAt(n,o.off);
            const D3D12_BOX* box = (const D3D12_BOX*)GhRecAt(n,o.off2);
            if (!two) break;
            GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,const D3D12_TEXTURE_COPY_LOCATION*,
                                     UINT,UINT,UINT,const D3D12_TEXTURE_COPY_LOCATION*,
                                     const D3D12_BOX*)>(d,16)
                (d,(o.flags&1)?&two[0]:nullptr,(UINT)o.a[0],(UINT)o.a[1],(UINT)o.a[2],
                   (o.flags&2)?&two[1]:nullptr,box);
            break; }
        case 17: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,ID3D12Resource*,
                                          ID3D12Resource*)>(d,17)
                    (d,(ID3D12Resource*)(uintptr_t)o.a[0],(ID3D12Resource*)(uintptr_t)o.a[1]); break;
        case 20: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,D3D12_PRIMITIVE_TOPOLOGY)>(d,20)
                    (d,(D3D12_PRIMITIVE_TOPOLOGY)o.a[0]); break;
        case 21: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,UINT,const D3D12_VIEWPORT*)>(d,21)
                    (d,o.nA,(const D3D12_VIEWPORT*)GhRecAt(n,o.off)); break;
        case 22: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,UINT,const D3D12_RECT*)>(d,22)
                    (d,o.nA,(const D3D12_RECT*)GhRecAt(n,o.off)); break;
        case 23: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,const FLOAT*)>(d,23)
                    (d,(const FLOAT*)GhRecAt(n,o.off)); break;
        case 24: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,UINT)>(d,24)
                    (d,(UINT)o.a[0]); break;
        case 25: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,ID3D12PipelineState*)>(d,25)
                    (d,(ID3D12PipelineState*)(uintptr_t)o.a[0]); break;
        case 26: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,UINT,
                                          const D3D12_RESOURCE_BARRIER*)>(d,26)
                    (d,o.nA,(const D3D12_RESOURCE_BARRIER*)GhRecAt(n,o.off)); break;
        case 28: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,UINT,
                                          ID3D12DescriptorHeap* const*)>(d,28)
                    (d,o.nA,(ID3D12DescriptorHeap* const*)GhRecAt(n,o.off)); break;
        case 29: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,ID3D12RootSignature*)>(d,29)
                    (d,(ID3D12RootSignature*)(uintptr_t)o.a[0]); break;
        case 30: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,ID3D12RootSignature*)>(d,30)
                    (d,(ID3D12RootSignature*)(uintptr_t)o.a[0]); break;
        case 31: case 32: {
            D3D12_GPU_DESCRIPTOR_HANDLE h{}; h.ptr = o.a[1];
            GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,UINT,
                                     D3D12_GPU_DESCRIPTOR_HANDLE)>(d,o.slot)(d,(UINT)o.a[0],h);
            break; }
        case 38: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,UINT,
                                          D3D12_GPU_VIRTUAL_ADDRESS)>(d,38)
                    (d,(UINT)o.a[0],(D3D12_GPU_VIRTUAL_ADDRESS)o.a[1]); break;
        case 43: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,
                                          const D3D12_INDEX_BUFFER_VIEW*)>(d,43)
                    (d,(const D3D12_INDEX_BUFFER_VIEW*)GhRecAt(n,o.off)); break;
        case 44: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,UINT,UINT,
                                          const D3D12_VERTEX_BUFFER_VIEW*)>(d,44)
                    (d,(UINT)o.a[0],o.nA,(const D3D12_VERTEX_BUFFER_VIEW*)GhRecAt(n,o.off)); break;
        case 46: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,UINT,
                                          const D3D12_CPU_DESCRIPTOR_HANDLE*,BOOL,
                                          const D3D12_CPU_DESCRIPTOR_HANDLE*)>(d,46)
                    (d,o.nA,(const D3D12_CPU_DESCRIPTOR_HANDLE*)GhRecAt(n,o.off),(BOOL)o.a[0],
                       (const D3D12_CPU_DESCRIPTOR_HANDLE*)GhRecAt(n,o.off2)); break;
        case 47: {
            D3D12_CPU_DESCRIPTOR_HANDLE h{}; h.ptr = (SIZE_T)o.a[0];
            FLOAT depth; memcpy(&depth,&o.a[2],sizeof depth);
            GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,D3D12_CPU_DESCRIPTOR_HANDLE,
                                     D3D12_CLEAR_FLAGS,FLOAT,UINT8,UINT,const D3D12_RECT*)>(d,47)
                (d,h,(D3D12_CLEAR_FLAGS)o.a[1],depth,(UINT8)o.a[3],o.nA,
                   (const D3D12_RECT*)GhRecAt(n,o.off));
            break; }
        case 48: {
            D3D12_CPU_DESCRIPTOR_HANDLE h{}; h.ptr = (SIZE_T)o.a[0];
            GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,D3D12_CPU_DESCRIPTOR_HANDLE,
                                     const FLOAT*,UINT,const D3D12_RECT*)>(d,48)
                (d,h,(const FLOAT*)GhRecAt(n,o.off),o.nA,(const D3D12_RECT*)GhRecAt(n,o.off2));
            break; }
        case 49: {
            D3D12_GPU_DESCRIPTOR_HANDLE g{}; g.ptr = o.a[0];
            D3D12_CPU_DESCRIPTOR_HANDLE c{}; c.ptr = (SIZE_T)o.a[1];
            GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,D3D12_GPU_DESCRIPTOR_HANDLE,
                                     D3D12_CPU_DESCRIPTOR_HANDLE,ID3D12Resource*,const UINT*,
                                     UINT,const D3D12_RECT*)>(d,49)
                (d,g,c,(ID3D12Resource*)(uintptr_t)o.a[2],(const UINT*)GhRecAt(n,o.off),
                   o.nA,(const D3D12_RECT*)GhRecAt(n,o.off2));
            break; }
        case 51: {
            D3D12_DISCARD_REGION reg{};
            const D3D12_DISCARD_REGION* src = (const D3D12_DISCARD_REGION*)GhRecAt(n,o.off);
            if (src){
                reg = *src;
                reg.pRects = (const D3D12_RECT*)GhRecAt(n,o.off2);   // repoint into the arena
            }
            GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,ID3D12Resource*,
                                     const D3D12_DISCARD_REGION*)>(d,51)
                (d,(ID3D12Resource*)(uintptr_t)o.a[0],(o.flags&1)?&reg:nullptr);
            break; }
        case 53: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,ID3D12QueryHeap*,
                                          D3D12_QUERY_TYPE,UINT)>(d,53)
                    (d,(ID3D12QueryHeap*)(uintptr_t)o.a[0],(D3D12_QUERY_TYPE)o.a[1],
                       (UINT)o.a[2]); break;
        case 54: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,ID3D12QueryHeap*,
                                          D3D12_QUERY_TYPE,UINT,UINT,ID3D12Resource*,UINT64)>(d,54)
                    (d,(ID3D12QueryHeap*)(uintptr_t)o.a[0],(D3D12_QUERY_TYPE)o.a[1],
                       (UINT)o.a[2],(UINT)o.a[3],(ID3D12Resource*)(uintptr_t)o.a[4],o.a[5]); break;
        case 55: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,ID3D12Resource*,UINT64,
                                          D3D12_PREDICATION_OP)>(d,55)
                    (d,(ID3D12Resource*)(uintptr_t)o.a[0],o.a[1],
                       (D3D12_PREDICATION_OP)o.a[2]); break;
        case 57: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,UINT,const void*,UINT)>(d,57)
                    (d,(UINT)o.a[0],GhRecAt(n,o.off),o.nA); break;
        case 58: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*)>(d,58)(d); break;
        case 59: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,ID3D12CommandSignature*,UINT,
                                          ID3D12Resource*,UINT64,ID3D12Resource*,UINT64)>(d,59)
                    (d,(ID3D12CommandSignature*)(uintptr_t)o.a[0],(UINT)o.a[1],
                       (ID3D12Resource*)(uintptr_t)o.a[2],o.a[3],
                       (ID3D12Resource*)(uintptr_t)o.a[4],o.a[5]); break;
        default:
            // Anything the switch above does not know is a command that would silently
            // stay behind. Say so loudly rather than replaying a partial stream.
            if (!GhRecReplayExtra(d, n, o)){
                static LONG shown = 0;
                if (InterlockedIncrement(&shown) <= 8)
                    Diag("CLREC UNHANDLED slot=" + std::to_string((long)o.slot) +
                         " -- the replayed stream is INCOMPLETE, this run is VOID");
                InterlockedIncrement(&g_recOverflow);
            }
            break;
        }
        if (isWork)      GhBreadcrumbAfter(d, bcg);
        if (o.slot == 26 && !dup) GhBcBarrierAfter(d, bck);
    }
    if (dup){
        InterlockedAdd(&g_recDupOps, end - from);
        InterlockedIncrement(&g_recDupCopies);
    } else {
        InterlockedIncrement(&g_recReplays);
    }
    return end - from;
}

// --- the rest of the stream methods -------------------------------------------------
// Some of these the census counted zero times in its window (13, 19, 27, 40, 42, 50, 52).
// They are captured anyway: "not called in the first 44 submissions" is not "never
// called", and an uncaptured method is a command that silently stays in the guest's list
// while everything around it moves, which would corrupt the experiment rather than fail
// it.
static bool GhRecCap13(ID3D12GraphicsCommandList* self, UINT a, UINT b, UINT c, INT d, UINT e)
{ GhCLNote* _n = GhRecOn(self); if (!_n) return false;
  GhRecOp* _o = GhRecPush(_n, 13); if (!_o) return false;
  _o->a[0]=a; _o->a[1]=b; _o->a[2]=c; _o->a[3]=(UINT64)(INT64)d; _o->a[4]=e; return GhRecKeep(); }

static bool GhRecCap19(ID3D12GraphicsCommandList* self, ID3D12Resource* d, UINT ds,
                       ID3D12Resource* s, UINT ss, DXGI_FORMAT f)
{ GhCLNote* _n = GhRecOn(self); if (!_n) return false;
  GhRecOp* _o = GhRecPush(_n, 19); if (!_o) return false;
  _o->a[0]=(UINT64)(uintptr_t)d; _o->a[1]=ds; _o->a[2]=(UINT64)(uintptr_t)s;
  _o->a[3]=ss; _o->a[4]=(UINT64)f; return GhRecKeep(); }

static bool GhRecCap27(ID3D12GraphicsCommandList* self, ID3D12GraphicsCommandList* b)
{ GhCLNote* _n = GhRecOn(self); if (!_n) return false;
  GhRecOp* _o = GhRecPush(_n, 27); if (!_o) return false;
  _o->a[0]=(UINT64)(uintptr_t)b; return GhRecKeep(); }

static bool GhRecCapVA(ID3D12GraphicsCommandList* self, UINT16 slot, UINT i,
                       D3D12_GPU_VIRTUAL_ADDRESS va)
{ GhCLNote* _n = GhRecOn(self); if (!_n) return false;
  GhRecOp* _o = GhRecPush(_n, slot); if (!_o) return false;
  _o->a[0]=i; _o->a[1]=va; return GhRecKeep(); }

static bool GhRecCap50(ID3D12GraphicsCommandList* self, D3D12_GPU_DESCRIPTOR_HANDLE g,
                       D3D12_CPU_DESCRIPTOR_HANDLE c, ID3D12Resource* res,
                       const FLOAT v[4], UINT nr, const D3D12_RECT* r)
{ GhCLNote* _n = GhRecOn(self); if (!_n) return false;
  GhRecOp* _o = GhRecPush(_n, 50); if (!_o) return false;
  _o->a[0]=g.ptr; _o->a[1]=c.ptr; _o->a[2]=(UINT64)(uintptr_t)res; _o->nA=(UINT16)nr;
  _o->off  = GhRecBlob(_n, v, 4*sizeof(FLOAT));
  _o->off2 = GhRecBlob(_n, r, (size_t)nr*sizeof(*r)); return GhRecKeep(); }

static bool GhRecCap52(ID3D12GraphicsCommandList* self, ID3D12QueryHeap* h,
                       D3D12_QUERY_TYPE t, UINT i)
{ GhCLNote* _n = GhRecOn(self); if (!_n) return false;
  GhRecOp* _o = GhRecPush(_n, 52); if (!_o) return false;
  _o->a[0]=(UINT64)(uintptr_t)h; _o->a[1]=(UINT64)t; _o->a[2]=i; return GhRecKeep(); }

static LONG GhRecReplayExtra(ID3D12GraphicsCommandList* d, GhCLNote* n, const GhRecOp& o)
{
    switch (o.slot){
    case 13: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,UINT,UINT,UINT,INT,UINT)>(d,13)
                (d,(UINT)o.a[0],(UINT)o.a[1],(UINT)o.a[2],(INT)(INT64)o.a[3],(UINT)o.a[4]); return 1;
    case 19: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,ID3D12Resource*,UINT,
                                      ID3D12Resource*,UINT,DXGI_FORMAT)>(d,19)
                (d,(ID3D12Resource*)(uintptr_t)o.a[0],(UINT)o.a[1],
                   (ID3D12Resource*)(uintptr_t)o.a[2],(UINT)o.a[3],(DXGI_FORMAT)o.a[4]); return 1;
    case 27: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,
                                      ID3D12GraphicsCommandList*)>(d,27)
                (d,(ID3D12GraphicsCommandList*)(uintptr_t)o.a[0]); return 1;
    case 40: case 42:
        GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,UINT,
                                 D3D12_GPU_VIRTUAL_ADDRESS)>(d,o.slot)
            (d,(UINT)o.a[0],(D3D12_GPU_VIRTUAL_ADDRESS)o.a[1]); return 1;
    case 50: {
        D3D12_GPU_DESCRIPTOR_HANDLE g{}; g.ptr = o.a[0];
        D3D12_CPU_DESCRIPTOR_HANDLE c{}; c.ptr = (SIZE_T)o.a[1];
        GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,D3D12_GPU_DESCRIPTOR_HANDLE,
                                 D3D12_CPU_DESCRIPTOR_HANDLE,ID3D12Resource*,const FLOAT*,
                                 UINT,const D3D12_RECT*)>(d,50)
            (d,g,c,(ID3D12Resource*)(uintptr_t)o.a[2],(const FLOAT*)GhRecAt(n,o.off),
               o.nA,(const D3D12_RECT*)GhRecAt(n,o.off2));
        return 1; }
    case 52: GhRecFn<void(__stdcall*)(ID3D12GraphicsCommandList*,ID3D12QueryHeap*,
                                      D3D12_QUERY_TYPE,UINT)>(d,52)
                (d,(ID3D12QueryHeap*)(uintptr_t)o.a[0],(D3D12_QUERY_TYPE)o.a[1],
                   (UINT)o.a[2]); return 1;
    default: return 0;
    }
}

// --- gh_sub_census: what does one submission actually reference? --------------------
//
// A sorted set of UINT64 keys. Sorted-insert rather than a linear scan because the big
// startup list carries ~2600 ops and the counts have to be exact, not sampled. Overflow
// is counted, never silently dropped: a saturated set would read as a plateau and
// "the count stopped growing" is exactly the shape of the answer being looked for.
struct GhUniqSet {
    static const int CAP = 4096;
    UINT64 v[CAP];
    int    n;
    int    over;
    // #158. Every ranked candidate for the consumable lost to the raw op count
    // (CV 3.46% vs 6.83% for uniqRes), but "unique per submission, summed over
    // submissions" is a poor proxy for what a kernel patch-location list actually
    // holds, which is one entry per REFERENCE, not per distinct object. That
    // quantity has never been measured. It is one counter.
    int    tot;
    void clear(){ n = 0; over = 0; tot = 0; }
    void add(UINT64 x){
        if (!x) return;                       // null resource / unset handle is not a reference
        ++tot;
        int lo = 0, hi = n;
        while (lo < hi){ const int m = (lo + hi) >> 1; if (v[m] < x) lo = m + 1; else hi = m; }
        if (lo < n && v[lo] == x) return;
        if (n >= CAP){ ++over; return; }
        memmove(&v[lo + 1], &v[lo], (size_t)(n - lo) * sizeof(UINT64));
        v[lo] = x; ++n;
    }
    // The array is kept sorted, so the value RANGE is free -- and the range is the
    // question: five submissions with identical COUNTS say nothing about whether
    // they name the same objects or fresh ones marching through a ring.
    UINT64 lo_() const { return n ? v[0] : 0; }
    UINT64 hi_() const { return n ? v[n - 1] : 0; }
    // How many of our members also appear in o. Both sides sorted -> one merge walk.
    int isect(const GhUniqSet& o) const {
        int i = 0, j = 0, c = 0;
        while (i < n && j < o.n){
            if      (v[i] < o.v[j]) ++i;
            else if (v[i] > o.v[j]) ++j;
            else { ++c; ++i; ++j; }
        }
        return c;
    }
};

struct GhSubCen {
    GhUniqSet res, va, heap, pso, tbl, cpuh, qheap, rsig;
    LONG ops, work, draws, disp, indirect, barriers, maxListOps;
    void clear(){
        res.clear(); va.clear(); heap.clear(); pso.clear();
        tbl.clear(); cpuh.clear(); qheap.clear(); rsig.clear();
        ops = work = draws = disp = indirect = barriers = maxListOps = 0;
    }
};

// Every slot that names something the kernel would have to put in an allocation or
// patch-location list. Slot numbers are the vtable slots the census of 0.0.277.0 found.
static void GhSubCenOp(GhSubCen& c, GhCLNote* n, const GhRecOp& o)
{
    ++c.ops;
    switch (o.slot){
    case 12: ++c.draws; ++c.work; break;
    case 13: ++c.draws; ++c.work; break;
    case 14: ++c.disp;  ++c.work; break;
    case 59: ++c.indirect; ++c.work;
             c.res.add(o.a[0]);              // the command signature
             c.res.add(o.a[2]);              // argument buffer
             c.res.add(o.a[4]);              // count buffer
             break;
    case 15: c.res.add(o.a[0]); c.res.add(o.a[2]); break;
    case 17: c.res.add(o.a[0]); c.res.add(o.a[1]); break;
    case 19: c.res.add(o.a[0]); c.res.add(o.a[2]); break;
    case 16: {
        const D3D12_TEXTURE_COPY_LOCATION* two =
            (const D3D12_TEXTURE_COPY_LOCATION*)GhRecAt(n, o.off);
        if (two){
            if (o.flags & 1) c.res.add((UINT64)(uintptr_t)two[0].pResource);
            if (o.flags & 2) c.res.add((UINT64)(uintptr_t)two[1].pResource);
        }
        break; }
    case 25: c.pso.add(o.a[0]); break;
    case 26: {
        ++c.barriers;
        const D3D12_RESOURCE_BARRIER* b = (const D3D12_RESOURCE_BARRIER*)GhRecAt(n, o.off);
        if (b) for (UINT i = 0; i < o.nA; ++i){
            if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
                c.res.add((UINT64)(uintptr_t)b[i].Transition.pResource);
            else if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_UAV)
                c.res.add((UINT64)(uintptr_t)b[i].UAV.pResource);
            else if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING){
                c.res.add((UINT64)(uintptr_t)b[i].Aliasing.pResourceBefore);
                c.res.add((UINT64)(uintptr_t)b[i].Aliasing.pResourceAfter);
            }
        }
        break; }
    case 28: {
        ID3D12DescriptorHeap* const* h = (ID3D12DescriptorHeap* const*)GhRecAt(n, o.off);
        if (h) for (UINT i = 0; i < o.nA; ++i) c.heap.add((UINT64)(uintptr_t)h[i]);
        break; }
    case 29: case 30: c.rsig.add(o.a[0]); break;
    case 31: case 32: c.tbl.add(o.a[1]); break;             // GPU descriptor-table handle
    case 38: case 40: case 42: c.va.add(o.a[1]); break;     // root CBV / SRV / UAV
    case 43: {
        const D3D12_INDEX_BUFFER_VIEW* v = (const D3D12_INDEX_BUFFER_VIEW*)GhRecAt(n, o.off);
        if (v) c.va.add((UINT64)v->BufferLocation);
        break; }
    case 44: {
        const D3D12_VERTEX_BUFFER_VIEW* v = (const D3D12_VERTEX_BUFFER_VIEW*)GhRecAt(n, o.off);
        if (v) for (UINT i = 0; i < o.nA; ++i) c.va.add((UINT64)v[i].BufferLocation);
        break; }
    case 46: {
        const D3D12_CPU_DESCRIPTOR_HANDLE* r = (const D3D12_CPU_DESCRIPTOR_HANDLE*)GhRecAt(n, o.off);
        const UINT cnt = o.a[0] ? 1u : (UINT)o.nA;
        if (r) for (UINT i = 0; i < cnt; ++i) c.cpuh.add((UINT64)r[i].ptr);
        if (const D3D12_CPU_DESCRIPTOR_HANDLE* d = (const D3D12_CPU_DESCRIPTOR_HANDLE*)GhRecAt(n, o.off2))
            c.cpuh.add((UINT64)d->ptr);
        break; }
    case 47: case 48: c.cpuh.add(o.a[0]); break;
    case 49: case 50: c.res.add(o.a[2]); c.cpuh.add(o.a[1]); c.tbl.add(o.a[0]); break;
    case 51: c.res.add(o.a[0]); break;
    case 52: case 53: c.qheap.add(o.a[0]); break;
    case 54: c.qheap.add(o.a[0]); c.res.add(o.a[4]); break;
    case 55: c.res.add(o.a[0]); break;
    default: break;
    }
}

// #158: the counts alone could not separate "the same 73 handles reused every frame"
// from "73 fresh handles per frame out of a ring that runs out on the fifth". Keep one
// full startup cycle of history (the cycle is six submissions) and diff each submission
// against the most recent EARLIER one of the same shape -- which pairs #35 with #29,
// #29 with #23, and so on. Only the four value-carrying sets are retained.
struct GhCenHist {
    LONG      seq, ops, draws;
    GhUniqSet va, tbl, cpuh, res;
};
static const int kCenHist = 16;          // two full 6-submission cycles, plus slack
static GhCenHist g_cenHist[kCenHist]{};
static int       g_cenHistN = 0;

// A shader-visible table handle is a GPU address inside some descriptor heap. Its
// INDEX within that heap is the unit that matters: a ring shows up as an index that
// climbs each frame and then wraps or runs past num.
static void GhCenTblIndex(const GhUniqSet& tbl, UINT64* idxLo, UINT64* idxHi,
                          UINT32* heapNum, int* unresolved)
{
    *idxLo = ~0ull; *idxHi = 0; *heapNum = 0; *unresolved = 0;
    LONG hn = g_dhN; if (hn > 128) hn = 128;
    for (int k = 0; k < tbl.n; ++k){
        const UINT64 g = tbl.v[k];
        bool found = false;
        for (LONG i = 0; i < hn; ++i){
            const GhDHNote& d = g_dh[i];
            if (!d.gpu0 || !d.inc) continue;
            if (g >= d.gpu0 && g < d.gpu0 + (UINT64)d.inc * d.num){
                const UINT64 idx = (g - d.gpu0) / d.inc;
                if (idx < *idxLo) *idxLo = idx;
                if (idx > *idxHi) *idxHi = idx;
                if (d.num > *heapNum) *heapNum = d.num;
                found = true; break;
            }
        }
        if (!found) ++*unresolved;
    }
    if (*idxLo == ~0ull) *idxLo = 0;
}

// One line per submission. Called from Hook_CQExecute BEFORE the submission goes down,
// so a line exists for the fatal one even though it never retires.
static void GhSubCensusEmit(LONG seq, UINT n, ID3D12CommandList* const* l)
{
    if (!GhSubCensus() || !n || !l) return;
    // gh_sub_census = N: stop after N submissions so a surviving run cannot drown the
    // log. The hang lives in the first hundred; 1 means "no limit" for the default use.
    const unsigned lim = GhSubCensus();
    if (lim > 1 && (unsigned)seq > lim) return;

    // 256 KB, reused rather than stack-allocated. Its own lock: g_recLk is declared
    // further down guest_loader.cpp, and the census must not serialize against replay.
    static GhSubCen c;
    static SRWLOCK cenLk = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&cenLk);
    c.clear();
    LONG tracked = 0;
    for (UINT i = 0; i < n; ++i){
        GhCLNote* nt = GhCLFind(l[i]);
        if (!nt || nt->ropN <= 0) continue;
        ++tracked;
        if (nt->ropN > c.maxListOps) c.maxListOps = nt->ropN;
        for (LONG k = 0; k < nt->ropN; ++k) GhSubCenOp(c, nt, nt->rops[k]);
    }
    const int over = c.res.over + c.va.over + c.heap.over + c.pso.over +
                     c.tbl.over + c.cpuh.over + c.qheap.over + c.rsig.over;
    std::string m = "SUBCENSUS #" + std::to_string((long)seq) +
        " lists=" + std::to_string((unsigned)n) +
        " tracked=" + std::to_string((long)tracked) +
        " ops=" + std::to_string((long)c.ops) +
        " maxlist=" + std::to_string((long)c.maxListOps) +
        " work=" + std::to_string((long)c.work) +
        " draws=" + std::to_string((long)c.draws) +
        " disp=" + std::to_string((long)c.disp) +
        " indirect=" + std::to_string((long)c.indirect) +
        " barrops=" + std::to_string((long)c.barriers) +
        " uniqRes=" + std::to_string(c.res.n) +
        " uniqVA=" + std::to_string(c.va.n) +
        " uniqHeap=" + std::to_string(c.heap.n) +
        " uniqPSO=" + std::to_string(c.pso.n) +
        " uniqTbl=" + std::to_string(c.tbl.n) +
        " uniqCPUh=" + std::to_string(c.cpuh.n) +
        " uniqQHeap=" + std::to_string(c.qheap.n) +
        " uniqRSig=" + std::to_string(c.rsig.n) +
        " over=" + std::to_string(over) +
        // non-unique reference counts: the patch-list hypothesis reads these, not uniq*
        " refRes=" + std::to_string(c.res.tot) +
        " refVA=" + std::to_string(c.va.tot) +
        " refTbl=" + std::to_string(c.tbl.tot) +
        " refCPUh=" + std::to_string(c.cpuh.tot) +
        " refHeap=" + std::to_string(c.heap.tot) +
        " refPSO=" + std::to_string(c.pso.tot);

    // VALUE RANGES. Hex, because these are GPU addresses and handles.
    char rb[320];
    UINT64 idxLo = 0, idxHi = 0; UINT32 heapNum = 0; int unres = 0;
    GhCenTblIndex(c.tbl, &idxLo, &idxHi, &heapNum, &unres);
    std::snprintf(rb, sizeof rb,
        " vaLo=0x%llX vaHi=0x%llX vaSpan=%llu"
        " tblLo=0x%llX tblHi=0x%llX tblIdx=%llu..%llu/%u tblUnres=%d"
        " cpuhLo=0x%llX cpuhHi=0x%llX",
        (unsigned long long)c.va.lo_(),  (unsigned long long)c.va.hi_(),
        (unsigned long long)(c.va.n ? c.va.hi_() - c.va.lo_() : 0),
        (unsigned long long)c.tbl.lo_(), (unsigned long long)c.tbl.hi_(),
        (unsigned long long)idxLo, (unsigned long long)idxHi, heapNum, unres,
        (unsigned long long)c.cpuh.lo_(), (unsigned long long)c.cpuh.hi_());
    m += rb;

    // DIFF against the most recent earlier submission of the same shape. reuse=N/M
    // means N of this submission's M values also appeared in that one, so reuse==M
    // is "identical set" and reuse==0 is "entirely fresh".
    // The frame program is DOUBLE BUFFERED -- #11/#23/#35 share one set of addresses
    // and #17/#29 the other -- so a diff against the immediately preceding same-shape
    // submission always compares opposite buffers and reads 0 by construction. Report
    // the two most recent, which puts the SAME buffer's previous use in the -2 slot.
    const GhCenHist* p1 = nullptr; const GhCenHist* p2 = nullptr;
    for (int i = 0; i < kCenHist; ++i){
        const GhCenHist& h = g_cenHist[i];
        if (!h.seq || h.seq >= seq) continue;
        if (h.ops != c.ops || h.draws != c.draws) continue;
        if (!p1 || h.seq > p1->seq){ p2 = p1; p1 = &h; }
        else if (!p2 || h.seq > p2->seq) p2 = &h;
    }
    for (int which = 0; which < 2; ++which){
        const GhCenHist* pv = which ? p2 : p1;
        if (!pv) continue;
        char db[224];
        std::snprintf(db, sizeof db,
            " | vs #%ld reuseVA=%d/%d reuseTbl=%d/%d reuseCPUh=%d/%d reuseRes=%d/%d",
            (long)pv->seq,
            c.va.isect(pv->va),     c.va.n,
            c.tbl.isect(pv->tbl),   c.tbl.n,
            c.cpuh.isect(pv->cpuh), c.cpuh.n,
            c.res.isect(pv->res),   c.res.n);
        m += db;
    }
    {   // retain this submission for the next comparison
        GhCenHist& h = g_cenHist[g_cenHistN++ % kCenHist];
        h.seq = seq; h.ops = c.ops; h.draws = c.draws;
        h.va = c.va; h.tbl = c.tbl; h.cpuh = c.cpuh; h.res = c.res;
    }
    ReleaseSRWLockExclusive(&cenLk);
    Diag(m);
}
