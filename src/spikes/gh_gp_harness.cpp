// gh_gp_harness.cpp — standalone VM harness to crack the games-played login wall.
//
// On the real Xbox we send a byte-perfect CMsgClientGamesPlayed
// (correct real SteamID in the header, valid client_sessionid, sendw=1, transmitted)
// yet the account's profile never flips to "playing Lethal Company". The wall is the
// LOGIN SESSION, not the message: our login upgrades an anonymous CreateLocalUser to
// the real account via Authentication.BeginAuthSessionViaQR and LogOn's with the
// REFRESH token. Leading hypothesis: Steam doesn't treat a QR-refresh-token upgrade
// session as the account's active game-playing client (we likely need to exchange
// refresh -> ACCESS token first).
//
// Iterating that on the Xbox is minutes per loop. This harness runs the SAME bring-up
// + QR login + games-played send as its OWN native process on the control VM (a full
// Steam install), so compile -> scp -> run -> read-stdout is seconds. It answers:
// is it our login, or is it Xbox-environment-specific?  Ported from the proven flow in
// src/spikes/steamclient_probe.cpp (which logged in a real account on real Xbox) + the
// PATH-B send from corpus/shims/gh_steam_engine.h, retargeted to the VM's 10.87.82.64
// steamclient64.dll RVAs.
//
// CREDENTIAL BOUNDARY (unchanged from the probe): only the user scans/approves. The
// refresh token AND (E1) the access token live only in local buffers, are fed straight
// to the engine, then scrubbed — they are NEVER printed or written to disk. Only the
// PUBLIC challenge_url and the (public) SteamID are ever externalized.
//
// Build:  toolchain/build.sh src/spikes/gh_gp_harness.cpp gh_gp_harness.exe
// Run  :  from the Steam install dir (so steamclient64.dll + tier0_s64.dll +
//         vstdlib_s64.dll resolve), with retail Steam CLOSED and the test account playing
//         nowhere else. Drives QR login; user scans the printed challenge_url.
//
// Experiment switches (compile-time; one variant per build — see §26.4 ladder):
//   (default)             E0: current refresh-token login  (reproduce the wall)
//   -DGH_E1_TOKEN_EXCHANGE E1: exchange refresh->access token before LogOn

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cstdint>

typedef void* (__cdecl *CreateInterfaceFn)(const char* pName, int* pReturnCode);
typedef int32_t HSteamPipe;
typedef int32_t HSteamUser;

// psapi.h is excluded by WIN32_LEAN_AND_MEAN; declare the one struct we use.
typedef struct _GH_MODULEINFO { LPVOID lpBaseOfDll; DWORD SizeOfImage; LPVOID EntryPoint; } GH_MODULEINFO, *GH_LPMODULEINFO;

// ---- steamclient64.dll 10.87.82.64 RVAs (control VM) ------------------------
// Send path (PATH B, live-validated on 10.87):
namespace rva {
    // CProtoBufMsg<CMsgClientGamesPlayed> construct + populate + send. (For an EMPTY body these serialize to
    // nothing regardless of T, so this same scaffold sends a valid empty ClientChangeStatus (EMsg 0x2cc) too —
    // the header EMsg comes from Ctor, not the type-vtable.)
#ifdef GH_SC1041
    // 10.41.26.25 send-path RVAs (the Xbox build). WITHOUT these the 10.87
    // defaults jump into wrong code on 10.41 -> send_empty_emsg/send_gp_via were silently broken on 10.41.
    constexpr uintptr_t Ctor            = 0xcd8350;
    constexpr uintptr_t TypeVtbl        = 0x12fe6f0;
    constexpr uintptr_t Descriptor      = 0x167bd80;
    constexpr uintptr_t BuildBody       = 0xcdae80;
    constexpr uintptr_t AddGamesPlayed  = 0x9c1cb0;
    constexpr uintptr_t SendWrapper     = 0x5804a0;
    constexpr uintptr_t Dtor            = 0xcd85f0;
    constexpr uintptr_t ByteSize        = 0xcda5e0;
    constexpr uintptr_t SerializeToArray= 0xcda860;
#else
    constexpr uintptr_t Ctor            = 0xcfc8f0;   // (rcx=msg, edx=EMsg=0x1522, r8d=0)
    constexpr uintptr_t TypeVtbl        = 0x1336f30;  // CProtoBufMsg<CMsgClientGamesPlayed> type-vtable
    constexpr uintptr_t Descriptor      = 0x16f38b0;  // CMsgClientGamesPlayed descriptor / default-instance
    constexpr uintptr_t BuildBody       = 0xcff490;   // New()s empty body -> [msg+0x30]
    constexpr uintptr_t AddGamesPlayed  = 0x9e2ea0;   // RepeatedPtrField::Add on games_played@body+0x18
    constexpr uintptr_t SendWrapper     = 0x590b00;   // (rcx=CCMInterface, rdx=msg) -> bool  (finalize+BAsyncSend)
    constexpr uintptr_t Dtor            = 0xcfcba0;   // (rcx=msg)
    constexpr uintptr_t ByteSize        = 0xcfeb90;   // type-vtbl slot 3 (+0x18)
    constexpr uintptr_t SerializeToArray= 0xcfee60;   // type-vtbl slot 4 (+0x20)
#endif
    // Routing / identity vtables (for the CCM heap-scan + wrong-object guard).
#ifdef GH_SC1041
    // 10.41.26.25 (the Xbox steamclient) — to reproduce the Xbox AddClientGame crash off-Xbox.
    constexpr uintptr_t CCMInterfaceVtbl= 0x12491d8;  // RTTI-confirmed (10.41)
    constexpr uintptr_t CUserVtbl       = 0x12faa40;  // real logged-on CUser (10.41)
    constexpr uintptr_t AddClientGame   = 0x9933a0;   // CUser::AddClientGame (10.41)
    constexpr uintptr_t GamesListCount  = 0x24d8;     // CUser+0x24d8 (10.41 count)
    constexpr uintptr_t GuardSteamID    = 0x23c;      // CUser+0x23c (10.41)
#else
    constexpr uintptr_t CCMInterfaceVtbl= 0x1279888;  // RTTI-confirmed (10.87)
    constexpr uintptr_t CUserVtbl       = 0x1333178;  // real logged-on CUser (its +0x48 is the persona CM)
    constexpr uintptr_t AddClientGame   = 0x9b40d0;   // CUser::AddClientGame (10.87) — the real register path
    constexpr uintptr_t GamesListCount  = 0x2548;     // CUser+0x2548 (10.87 count)
    constexpr uintptr_t GuardSteamID    = 0x23a;      // CUser+0x23a (10.87)
#endif
    // IClientUser flat wrapper vtable (sanity: what GetIClientUser returns).
    // (Recorded for the version guard; harmless if it drifts — we only warn.)
}
constexpr uint32_t kAppId   = 1966720;                    // Lethal Company
// SteamID64 of the test account, used only to cross-check the message header in a log
// line. Read from GH_ACCT_SID (decimal); unset prints 0.
static uint64_t acct_sid(){ const char* s=getenv("GH_ACCT_SID"); return s ? _strtoui64(s,nullptr,10) : 0; }
constexpr uint64_t kAnonSid = 0x01A0000000000000ULL;      // Public/AnonUser

// ================= minimal protobuf wire codec (Authentication) =============
static int pb_varint(unsigned char* p, uint64_t v){ int n=0; while(v>=0x80){p[n++]=(unsigned char)(v|0x80); v>>=7;} p[n++]=(unsigned char)v; return n; }
static int pb_tag(unsigned char* p,int f,int wt){ return pb_varint(p,((uint64_t)f<<3)|wt); }
static int pb_str(unsigned char* p,int f,const char* s){ int n=pb_tag(p,f,2); size_t L=strlen(s); n+=pb_varint(p+n,L); memcpy(p+n,s,L); return n+(int)L; }
static int pb_int(unsigned char* p,int f,uint64_t v){ int n=pb_tag(p,f,0); return n+pb_varint(p+n,v); }
static int pb_fixed64(unsigned char* p,int f,uint64_t v){ int n=pb_tag(p,f,1); memcpy(p+n,&v,8); return n+8; }
static uint64_t pb_getvar(const unsigned char* b,uint32_t len,uint32_t* off){ uint64_t v=0; int sh=0; while(*off<len){ unsigned char c=b[(*off)++]; v|=(uint64_t)(c&0x7f)<<sh; sh+=7; if(!(c&0x80)) break; } return v; }
static bool pb_find(const unsigned char* b,uint32_t len,int want,const unsigned char** ps,uint32_t* pl,uint64_t* pv){
    uint32_t off=0;
    while(off<len){ uint64_t tag=pb_getvar(b,len,&off); int f=(int)(tag>>3),wt=(int)(tag&7);
        if(wt==0){ uint64_t v=pb_getvar(b,len,&off); if(f==want){ if(pv)*pv=v; return true; } }
        else if(wt==2){ uint64_t L=pb_getvar(b,len,&off); if(off+L>len) break; if(f==want){ if(ps)*ps=b+off; if(pl)*pl=(uint32_t)L; return true; } off+=(uint32_t)L; }
        else if(wt==5) off+=4; else if(wt==1) off+=8; else break; }
    return false;
}

// ---- JWT 'sub' SteamID extraction (in-process; token never printed) --------
static uint64_t jwt_sub_steamid(const char* jwt){
    const char* d1=strchr(jwt,'.'); if(!d1) return 0;
    const char* d2=strchr(d1+1,'.'); if(!d2) return 0;
    const char* p=d1+1; size_t n=(size_t)(d2-p);
    signed char tbl[256]; for(int i=0;i<256;i++) tbl[i]=-1;
    for(int i=0;i<26;i++){ tbl['A'+i]=i; tbl['a'+i]=26+i; }
    for(int i=0;i<10;i++) tbl['0'+i]=52+i; tbl[(unsigned char)'-']=62; tbl[(unsigned char)'_']=63;
    unsigned char* out=(unsigned char*)malloc(n+4); if(!out) return 0;
    size_t ol=0; uint32_t acc=0; int bits=0;
    for(size_t i=0;i<n;i++){ signed char v=tbl[(unsigned char)p[i]]; if(v<0) continue; acc=(acc<<6)|v; bits+=6; if(bits>=8){ bits-=8; out[ol++]=(unsigned char)(acc>>bits); } }
    out[ol]=0; uint64_t sid=0;
    const char* s=strstr((char*)out,"\"sub\"");
    if(s){ s+=5; while(*s&&(*s==':'||*s=='"'||*s==' ')) ++s; while(*s>='0'&&*s<='9'){ sid=sid*10+(uint64_t)(*s-'0'); ++s; } }
    free(out); return sid;
}

// ================= AV-safe memory probe (VirtualQuery-gated) ================
// steamclient's vectored exception handler kills the process on a wild read even
// under __try, so ALL engine-memory reads go through this.
static bool probe_read(const void* addr, void* out, size_t n){
    if(!addr) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if(VirtualQuery(addr,&mbi,sizeof(mbi))!=sizeof(mbi)) return false;
    if(mbi.State!=MEM_COMMIT) return false;
    if(mbi.Protect&(PAGE_GUARD|PAGE_NOACCESS)) return false;
    const DWORD rd=PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY;
    if(!((mbi.Protect&0xFF)&rd)) return false;
    uintptr_t regEnd=(uintptr_t)mbi.BaseAddress+mbi.RegionSize;
    if((uintptr_t)addr+n>regEnd) return false;
    memcpy(out,addr,n); return true;
}
static uintptr_t probe_ptr(const void* a){ uintptr_t v=0; return probe_read(a,&v,sizeof(v))?v:0; }
static uint32_t  probe_u32(const void* a){ uint32_t  v=0; probe_read(a,&v,sizeof(v)); return v; }

// MSVC x64 RTTI mangled class name for a COM-style object (all reads AV-safe via probe_read).
// vtable[-1] -> COL; COL+20 = pSelf (image-rel) -> module base; COL+12 = pTypeDescriptor (image-rel);
// name (".?AV<Class>@@") at TypeDescriptor+16.
static const char* gh_rtti(void* iface){
    static char buf[192]; buf[0]=0;
    uintptr_t vt=probe_ptr(iface); if(!vt) return buf;
    uintptr_t col=0; if(!probe_read((void*)(vt-8),&col,8)||!col) return buf;
    uint32_t pSelf=0,tdRva=0;
    if(!probe_read((void*)(col+20),&pSelf,4)) return buf;
    if(!probe_read((void*)(col+12),&tdRva,4)) return buf;
    uintptr_t modbase=col-pSelf; const char* name=(const char*)(modbase+tdRva)+16;
    int i=0; for(;i<191;i++){ char c=0; if(!probe_read(name+i,&c,1)||!c) break; buf[i]=c; }
    buf[i]=0; if(!(buf[0]=='.'&&buf[1]=='?'&&buf[2]=='A')) buf[0]=0;
    return buf;
}

// ================= SEH-isolated engine calls ================================
static int call_i(void* self, void* fn, const char* nm){
    typedef int (*F)(void*); int v=-999;
    __try{ v=((F)fn)(self); } __except(EXCEPTION_EXECUTE_HANDLER){ printf("GPH %s EXC 0x%08lx\n",nm,GetExceptionCode()); }
    return v;
}
static void iu_logon(void* self, uint64_t steamID){
    typedef void(*F)(void*,uint64_t); void** vt=*(void***)self;
    __try{ ((F)vt[1])(self,steamID); } __except(EXCEPTION_EXECUTE_HANDLER){ printf("GPH LogOn EXC\n"); }
}
static void iu_logoff(void* self){ typedef void(*F)(void*); void** vt=*(void***)self; __try{((F)vt[3])(self);}__except(EXCEPTION_EXECUTE_HANDLER){} }
static void iu_set_token(void* self,const char* tok,const char* acct){ typedef void(*F)(void*,const char*,const char*); void** vt=*(void***)self; __try{((F)vt[56])(self,tok,acct);}__except(EXCEPTION_EXECUTE_HANDLER){printf("GPH SetToken EXC\n");} }
static void iu_set_acct(void* self,const char* acct,bool b){ typedef void(*F)(void*,const char*,bool); void** vt=*(void***)self; __try{((F)vt[50])(self,acct,b);}__except(EXCEPTION_EXECUTE_HANDLER){printf("GPH SetAcct EXC\n");} }
static uint64_t iu_getsteamid(void* self){ typedef void*(*F)(void*,uint64_t*); void** vt=*(void***)self; uint64_t out=0; __try{((F)vt[10])(self,&out);}__except(EXCEPTION_EXECUTE_HANDLER){} return out; }

// IClientUnifiedMessages (engine slot 52): SendMethod[0], GetMethodResponseInfo[1], GetMethodResponseData[2]
static uint64_t um_send(void* self,const char* name,const void* proto,uint32_t cub){
    typedef uint64_t(*F)(void*,const char*,const void*,uint32_t,uint64_t); void** vt=*(void***)self; uint64_t h=0;
    __try{ h=((F)vt[0])(self,name,proto,cub,0); } __except(EXCEPTION_EXECUTE_HANDLER){ printf("GPH SendMethod EXC\n"); }
    return h;
}
static bool um_info(void* self,uint64_t hc,uint32_t* sz,int* res){
    typedef bool(*F)(void*,uint64_t,uint32_t*,int*); void** vt=*(void***)self; bool r=false;
    __try{ r=((F)vt[1])(self,hc,sz,res); } __except(EXCEPTION_EXECUTE_HANDLER){}
    return r;
}
static bool um_data(void* self,uint64_t hc,void* buf,uint32_t cub,bool rel){
    typedef bool(*F)(void*,uint64_t,void*,uint32_t,bool); void** vt=*(void***)self; bool r=false;
    __try{ r=((F)vt[2])(self,hc,buf,cub,rel); } __except(EXCEPTION_EXECUTE_HANDLER){}
    return r;
}

// ---- Steam_RunFrame + callback pump: a real client runs these continuously. Without them
// steamclient's client-manager subsystems don't advance (server go-online announce, heartbeats,
// active-game-client registration), which may be why games-played never surfaces. -----------
static void* g_runframe;   // Steam_RunFrame export
static void* g_bgetcb;     // Steam_BGetCallback(pipe,&msg)
static void* g_freecb;     // Steam_FreeLastCallback(pipe)
// Callback histogram. We dequeued and discarded callbacks for the whole project without ever
// looking at them, so "the friends subsystem is inert" was never actually measured — only inferred
// from an oracle that reads 0 for everyone. The callback id encodes its subsystem
// (id/100: 1=SteamUser, 3=SteamFriends, 4=SteamApps, 5=SteamMatchmaking, ...), so a histogram says
// directly which subsystems the engine considers live on OUR pipe.
static uint32_t g_cb_ids[64]; static uint32_t g_cb_cnt[64]; static int g_cb_n; static uint64_t g_cb_total;
static void cb_note(uint32_t id){
    g_cb_total++;
    for(int i=0;i<g_cb_n;i++) if(g_cb_ids[i]==id){ g_cb_cnt[i]++; return; }
    if(g_cb_n<64){ g_cb_ids[g_cb_n]=id; g_cb_cnt[g_cb_n]=1; g_cb_n++; }
}
static void cb_report(const char* when){
    char b[512]; int o=0; b[0]=0;
    for(int i=0;i<g_cb_n && o<(int)sizeof(b)-24;i++)
        o+=snprintf(b+o,sizeof(b)-o,"%u:%u ",g_cb_ids[i],g_cb_cnt[i]);
    printf("GPH CB-HISTO[%s] total=%llu distinct=%d [%s]\n",
           when,(unsigned long long)g_cb_total,g_cb_n,b); fflush(stdout);
}
static void pump_frame(HSteamPipe pipe){
    if(g_runframe){ typedef void(*F)(void); __try{ ((F)g_runframe)(); }__except(EXCEPTION_EXECUTE_HANDLER){} }
    if(g_bgetcb){
        typedef bool(*G)(HSteamPipe,void*); typedef void(*Fr)(HSteamPipe);
        struct CB{ int32_t user; int32_t iCallback; void* param; int32_t cub; };
        for(int q=0;q<128;q++){ CB c; memset(&c,0,sizeof(c)); bool got=false;
            __try{ got=((G)g_bgetcb)(pipe,&c); }__except(EXCEPTION_EXECUTE_HANDLER){ break; }
            if(!got) break;
            cb_note((uint32_t)c.iCallback);
            if(g_freecb){ __try{ ((Fr)g_freecb)(pipe); }__except(EXCEPTION_EXECUTE_HANDLER){} }
        }
    }
}

// ---- Download spike (B0): generic unified-message request/response + a shallow
// protobuf response dumper, so we can reverse the exact wire layout from real
// on-device/VM data instead of guessing field numbers. RVA-independent (rides the
// same SendMethod path proven for Authentication.*).
static bool um_call(void* ium, HSteamPipe pipe, const char* method,
                    const void* req, uint32_t reqlen,
                    unsigned char* resp, uint32_t cap, uint32_t* outlen, int* outres){
    *outlen=0; *outres=-1;
    uint64_t h=um_send(ium,method,req,reqlen);
    printf("GPH DL um_call %s -> hCall=0x%llx reqlen=%u\n",method,(unsigned long long)h,reqlen); fflush(stdout);
    if(!h) return false;
    for(int w=0;w<200;w++){
        uint32_t sz=0; int res=-1;
        if(um_info(ium,h,&sz,&res)){
            *outres=res;
            if(sz==0){ um_data(ium,h,nullptr,0,true); *outlen=0; return true; }
            uint32_t rn = sz>cap ? cap : sz;
            if(sz>cap) printf("GPH DL um_call %s response %u > cap %u (truncated read may fail)\n",method,sz,cap);
            bool ok = um_data(ium,h,resp,rn,true);
            *outlen = ok? rn : 0;
            return true;
        }
        pump_frame(pipe); Sleep(50);
    }
    printf("GPH DL um_call %s TIMEOUT\n",method); fflush(stdout);
    return false;
}
// Walk a protobuf message, printing top-level fields; recurse one level into
// length-delimited fields that don't look like printable strings.
static void pb_dump(const unsigned char* b, uint32_t len, int depth){
    uint32_t off=0;
    auto ind=[&]{ for(int i=0;i<depth;i++) printf("  "); };
    while(off<len){
        uint64_t key=pb_getvar(b,len,&off); if(off>len) break;
        int field=(int)(key>>3), wt=(int)(key&7);
        if(wt==0){ uint64_t v=pb_getvar(b,len,&off); ind(); printf("f%d varint=%llu\n",field,(unsigned long long)v); }
        else if(wt==5){ if(off+4>len)break; uint32_t v; memcpy(&v,b+off,4); off+=4; ind(); printf("f%d fixed32=%u\n",field,v); }
        else if(wt==1){ if(off+8>len)break; uint64_t v; memcpy(&v,b+off,8); off+=8; ind(); printf("f%d fixed64=%llu\n",field,(unsigned long long)v); }
        else if(wt==2){ uint64_t L=pb_getvar(b,len,&off); if(off+L>len){ ind(); printf("f%d len=%llu OVERRUN\n",field,(unsigned long long)L); break; }
            const unsigned char* p=b+off; off+=(uint32_t)L;
            bool printable=(L>0&&L<256); for(uint32_t i=0;i<L&&printable;i++){ unsigned char c=p[i]; if(c<0x20||c>0x7e) printable=false; }
            ind();
            if(printable) printf("f%d str[%llu]=\"%.*s\"\n",field,(unsigned long long)L,(int)L,p);
            else if(depth<3 && L>=2){ printf("f%d msg[%llu]:\n",field,(unsigned long long)L); pb_dump(p,(uint32_t)L,depth+1); }
            else printf("f%d bytes[%llu]\n",field,(unsigned long long)L);
        }
        else { ind(); printf("f%d wiretype=%d unhandled\n",field,wt); break; }
    }
}

// ---- watchdogged engine accessor (some accessors block; run on a throwaway thread) ---
typedef void* (*Fn_GetIface)(void* self,int32_t hUser,int32_t hPipe,const char* ver);
static void* g_acc_self; static Fn_GetIface g_acc_fn; static int32_t g_acc_u,g_acc_p; static volatile void* g_acc_res;
static DWORD WINAPI acc_thread(LPVOID){ void* r=nullptr; __try{ r=g_acc_fn(g_acc_self,g_acc_u,g_acc_p,""); }__except(EXCEPTION_EXECUTE_HANDLER){ r=(void*)(intptr_t)-2; } g_acc_res=r; return 0; }
static void* call_accessor(void* self,void* fn,int32_t u,int32_t p,DWORD ms){
    g_acc_self=self; g_acc_fn=(Fn_GetIface)fn; g_acc_u=u; g_acc_p=p; g_acc_res=(void*)(intptr_t)-3;
    HANDLE th=CreateThread(nullptr,0,acc_thread,nullptr,0,nullptr); if(!th) return (void*)(intptr_t)-4;
    if(WaitForSingleObject(th,ms)==WAIT_TIMEOUT){ return (void*)(intptr_t)-1; }
    void* r=(void*)g_acc_res; CloseHandle(th); return r;
}

// ================= games-played send (PATH B, 10.87) ========================
// Heap-scan for the account's connected CCMInterface (its +0x5f4 session != 0), preferring the one
// whose (addr-0x48) is a real CUser vtable — the persona CM (the §26.1 wrong-object fix, VM-RVA'd).
// Find the account's persona CCMInterface: the ONE whose (addr-0x48) carries the real 10.87
// CUser vtable. Heap matches on the CCM vtable value alone yield stack/heap false-positives
// (garbage cuserVtRva), so we accept a candidate ONLY when (A-0x48)[0]==cuserVT — on a healthy
// client there is exactly one such object. We return it regardless of its session value (a
// dropped game-session shows [+0x5f4]==0; send_gp_via then gate-fails harmlessly but is ready
// the instant the session comes back), and log its session so we can watch durability.
static uint32_t g_gp_pid;     // f9 process_id to advertise (0 = omit); set to a live pid at startup
static void* g_manual_cu=nullptr; static uintptr_t g_manual_arr=0;   // GH_MANUAL: CUser + games-list array
static void* g_persona_ccm;   // remembered once found (its address is stable for the session)
static void* find_connected_ccm(uintptr_t B, bool logAll){
    const uintptr_t ccmVT = B + rva::CCMInterfaceVtbl;
    const uintptr_t cuserVT = B + rva::CUserVtbl;
    const uintptr_t REGION_CAP = 512ull*1024*1024;
    void* persona=nullptr; uint32_t personaSess=0; int found=0,personas=0,regions=0;
    MEMORY_BASIC_INFORMATION mbi;
    static unsigned char chunk[0x10000];
    uintptr_t addr=0x10000;
    while(addr<0x00007fffffffffffULL){
        if(VirtualQuery((void*)addr,&mbi,sizeof(mbi))!=sizeof(mbi)) break;
        uintptr_t rbase=(uintptr_t)mbi.BaseAddress, next=rbase+mbi.RegionSize;
        if(next<=addr) break;
        if(mbi.State==MEM_COMMIT && !(mbi.Protect&(PAGE_GUARD|PAGE_NOACCESS)) &&
           ((mbi.Protect&0xFF)==PAGE_READWRITE) && mbi.RegionSize<=REGION_CAP){
            ++regions;
            for(uintptr_t p=rbase;p<next;p+=sizeof(chunk)){
                size_t n=(next-p)<sizeof(chunk)?(size_t)(next-p):sizeof(chunk);
                if(!probe_read((void*)p,chunk,n)) continue;
                for(size_t o=0;o+8<=n;o+=8){
                    if(*(uintptr_t*)(chunk+o)!=ccmVT) continue;
                    uintptr_t A=p+o; ++found;
                    uintptr_t cvt=probe_ptr((void*)(A-0x48));
                    if(cvt!=cuserVT) continue;                 // reject non-persona (false-positive) matches
                    ++personas; uint32_t s=probe_u32((void*)(A+0x5f4));
                    if(logAll) printf("GPH   persona CCM@%p sess=%u f168=%u\n",(void*)A,s,probe_u32((void*)(A+0x168)));
                    if(!persona || (s!=0 && s>personaSess)){ persona=(void*)A; personaSess=s; }
                }
            }
        }
        addr=next;
    }
    if(persona) g_persona_ccm=persona;
    if(logAll) printf("GPH CCM-scan done found=%d personas=%d regions=%d persona=%p sess=%u\n",
                      found,personas,regions,persona,personaSess);
    return persona ? persona : g_persona_ccm;   // remembered persona survives a scan that misses it
}

// Build + send ONE CMsgClientGamesPlayed through a specific CCMInterface (10.87 RVAs).
// If ccm==nullptr this is a SELF-TEST: build+serialize+log the wire, no send (no login needed) —
// used to confirm the on-wire encoding matches the real client's known-good `0A 04 08 80 85 78`.
//
// game_id ENCODING (2026-08-09 fix): the real client writes CGameID to GamePlayed **field 1 as a
// VARINT** — `[gp+0x78]`, has-bit `0x200` — yielding `0A 04 08 80 85 78`. The old PATH-B used field 2
// fixed64 (`[gp+0x80]`, `0x400`), which the server ignores. That mis-field is why games-played never
// surfaced despite a perfect session.
// Read the account name + public SteamID out of steamclient's own loginusers.vdf. Deliberately
// reads nothing else: the credential itself lives in steamclient's store and must stay there, so
// we take only the two non-secret fields needed to ask the engine to log on from that store.
static bool read_cached_account(char* nameOut, size_t cap, uint64_t* idOut){
    const char* paths[] = { "config\\loginusers.vdf",
                            "C:\\Program Files (x86)\\Steam\\config\\loginusers.vdf" };
    for(const char* p : paths){
        FILE* f=fopen(p,"rb"); if(!f) continue;
        static char buf[65536]; size_t n=fread(buf,1,sizeof(buf)-1,f); fclose(f); buf[n]=0;
        // Blocks look like: "<steamid64>" { "AccountName" "<account>" ... }
        char* q=buf; uint64_t id=0;
        while((q=strchr(q,'"'))){
            char* e=strchr(q+1,'"'); if(!e) break;
            size_t len=(size_t)(e-q-1);
            if(len==17){ bool digits=true; for(size_t i=1;i<=17;i++) if(q[i]<'0'||q[i]>'9'){ digits=false; break; }
                         if(digits){ char t[24]; memcpy(t,q+1,17); t[17]=0; id=_strtoui64(t,nullptr,10); } }
            if(id && len==11 && memcmp(q+1,"AccountName",11)==0){
                char* v=strchr(e+1,'"'); if(!v) break;
                char* ve=strchr(v+1,'"'); if(!ve) break;
                size_t vl=(size_t)(ve-v-1);
                if(vl && vl<cap){ memcpy(nameOut,v+1,vl); nameOut[vl]=0; *idOut=id; return true; }
            }
            q=e+1;
        }
    }
    return false;
}

// ClientHeartBeat (EMsg 0x2bf). An EMsg inventory over a 120s window showed retail sending 14
// message types including 0x2bf, while our session sent only ServiceMethodCallFromClient (0x97)
// — no heartbeat at all. Rather than RE a SendHeartBeat function, note that CProtoBufMsg's ctor
// takes the EMsg as an argument: construct with 0x2bf and leave every body field unset, and it
// serialises to an empty body, which is exactly what CMsgClientHeartBeat is on the wire
// (verified: 8 bytes, bf 02 00 80 00 00 00 00). The GamesPlayed type-vtable/descriptor are used
// only because buildBody needs a concrete type to allocate; with no has-bits set it adds nothing.
// Generalised: any EMsg whose body may be empty. Used for 0x2bf (heartbeat) and 0x2cc
// (ClientChangeStatus). For 0x2cc this is a deliberate first cut — retail's carries player_name,
// but the hypothesis under test is that the CM keys presence off *receiving a ChangeStatus at all*,
// not off its contents. If an empty one is ignored, RE the real descriptor and set player_name.
static bool send_empty_emsg(uintptr_t B, void* ccm, uint32_t emsg){
    typedef void (*CtorFn)(void*,uint32_t,uint32_t);
    typedef void*(*P1Fn)(void*);
    typedef int  (*SendFn)(void*,void*);
    typedef void (*DtorFn)(void*);
    auto ctor =(CtorFn)(B+rva::Ctor);
    auto build=(P1Fn) (B+rva::BuildBody);
    auto sendw=(SendFn)(B+rva::SendWrapper);
    auto dtor =(DtorFn)(B+rva::Dtor);
    alignas(16) unsigned char msg[0x100]; memset(msg,0,sizeof(msg));
    bool ok=false;
    __try{
        ctor(msg,emsg,0);
        *(void**)(msg+0x00)=(void*)(B+rva::TypeVtbl);
        *(void**)(msg+0x08)=(void*)(B+rva::Descriptor);
        build(msg);
        if(ccm) ok = sendw(ccm,msg)!=0;
        dtor(msg);
    }__except(EXCEPTION_EXECUTE_HANDLER){ printf("GPH EMSG EXC emsg=0x%x\n",emsg); }
    return ok;
}
static bool send_hb_via(uintptr_t B, void* ccm){ return send_empty_emsg(B,ccm,0x2bf); }

// Populated ClientChangeStatus (0x2cc) — RE'd body layout (10.41, builder @0x590984):
//   type-vtbl=0x124b8e0  descriptor=0x16794c0  (CProtoBufMsg<CMsgClientChangeStatus>)
//   body has-bits @+0x10; persona_state(f1)=dword @+0x20 hasbit 0x2; persona_state_flags(f6)=dword @+0x28 hasbit 0x40.
// Sends an ONLINE announce (the message retail's session issues that ours never does). dumpBody prints the
// serialized body once to confirm persona_state landed on the wire (expect `08 01` for persona_state=1).
static bool send_changestatus(uintptr_t B, void* ccm, uint32_t persona_state, bool set_flags, uint32_t flags, bool dumpBody){
    typedef void (*CtorFn)(void*,uint32_t,uint32_t);
    typedef void*(*P1Fn)(void*);
    typedef int  (*SendFn)(void*,void*);
    typedef void (*DtorFn)(void*);
    typedef int  (*BSFn)(void*);
    typedef int  (*SerFn)(void*,void*,int);
    auto ctor =(CtorFn)(B+rva::Ctor);
    auto build=(P1Fn) (B+rva::BuildBody);
    auto sendw=(SendFn)(B+rva::SendWrapper);
    auto dtor =(DtorFn)(B+rva::Dtor);
#ifdef GH_SC1041
    const uintptr_t CS_VT=0x124b8e0, CS_DESC=0x16794c0;
#else
    const uintptr_t CS_VT=rva::TypeVtbl, CS_DESC=rva::Descriptor;   // 10.87 ClientChangeStatus not derived; would need RE
#endif
    alignas(16) unsigned char msg[0x100]; memset(msg,0,sizeof(msg));
    bool ok=false;
    __try{
        ctor(msg,0x2cc,0);
        *(void**)(msg+0x00)=(void*)(B+CS_VT);
        *(void**)(msg+0x08)=(void*)(B+CS_DESC);
        build(msg);
        void* body=*(void**)(msg+0x30);
        if(body){
            *(uint32_t*)((uintptr_t)body+0x20)=persona_state;   // persona_state (f1)
            *(uint32_t*)((uintptr_t)body+0x10)|=0x2;
            if(set_flags){ *(uint32_t*)((uintptr_t)body+0x28)=flags; *(uint32_t*)((uintptr_t)body+0x10)|=0x40; }
        }
        if(dumpBody){
            void** tv=*(void***)msg;   // type-vtable
            int n=((BSFn)tv[3])(msg);  // ByteSize (slot 3) — serializes whole msg? no: slot3 on the type-vtbl is body ByteSize
            if(n>0 && n<200){ unsigned char buf[256]; int w=((SerFn)tv[4])(msg,buf,n);
                char hx[600]; int ho=0; for(int q=0;q<w && ho<560;q++) ho+=snprintf(hx+ho,sizeof(hx)-ho,"%02x",buf[q]);
                printf("GPH CS body wire[%d]=%s (persona_state=%u)\n",w,hx,persona_state); fflush(stdout); }
            else printf("GPH CS ByteSize=%d (unexpected)\n",n);
        }
        if(ccm) ok = sendw(ccm,msg)!=0;
        dtor(msg);
    }__except(EXCEPTION_EXECUTE_HANDLER){ printf("GPH CS EXC\n"); }
    return ok;
}

static bool send_gp_via(uintptr_t B, void* ccm, uint32_t appid, bool dumpHdr){
    typedef void (*CtorFn)(void*,uint32_t,uint32_t);
    typedef void*(*P1Fn)(void*);
    typedef int  (*SendFn)(void*,void*);
    typedef void (*DtorFn)(void*);
    typedef int  (*SizeFn)(void*);
    typedef int  (*SerFn)(void*,void*,uint32_t);
    auto ctor =(CtorFn)(B+rva::Ctor);
    auto build=(P1Fn) (B+rva::BuildBody);
    auto addgp=(P1Fn) (B+rva::AddGamesPlayed);
    auto sendw=(SendFn)(B+rva::SendWrapper);
    auto dtor =(DtorFn)(B+rva::Dtor);
    auto sizef=(SizeFn)(B+rva::ByteSize);
    auto serf =(SerFn) (B+rva::SerializeToArray);
    void* vtbl=(void*)(B+rva::TypeVtbl);
    void* desc=(void*)(B+rva::Descriptor);
    alignas(16) unsigned char msg[0x100]; memset(msg,0,sizeof(msg));
    bool ok=false;
    __try{
        uint32_t sess = ccm ? *(volatile uint32_t*)((uintptr_t)ccm+0x5f4) : 0;
        ctor(msg,0x1522,0);
        *(void**)(msg+0x00)=vtbl;
        *(void**)(msg+0x08)=desc;
        build(msg);
        void* body=*(void**)(msg+0x30);
        if(body){
            void* gp=addgp(body);
            // Field map measured empirically by gp_hasbit_sweep + gp_offset_sweep (both
            // login-free, see the OFFSWEEP/SWEEP output). has-bit indices are struct-member
            // order, NOT field number: strings occupy bits 0-7, numerics start at bit 9.
            //   f2 game_id (fixed64) -> [gp+0x80], has-bit 0x400   (bit 10)
            //   f9 process_id (u32)  -> [gp+0x94], has-bit 0x4000  (bit 14)
            // (The earlier fieldno+8 guess was wrong: 0x20000 is f12, and +0x90 is not f9.)
            // FULL field set, reconstructed to match a real launch (captured).
            // Cross-check: the has-bits below sum to 0x2247f603; the real launch's were
            // 0x2247f703 — identical except bit 8, the one field that needs a live string
            // object (a fresh message has none, so touching it faults).
            *(uint64_t*)((uintptr_t)gp+0x78)=0;                  // f1  steam_id_gs
            *(uint64_t*)((uintptr_t)gp+0x80)=(uint64_t)appid;    // f2  game_id (CGameID, fixed64)
            *(uint32_t*)((uintptr_t)gp+0x8c)=0;                  // f4
            *(uint32_t*)((uintptr_t)gp+0x94)=g_gp_pid;           // f9  process_id
            *(uint32_t*)((uintptr_t)gp+0x98)=0;                  // f10
            *(uint32_t*)((uintptr_t)gp+0x9c)=0;                  // f11
            *(uint32_t*)((uintptr_t)gp+0xa0)=0;                  // f12
            *(uint32_t*)((uintptr_t)gp+0xa4)=0;                  // f15
            *(uint32_t*)((uintptr_t)gp+0xb8)=404;                // f21 (real launch sends 404)
            *(uint32_t*)((uintptr_t)gp+0xc4)=0xffffffffu;        // f25 = -1
            *(uint32_t*)((uintptr_t)gp+0xd4)=0;                  // f31
            //  f6/f7 empty strings (bits 0/1) come from the default string object; f5 is a bool.
            *(uint32_t*)((uintptr_t)gp+0x10)|=0x2247f603u;
            // Outer CMsgClientGamesPlayed: f2 client_os_type=20 (Windows), f3=0, f4=0.
            *(uint32_t*)((uintptr_t)body+0x30)=20;
            *(uint32_t*)((uintptr_t)body+0x34)=0;
            *(uint32_t*)((uintptr_t)body+0x38)=0;
            *(uint32_t*)((uintptr_t)body+0x10)|=0x7;
            int sz=-1,n=-1; unsigned char wbuf[160];
            __try{ sz=sizef(msg); n=serf(msg,wbuf,sizeof(wbuf)); }__except(EXCEPTION_EXECUTE_HANDLER){ n=-2; }
            char hex[200]; int oo=0; int hn=(n>0?(n<60?n:60):0);
            for(int i=0;i<hn&&oo<(int)sizeof(hex)-4;i++) oo+=snprintf(hex+oo,sizeof(hex)-oo,"%02x ",wbuf[i]);
            hex[oo]=0;
            if(!ccm){ printf("GPH GP-SELFTEST wire[%d]= %s (want body ...0a 04 08 80 85 78)\n",n,hex); dtor(msg); return false; }
            int r=sendw(ccm,msg); ok=(r!=0);
            printf("GPH GAMES-PLAYED ccm=%p sess=%u sendw=%d wire[%d]= %s\n",ccm,sess,r,n,hex);
            if(dumpHdr && sess!=0){
                uintptr_t hdr=probe_ptr((void*)((uintptr_t)msg+0x28));
                uint64_t sid=0,r70=0; if(hdr){ probe_read((void*)(hdr+0x68),&sid,8); probe_read((void*)(hdr+0x70),&r70,8); }
                uint32_t c168=probe_u32((void*)((uintptr_t)ccm+0x168));
                printf("GPH HDR hdr=0x%llx sid@+0x68=%llu (acct=%llu?) r70@+0x70=0x%llx ccm168=0x%x\n",
                       (unsigned long long)hdr,(unsigned long long)sid,(unsigned long long)acct_sid(),
                       (unsigned long long)r70,c168);
            }
        }
        dtor(msg);
    }__except(EXCEPTION_EXECUTE_HANDLER){ printf("GPH GAMES-PLAYED ccm=%p SEH\n",ccm); return false; }
    return ok;
}

// ================= has-bit -> field-number sweep (login-free) ================
// protobuf-C++ assigns has-bit indices in *declaration* order, which is NOT field-number
// order here (bit 10 -> f2 game_id, bit 17 -> f12). So rather than guess, build one message
// per bit with a distinctive value parked at [gp+0x90] and read the emitted tag off the wire.
static void gp_hasbit_sweep(uintptr_t B, bool outer=false){
    typedef void (*CtorFn)(void*,uint32_t,uint32_t);
    typedef void*(*P1Fn)(void*);
    typedef void (*DtorFn)(void*);
    typedef int  (*SerFn)(void*,void*,uint32_t);
    auto ctor =(CtorFn)(B+rva::Ctor);
    auto build=(P1Fn) (B+rva::BuildBody);
    auto addgp=(P1Fn) (B+rva::AddGamesPlayed);
    auto dtor =(DtorFn)(B+rva::Dtor);
    auto serf =(SerFn) (B+rva::SerializeToArray);
    void* vtbl=(void*)(B+rva::TypeVtbl);
    void* desc=(void*)(B+rva::Descriptor);
    printf("GPH ---- has-bit sweep (marker 0x1234 at gp+0x90 -> look for `48 b4 24`) ----\n");
    for(int bit=0; bit<32; bit++){
        alignas(16) unsigned char msg[0x100]; memset(msg,0,sizeof(msg));
        __try{
            ctor(msg,0x1522,0);
            *(void**)(msg+0x00)=vtbl;
            *(void**)(msg+0x08)=desc;
            build(msg);
            void* body=*(void**)(msg+0x30);
            if(body){
                void* gp = outer ? body : addgp(body);
                *(uint32_t*)((uintptr_t)gp+0x94)=0x1234;
                *(uint32_t*)((uintptr_t)gp+0x10)=(1u<<bit);
                unsigned char wbuf[192]; int n=-1, sz=-1;
                // ByteSize() must run first — SerializeToArray uses the cached sizes.
                __try{ sz=((int(*)(void*))(B+rva::ByteSize))(msg); n=serf(msg,wbuf,sizeof(wbuf)); }
                __except(EXCEPTION_EXECUTE_HANDLER){ n=-2; }
                char hex[260]; int oo=0; int hn=(n>0?(n<80?n:80):0);
                for(int i=0;i<hn&&oo<(int)sizeof(hex)-4;i++) oo+=snprintf(hex+oo,sizeof(hex)-oo,"%02x",wbuf[i]);
                hex[oo]=0;
                printf("GPH SWEEP bit%02d mask=0x%08x sz=%d n=%d %s\n",bit,(1u<<bit),sz,n,hex);
            }
            dtor(msg);
        }__except(EXCEPTION_EXECUTE_HANDLER){ printf("GPH SWEEP bit%02d SEH\n",bit); }
        fflush(stdout);
    }
    printf("GPH ---- end sweep ----\n"); fflush(stdout);
}

// Given a has-bit mask for a *numeric* field, find which struct offset backs it: park a
// marker at one offset at a time and look for the marker on the wire.
static void gp_offset_sweep(uintptr_t B, uint32_t mask, const char* what, bool outer=false){
    typedef void (*CtorFn)(void*,uint32_t,uint32_t);
    typedef void*(*P1Fn)(void*);
    typedef void (*DtorFn)(void*);
    typedef int  (*SerFn)(void*,void*,uint32_t);
    auto ctor =(CtorFn)(B+rva::Ctor);
    auto build=(P1Fn) (B+rva::BuildBody);
    auto addgp=(P1Fn) (B+rva::AddGamesPlayed);
    auto dtor =(DtorFn)(B+rva::Dtor);
    auto serf =(SerFn) (B+rva::SerializeToArray);
    void* vtbl=(void*)(B+rva::TypeVtbl);
    void* desc=(void*)(B+rva::Descriptor);
    for(uint32_t off=0x18; off<=0x120; off+=4){
        alignas(16) unsigned char msg[0x100]; memset(msg,0,sizeof(msg));
        __try{
            ctor(msg,0x1522,0);
            *(void**)(msg+0x00)=vtbl;
            *(void**)(msg+0x08)=desc;
            build(msg);
            void* body=*(void**)(msg+0x30);
            if(body){
                void* gp = outer ? body : addgp(body);
                *(uint32_t*)((uintptr_t)gp+off)=0x1234;
                *(uint32_t*)((uintptr_t)gp+0x10)=mask;
                unsigned char wbuf[192]; int n=-1;
                __try{ ((int(*)(void*))(B+rva::ByteSize))(msg); n=serf(msg,wbuf,sizeof(wbuf)); }
                __except(EXCEPTION_EXECUTE_HANDLER){ n=-2; }
                char hex[200]; int oo=0; int hn=(n>0?(n<60?n:60):0);
                for(int i=0;i<hn&&oo<(int)sizeof(hex)-4;i++) oo+=snprintf(hex+oo,sizeof(hex)-oo,"%02x",wbuf[i]);
                hex[oo]=0;
                bool hit = (n>0) && strstr(hex,"b424")!=nullptr;
                if(hit) printf("GPH OFFSWEEP[%s] mask=0x%08x  ==> +0x%02x   n=%d %s\n",what,mask,off,n,hex);
            }
            dtor(msg);
        }__except(EXCEPTION_EXECUTE_HANDLER){ }
        fflush(stdout);
    }
}

// Map every field of the real launch's message that we still need, in one login-free run.
static void gp_map_fields(uintptr_t B){
    printf("GPH ==== field map: GamePlayed (inner) ====\n");
    struct { uint32_t mask; const char* name; } inner[] = {
        {0x00001000,"f4"},   {0x00002000,"f5"},   {0x00004000,"f9 process_id"},
        {0x00008000,"f10"},  {0x00010000,"f11"},  {0x00020000,"f12"},
        {0x00040000,"f15"},  {0x00400000,"f21"},  {0x02000000,"f25"},
        {0x20000000,"f31"},
    };
    for(auto& e : inner) gp_offset_sweep(B, e.mask, e.name, false);
    // Outer CMsgClientGamesPlayed has only 3 has-bits: bit0=f2 client_os_type, bit1=f3, bit2=f4.
    printf("GPH ==== field map: CMsgClientGamesPlayed (outer) ====\n");
    gp_offset_sweep(B, 0x1, "outer f2 client_os_type", true);
    gp_offset_sweep(B, 0x2, "outer f3", true);
    gp_offset_sweep(B, 0x4, "outer f4", true);
    fflush(stdout);
}

// ================= QR url output ============================================
static void write_qr_url(const char* url){
    printf("GPH CHALLENGE_URL: %s\n", url); fflush(stdout);
    HANDLE h=CreateFileA("C:\\gh\\gh_qr_url.txt", GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(h!=INVALID_HANDLE_VALUE){ DWORD wr; WriteFile(h,url,(DWORD)strlen(url),&wr,nullptr); CloseHandle(h); }
}

// ================= E1: refresh -> access token exchange =====================
// Authentication.GenerateAccessTokenForApp#1
//   Request : { 1 refresh_token(str, SECRET), 2 steamid(int64), 3 renewal_type(enum=0 None) }
//   Response: { 1 access_token(str, SECRET), 2 refresh_token(str, if rotated, SECRET) }
// Returns true and fills accessOut (SECRET, never printed) on success.
#ifdef GH_E1_TOKEN_EXCHANGE
static bool exchange_access_token(void* ium, const char* refresh, uint64_t steamid, char* accessOut, size_t cap){
    unsigned char req[2400]; int rn=0;
    rn+=pb_str(req+rn,1,refresh);
    rn+=pb_int(req+rn,2,steamid);
    rn+=pb_int(req+rn,3,0);   // k_EAuthTokenRenewalType_None
    uint64_t h=um_send(ium,"Authentication.GenerateAccessTokenForApp#1",req,rn);
    printf("GPH E1 GenerateAccessTokenForApp -> hCall=0x%llx\n",(unsigned long long)h);
    if(!h) return false;
    for(int w=0;w<100;w++){
        uint32_t sz=0; int res=-1;
        if(um_info(ium,h,&sz,&res)){
            if(res!=1){ um_data(ium,h,nullptr,0,true); printf("GPH E1 exchange EResult=%d\n",res); return false; }
            unsigned char* buf=(unsigned char*)malloc(sz?sz:1); bool got=false;
            if(buf && sz && um_data(ium,h,buf,sz,true)){
                const unsigned char* sp=nullptr; uint32_t sl=0;
                if(pb_find(buf,sz,1,&sp,&sl,nullptr) && sl<cap){ memcpy(accessOut,sp,sl); accessOut[sl]=0; got=true; }
            }
            free(buf);
            printf("GPH E1 access_token=%s\n", got?"RECEIVED (redacted)":"(absent)");
            return got;
        }
        Sleep(100);
    }
    printf("GPH E1 exchange timed out\n");
    return false;
}
#endif

// ================= Phase B repro: CUser::AddClientGame on the MINIMAL engine ==========
// Does calling AddClientGame on this standalone CreateLocalUser bring-up crash the way the Xbox
// shim does (caller-deadline job, null CJob+0x178)? If YES on this VM, we've reproduced the Xbox
// wall on a fast/inspectable box and can bisect the missing CSteamEngine bootstrap here.
// A VEH logs the exact steamclient fault RVA (frame-based SEH can't catch the job-dispatch fault).
// Gated by GH_ADDGAME. 10.87 games-list: count CUser+0x2548, array +0x2538.
static volatile long g_veh_on=0;
static LONG CALLBACK addgame_veh(EXCEPTION_POINTERS* ep){
    if(!g_veh_on) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code=ep->ExceptionRecord->ExceptionCode;
    if((code & 0xF0000000)!=0xC0000000) return EXCEPTION_CONTINUE_SEARCH;
    HMODULE h=GetModuleHandleA("steamclient64.dll");
    uintptr_t rva=h?((uintptr_t)ep->ExceptionRecord->ExceptionAddress-(uintptr_t)h):0;
    uintptr_t bad=(ep->ExceptionRecord->NumberParameters>=2)?ep->ExceptionRecord->ExceptionInformation[1]:0;
    printf("GPH ADDGAME-VEH code=0x%08lx sc_rva=0x%llx badaddr=0x%llx\n",
           code,(unsigned long long)rva,(unsigned long long)bad); fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;   // let default handling proceed; we only want the address
}
static bool test_addclientgame(uintptr_t B, void* ccm){
    if(!ccm){ printf("GPH ADDGAME: no ccm\n"); fflush(stdout); return false; }
    void* cu=(void*)((uintptr_t)ccm-0x48);
    uint64_t gameid=(uint64_t)kAppId;   // plain Steam app: appid in low bits, type 0
    char exePath[MAX_PATH]; exePath[0]=0; GetModuleFileNameA(nullptr,exePath,MAX_PATH);
    typedef char(*Fn)(void*,uint32_t,uint64_t*,uint64_t*,const char*,const char*,
                      uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
    Fn addGame=(Fn)(B+rva::AddClientGame);
    uint32_t before=probe_u32((void*)((uintptr_t)cu+rva::GamesListCount));
    uint64_t sid=0; probe_read((void*)((uintptr_t)cu+rva::GuardSteamID),&sid,8);
    printf("GPH ADDGAME pre cu=%p sid=%llu pid=%lu gameid=0x%llx count=%u fn=%p\n",
           cu,(unsigned long long)sid,GetCurrentProcessId(),(unsigned long long)gameid,before,(void*)addGame);
    fflush(stdout);
    PVOID veh=AddVectoredExceptionHandler(1,addgame_veh); InterlockedExchange(&g_veh_on,1);
    int ret=-1;
    __try{ ret=(int)(unsigned char)addGame(cu,GetCurrentProcessId(),&gameid,&gameid,
                                           "Lethal Company",exePath,0,0,404,0,0,0); }
    __except(EXCEPTION_EXECUTE_HANDLER){ ret=-2; printf("GPH ADDGAME SEH caught 0x%08lx\n",GetExceptionCode()); }
    InterlockedExchange(&g_veh_on,0); if(veh) RemoveVectoredExceptionHandler(veh);
    uint32_t after=probe_u32((void*)((uintptr_t)cu+rva::GamesListCount));
    printf("GPH ADDGAME post ret=%d count %u->%u\n",ret,before,after); fflush(stdout);
    return ret>0;
}

// ---- HW-breakpoint engine-thread marshal (validates the SHIM mechanism: Dr0 bp @0xcdbe20 + VEH) ----
// Mirrors gh_steam_engine.h's gh_register_arm_engine_marshal exactly, so a pass here means the shim's
// marshal mechanics (not just frida Interceptor) are sound. Uses the 10.41 RVAs.
static volatile void* g_hw_ccm=nullptr; static volatile DWORD g_hw_etid=0;
static volatile long g_hw_pending=0, g_hw_suppress=0;
static DWORD hw_engine_tid(uintptr_t B){ uintptr_t gse=0; if(!probe_read((void*)(B+0x1755548),&gse,8)||!gse) return 0; return probe_u32((void*)(gse+0x1a18)); }
static LONG CALLBACK hw_marshal_veh(EXCEPTION_POINTERS* ep){
    DWORD code=ep->ExceptionRecord->ExceptionCode;
    uintptr_t B=(uintptr_t)GetModuleHandleA("steamclient64.dll"); if(!B) return EXCEPTION_CONTINUE_SEARCH;
    if(code==EXCEPTION_SINGLE_STEP && (uintptr_t)ep->ExceptionRecord->ExceptionAddress==B+0xcdbe20){
        ep->ContextRecord->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
        ep->ContextRecord->Dr0=0; ep->ContextRecord->Dr7 &= ~(DWORD64)1; ep->ContextRecord->Dr6=0;
        ep->ContextRecord->EFlags |= 0x10000;
        if(g_hw_pending && GetCurrentThreadId()==g_hw_etid && InterlockedCompareExchange(&g_hw_pending,0,1)==1){
            printf("GPH HWMARSHAL bp hit engine tid=%lu -> AddClientGame\n",GetCurrentThreadId()); fflush(stdout);
            __try{ test_addclientgame(B,(void*)g_hw_ccm); }__except(EXCEPTION_EXECUTE_HANDLER){ printf("GPH HWMARSHAL do_call SEH\n"); fflush(stdout); }
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if(code==EXCEPTION_ACCESS_VIOLATION && (uintptr_t)ep->ExceptionRecord->ExceptionAddress==B+0xcce5a2){
        CONTEXT* c=ep->ContextRecord; uint64_t rbx=0,ret=0;
        if(probe_read((void*)(uintptr_t)(c->Rsp+0x20),&rbx,8) && probe_read((void*)(uintptr_t)(c->Rsp+0x28),&ret,8) && ret){
            c->Rbx=rbx; c->Rip=ret; c->Rsp=c->Rsp+0x30; c->Rax=1;
            long n=InterlockedIncrement(&g_hw_suppress);
            if(n<=3||(n%200)==0){ printf("GPH HWMARSHAL doomed suppressed #%ld\n",n); fflush(stdout); }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void hw_marshal_arm(uintptr_t B, void* ccm){
    DWORD etid=hw_engine_tid(B); printf("GPH HWMARSHAL arm: etid=%lu ccm=%p\n",etid,ccm); fflush(stdout);
    if(!etid||!ccm) return;
    g_hw_ccm=ccm; g_hw_etid=etid; AddVectoredExceptionHandler(1,hw_marshal_veh); InterlockedExchange(&g_hw_pending,1);
    HANDLE hT=OpenThread(THREAD_GET_CONTEXT|THREAD_SET_CONTEXT|THREAD_SUSPEND_RESUME|THREAD_QUERY_INFORMATION,FALSE,etid);
    if(!hT){ printf("GPH HWMARSHAL OpenThread failed err=%lu\n",GetLastError()); fflush(stdout); InterlockedExchange(&g_hw_pending,0); return; }
    SuspendThread(hT);
    CONTEXT ctx; memset(&ctx,0,sizeof(ctx)); ctx.ContextFlags=CONTEXT_DEBUG_REGISTERS;
    BOOL got=GetThreadContext(hT,&ctx), set=FALSE;
    if(got){ ctx.Dr0=(DWORD64)(B+0xcdbe20); ctx.Dr7=(ctx.Dr7 & ~((DWORD64)0xF<<16))|(DWORD64)1; ctx.Dr6=0; ctx.ContextFlags=CONTEXT_DEBUG_REGISTERS; set=SetThreadContext(hT,&ctx); }
    ResumeThread(hT); CloseHandle(hT);
    printf("GPH HWMARSHAL arm get=%d set=%d\n",(int)got,(int)set); fflush(stdout);
}

// Immediate, unbuffered result log independent of stdout redirection (which buffers
// when the process is launched detached). Appends to C:\gh\dl_log.txt via WriteFile.
static void dl_log(const char* s){
    HANDLE h=CreateFileA("C:\\gh\\dl_log.txt",FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,
                         nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h!=INVALID_HANDLE_VALUE){ DWORD wr; WriteFile(h,s,(DWORD)strlen(s),&wr,nullptr);
        const char* nl="\r\n"; WriteFile(h,nl,2,&wr,nullptr); CloseHandle(h); }
}

// ================= main =====================================================
int main(void){
    setvbuf(stdout,nullptr,_IONBF,0);   // unbuffered so redirected output is live
    CreateDirectoryA("C:\\gh",nullptr);
    printf("GPH start pid=%lu\n",(unsigned long)GetCurrentProcessId());
    dl_log("GPH start");
    // GH_MANUAL: the "manual games-list + state-sync" route (avoids the crashing AddClientGame
    // coroutine job). Set the pipe's app context to LC via env BEFORE bring-up so AdvertiseGame
    // targets 1966720, then (post-login) advertise + watch the server oracle for a broadcast.
    if(getenv("GH_MANUAL")){
        SetEnvironmentVariableA("SteamAppId","1966720");
        SetEnvironmentVariableA("SteamGameId","1966720");
        printf("GPH MANUAL mode: SteamAppId/SteamGameId=1966720\n"); fflush(stdout);
    }
#ifdef GH_E1_TOKEN_EXCHANGE
    printf("GPH *** VARIANT E1: refresh->access token exchange before LogOn ***\n");
#else
    printf("GPH *** VARIANT E0: current refresh-token login (reproduce the wall) ***\n");
#endif
    fflush(stdout);

    SetEnvironmentVariableA("SteamAppId",  "1966720");   // Lethal Company
    SetEnvironmentVariableA("SteamGameId", "1966720");

    SetLastError(0);
    HMODULE h=LoadLibraryA("steamclient64.dll");
    DWORD e=GetLastError();
    printf("GPH LoadLibraryA(steamclient64.dll) -> %p err=%lu\n",(void*)h,(unsigned long)e); fflush(stdout);
    if(!h){ printf("GPH abort: steamclient64.dll failed to map (run from the Steam install dir)\n"); return 1; }
    const uintptr_t B=(uintptr_t)h;

    CreateInterfaceFn ci=(CreateInterfaceFn)GetProcAddress(h,"CreateInterface");
    if(!ci){ printf("GPH abort: no CreateInterface\n"); return 1; }
    void* engine=nullptr; int rc=0;
    __try{ engine=ci("CLIENTENGINE_INTERFACE_VERSION005",&rc); }__except(EXCEPTION_EXECUTE_HANDLER){ engine=nullptr; }
    printf("GPH CreateInterface(IClientEngine) -> %p rc=%d\n",engine,rc); fflush(stdout);
    if(!engine){ printf("GPH abort: no IClientEngine\n"); return 1; }

    // CreateLocalUser (vt[4]) -> (pipe,user)
    int32_t eng_pipe=0, eng_user=0;
    { typedef int32_t(*Fn)(void*,int32_t*,int32_t); void** vt=(void**)*(uintptr_t*)engine;
      __try{ eng_user=((Fn)vt[4])(engine,&eng_pipe,1); }__except(EXCEPTION_EXECUTE_HANDLER){ printf("GPH CreateLocalUser EXC\n"); } }
    printf("GPH CreateLocalUser -> user=%d pipe=%d\n",eng_user,eng_pipe); fflush(stdout);
    if(!eng_pipe || !eng_user){ printf("GPH abort: no local user/pipe\n"); return 1; }

    // GetIClientUser (vt[8]) -> flat IClientUser
    void* clientUser=nullptr;
    { typedef void*(*Fn)(void*,int32_t,int32_t,const char*); void** vt=(void**)*(uintptr_t*)engine;
      __try{ clientUser=((Fn)vt[8])(engine,eng_user,eng_pipe,"CLIENTUSER_INTERFACE_VERSION"); }__except(EXCEPTION_EXECUTE_HANDLER){ printf("GPH GetIClientUser EXC\n"); } }
    printf("GPH GetIClientUser -> %p\n",clientUser); fflush(stdout);
    if(!clientUser){ printf("GPH abort: no IClientUser\n"); return 1; }
    void** cuvt=*(void***)clientUser;
    printf("GPH IClientUser vtblRVA=0x%llx\n",(unsigned long long)((uintptr_t)cuvt-B));

    // ---- Version guard: confirm the send/route RVAs are valid for this DLL ----
    // The type-vtable and descriptor addresses must land inside the module image.
    GH_MODULEINFO mi; memset(&mi,0,sizeof(mi));
    { typedef BOOL(WINAPI*GMI)(HANDLE,HMODULE,GH_LPMODULEINFO,DWORD);
      HMODULE psapi=LoadLibraryA("psapi.dll"); GMI gmi=psapi?(GMI)GetProcAddress(psapi,"GetModuleInformation"):nullptr;
      if(gmi) gmi(GetCurrentProcess(),h,&mi,sizeof(mi)); }
    printf("GPH module base=%p size=0x%lx  CCMvt=0x%llx CUservt=0x%llx TypeVtbl=0x%llx\n",
           (void*)B,(unsigned long)mi.SizeOfImage,
           (unsigned long long)(B+rva::CCMInterfaceVtbl),(unsigned long long)(B+rva::CUserVtbl),
           (unsigned long long)(B+rva::TypeVtbl));
    if(mi.SizeOfImage && (rva::TypeVtbl>=mi.SizeOfImage || rva::CCMInterfaceVtbl>=mi.SizeOfImage)){
        printf("GPH *** VERSION GUARD: RVAs fall outside the module image — this is NOT 10.87.82.64. ABORT (re-derive RVAs). ***\n");
        return 2;
    }
    // Cheap validity check: the type-vtable's ByteSize/Serialize slots should point into the image.
    { uintptr_t tvt=B+rva::TypeVtbl; uintptr_t slot3=probe_ptr((void*)(tvt+0x18)), slot4=probe_ptr((void*)(tvt+0x20));
      printf("GPH VERSION GUARD type-vtbl[3]=0x%llx (want 0x%llx) [4]=0x%llx (want 0x%llx)\n",
             (unsigned long long)slot3,(unsigned long long)(B+rva::ByteSize),
             (unsigned long long)slot4,(unsigned long long)(B+rva::SerializeToArray));
      if(slot3 != B+rva::ByteSize || slot4 != B+rva::SerializeToArray)
          printf("GPH *** VERSION GUARD WARNING: type-vtbl slots differ from expected — RVAs may be stale. Proceeding, verify wire bytes. ***\n");
    }
    // Wire self-test (no login/CM needed): build+serialize a games-played and log the bytes to
    // confirm the encoding matches the real client's known-good body `...0a 04 08 80 85 78`.
    g_gp_pid = GetCurrentProcessId();     // advertise ourselves as the running "game" process
    printf("GPH process_id to advertise = %u\n", g_gp_pid);
#ifndef GH_SC1041
    send_gp_via(B, nullptr, kAppId, false);   // 10.87-only send-path RVAs; skipped on the 10.41 repro build
#endif
    fflush(stdout);
    if(getenv("GH_GP_SWEEP")) gp_map_fields(B);   // diagnostic-only; see the field map above

    // GetIClientUnifiedMessages (engine slot 52)
    void* ium=nullptr;
    { void** vt=(void**)*(uintptr_t*)engine; ium=call_accessor(engine,vt[52],eng_user,eng_pipe,3000);
      intptr_t iv=(intptr_t)ium; if(iv<0x10000){ printf("GPH GetIClientUnifiedMessages -> %p (nil/hang)\n",ium); ium=nullptr; } }
    printf("GPH IClientUnifiedMessages -> %p\n",ium); fflush(stdout);
    if(!ium){ printf("GPH abort: no IClientUnifiedMessages\n"); return 1; }

    // GH_PRELOGIN_WAIT (Phase-1): hold here (steamclient loaded, engine+user made, NO CM traffic yet) so an
    // external frida can attach + install its BAsyncSend/RecvPkt hooks before the first CM message goes out,
    // capturing the FULL login/friends-subscription outbound stream we've never probed.
    if(getenv("GH_PRELOGIN_WAIT")){
        printf("GPH PRELOGIN WAITING FOR GO (create C:\\gh41\\go.txt)\n"); fflush(stdout);
        for(int gw=0; gw<240; ++gw){ if(GetFileAttributesA("C:\\gh41\\go.txt")!=INVALID_FILE_ATTRIBUTES) break; Sleep(500); }
        printf("GPH PRELOGIN GO\n"); fflush(stdout);
    }

    // ---- Bring the CM up: EConnect, then anon LogOn for a session ----
    printf("GPH pre: LogonState=%d BConnected=%d BLoggedOn=%d\n",
           call_i(clientUser,cuvt[5],"GetLogonState"),call_i(clientUser,cuvt[6],"BConnected"),
           call_i(clientUser,cuvt[4],"BLoggedOn")); fflush(stdout);
    call_i(clientUser,cuvt[8],"EConnect");
    for(int w=0;w<150;w++){ if(call_i(clientUser,cuvt[6],"BConnected")==1) break; Sleep(100); }
    printf("GPH BConnected=%d -> anon LogOn\n",call_i(clientUser,cuvt[6],"BConnected"));
    iu_logon(clientUser,kAnonSid);
    for(int w=0;w<100;w++){ if(call_i(clientUser,cuvt[4],"BLoggedOn")==1) break; Sleep(100); }
    printf("GPH anon BLoggedOn=%d (session ready for authed SendMethod)\n",call_i(clientUser,cuvt[4],"BLoggedOn"));
    fflush(stdout);

    // ---- QR upgrade: BeginAuthSessionViaQR + poll (following rotation) ----
    unsigned char qreq[160]; int qn=0;
    qn+=pb_str(qreq+qn,1,"Greenhouse Harness");
    qn+=pb_int(qreq+qn,2,1);                  // platform_type = SteamClient
    { unsigned char dd[96]; int dn=0; dn+=pb_str(dd+dn,1,"Greenhouse Harness"); dn+=pb_int(dd+dn,2,1);
      qn+=pb_tag(qreq+qn,3,2); qn+=pb_varint(qreq+qn,dn); memcpy(qreq+qn,dd,dn); qn+=dn; }
    qn+=pb_str(qreq+qn,4,"Client");

    static char tok[2048]; static char acct[128]; uint64_t realID=0; bool loggedReal=false;

    // ---- Try cached-credential login first, so a run costs no QR scan ----
    // Every experiment so far has cost the owner a manual phone scan, which is what kept us to
    // roughly one variable per round-trip. steamclient keeps its own credential store; after a
    // successful QR login it records the account under config/loginusers.vdf with
    // RememberPassword=1. SetAccountNameForCachedCredentialLogin (IClientUser slot 50 — the same
    // call the QR path already makes before SetLoginToken) asks the engine to log on from that
    // store, so the token never passes through us: we read only the account name and the public
    // SteamID out of the vdf, and the secret stays inside steamclient. Falls through to QR if
    // this fails, so a stale or absent store just costs one extra connect attempt.
    if(!getenv("GH_FORCE_QR")){
        char cname[128]={0}; uint64_t cid=0;
        if(read_cached_account(cname,sizeof(cname),&cid) && cname[0] && cid){
            printf("GPH cached-credential login: account=\"%s\" steamid=%llu\n",cname,(unsigned long long)cid); fflush(stdout);
            iu_logoff(clientUser);
            for(int w=0;w<40;w++){ if(call_i(clientUser,cuvt[5],"GetLogonState")==0) break; Sleep(100); }
            call_i(clientUser,cuvt[8],"EConnect");
            for(int w=0;w<100;w++){ if(call_i(clientUser,cuvt[6],"BConnected")==1) break; Sleep(100); }
            iu_set_acct(clientUser,cname,true);
            iu_logon(clientUser,cid);
            uint64_t got=0;
            for(int w=0;w<150;w++){ if(call_i(clientUser,cuvt[4],"BLoggedOn")==1){ got=iu_getsteamid(clientUser); break; } Sleep(100); }
            if(got){ printf("GPH *** LOGGED IN from cached credentials: SteamID=%llu (no scan needed) ***\n",(unsigned long long)got);
                     realID=got; loggedReal=true; }
            else   { printf("GPH cached-credential login failed -> falling back to QR\n");
                     // Put the session back where the QR path expects it: connected + anon.
                     iu_logoff(clientUser);
                     for(int w=0;w<40;w++){ if(call_i(clientUser,cuvt[5],"GetLogonState")==0) break; Sleep(100); }
                     call_i(clientUser,cuvt[8],"EConnect");
                     for(int w=0;w<100;w++){ if(call_i(clientUser,cuvt[6],"BConnected")==1) break; Sleep(100); }
                     iu_logon(clientUser,kAnonSid);
                     for(int w=0;w<100;w++){ if(call_i(clientUser,cuvt[4],"BLoggedOn")==1) break; Sleep(100); } }
            fflush(stdout);
        } else printf("GPH no cached account in loginusers.vdf -> QR login\n");
    }

    uint64_t qr_client_id=0; unsigned char qr_reqid[64]; uint32_t qr_reqid_len=0;
    enum { BEGIN, WAIT_BEGIN, POLL } st=BEGIN;
    uint64_t hBegin=0, hPoll=0; bool pollInFlight=false;
    int qrIssuedAt=0, nextPollAt=0, lastStatus=-1, lastPollErr=-999, expStreak=0;
    const int kMaxExpStreak=5;
    const int steps=6000;   // ~600s scan window

    for(int s=0; s<steps && !loggedReal; ++s){
        if(st==BEGIN){
            hBegin=um_send(ium,"Authentication.BeginAuthSessionViaQR#1",qreq,qn);
            printf("GPH BeginAuthSessionViaQR -> hCall=0x%llx\n",(unsigned long long)hBegin); fflush(stdout);
            if(!hBegin){ Sleep(500); continue; }
            st=WAIT_BEGIN;
        } else if(st==WAIT_BEGIN){
            uint32_t sz=0; int res=-1;
            if(um_info(ium,hBegin,&sz,&res)){
                if(res==1 && sz>0){
                    unsigned char* buf=(unsigned char*)malloc(sz);
                    if(buf && um_data(ium,hBegin,buf,sz,true)){
                        const unsigned char* sp=nullptr; uint32_t sl=0; uint64_t v=0;
                        if(pb_find(buf,sz,1,nullptr,nullptr,&v)) qr_client_id=v;
                        if(pb_find(buf,sz,3,&sp,&sl,nullptr)&&sl<=sizeof(qr_reqid)){ memcpy(qr_reqid,sp,sl); qr_reqid_len=sl; }
                        if(pb_find(buf,sz,2,&sp,&sl,nullptr)){ char url[512]; uint32_t ul=sl<sizeof(url)-1?sl:sizeof(url)-1; memcpy(url,sp,ul); url[ul]=0; write_qr_url(url); }
                    }
                    free(buf);
                    st=qr_client_id?POLL:BEGIN; qrIssuedAt=s; lastStatus=-1; lastPollErr=-999; expStreak=0; nextPollAt=s+10;
                } else if(res!=1){ um_data(ium,hBegin,nullptr,0,true); printf("GPH begin EResult=%d retry\n",res); st=BEGIN; Sleep(500); }
            }
        } else if(st==POLL){
            if(expStreak>=kMaxExpStreak){ printf("GPH challenge dead -> refresh QR\n"); pollInFlight=false; st=BEGIN; Sleep(100); continue; }
            if(!pollInFlight && s>=nextPollAt){
                unsigned char preq[96]; int pn=0; pn+=pb_int(preq+pn,1,qr_client_id);
                if(qr_reqid_len){ pn+=pb_tag(preq+pn,2,2); pn+=pb_varint(preq+pn,qr_reqid_len); memcpy(preq+pn,qr_reqid,qr_reqid_len); pn+=qr_reqid_len; }
                hPoll=um_send(ium,"Authentication.PollAuthSessionStatus#1",preq,pn);
                pollInFlight=(hPoll!=0); nextPollAt=s+50;
            }
            if(pollInFlight){
                uint32_t sz=0; int res=-1;
                if(um_info(ium,hPoll,&sz,&res)){
                    pollInFlight=false;
                    if(res!=1){ um_data(ium,hPoll,nullptr,0,true); expStreak++;
                        if(res!=lastPollErr){ printf("GPH poll EResult=%d (keeping QR; expStreak=%d/%d)\n",res,expStreak,kMaxExpStreak); lastPollErr=res; }
                        Sleep(100); continue; }
                    expStreak=0; lastPollErr=-999;
                    unsigned char* buf=(unsigned char*)malloc(sz?sz:1); bool hasTok=false; uint64_t hadRI=0;
                    if(buf && sz && um_data(ium,hPoll,buf,sz,true)){
                        const unsigned char* sp=nullptr; uint32_t sl=0;
                        pb_find(buf,sz,5,nullptr,nullptr,&hadRI);
                        hasTok=pb_find(buf,sz,3,&sp,&sl,nullptr);
                        if(hasTok&&sl<sizeof(tok)){ memcpy(tok,sp,sl); tok[sl]=0; realID=jwt_sub_steamid(tok); }
                        const unsigned char* ap=nullptr; uint32_t al=0;
                        if(pb_find(buf,sz,6,&ap,&al,nullptr)&&al<sizeof(acct)){ memcpy(acct,ap,al); acct[al]=0; }
                        uint64_t newCid=0;
                        if(pb_find(buf,sz,1,nullptr,nullptr,&newCid)&&newCid&&newCid!=qr_client_id){ printf("GPH challenge rotated %llu->%llu\n",(unsigned long long)qr_client_id,(unsigned long long)newCid); qr_client_id=newCid; }
                        const unsigned char* np=nullptr; uint32_t nl=0;
                        if(pb_find(buf,sz,2,&np,&nl,nullptr)&&nl>1&&nl<500){ char nurl[512]; memcpy(nurl,np,nl); nurl[nl]=0; write_qr_url(nurl); qrIssuedAt=s; }
                    }
                    free(buf);
                    int status=(hasTok?2:(hadRI?1:0));
                    if(status!=lastStatus){ printf("GPH poll had_remote_interaction=%llu account=\"%s\" token=%s\n",(unsigned long long)hadRI,acct[0]?acct:"",hasTok?"RECEIVED":"(pending)"); lastStatus=status; fflush(stdout); }
                    if(hasTok){
                        printf("GPH *** approved: account=\"%s\" steamid=%llu — completing login ***\n",acct,(unsigned long long)realID); fflush(stdout);
                        // The token we LogOn with (E0: refresh; E1: exchanged access token).
                        const char* logonTok = tok;
#ifdef GH_E1_TOKEN_EXCHANGE
                        static char access[4096];
                        if(exchange_access_token(ium,tok,realID,access,sizeof(access))) logonTok=access;
                        else printf("GPH E1 exchange failed -> falling back to refresh token\n");
#endif
                        // Login-completion (Xbox-proven order): logoff -> EConnect -> SetAccount -> SetToken -> LogOn(real).
                        iu_logoff(clientUser);
                        for(int w=0;w<40;w++){ if(call_i(clientUser,cuvt[5],"GetLogonState")==0) break; Sleep(100); }
                        call_i(clientUser,cuvt[8],"EConnect");
                        for(int w=0;w<100;w++){ if(call_i(clientUser,cuvt[6],"BConnected")==1) break; Sleep(100); }
                        iu_set_acct(clientUser,acct,true);
                        iu_set_token(clientUser,logonTok,acct);
                        iu_logon(clientUser,realID);
                        uint64_t got=0;
                        for(int w=0;w<150;w++){ if(call_i(clientUser,cuvt[4],"BLoggedOn")==1){ got=iu_getsteamid(clientUser); break; } Sleep(100); }
                        memset(tok,0,sizeof(tok));
#ifdef GH_E1_TOKEN_EXCHANGE
                        memset(access,0,sizeof(access));
#endif
                        if(got){ printf("GPH *** LOGGED IN as real user: SteamID=%llu ***\n",(unsigned long long)got); loggedReal=true; }
                        else { printf("GPH login-completion did not reach BLoggedOn\n"); }
                    }
                }
            }
        }
        Sleep(100);
    }

    if(!loggedReal){ printf("GPH login not completed; exiting\n"); return 3; }

    // Resolve the frame-pump exports and run the client for ~3s so its subsystems settle
    // (server go-online announce, heartbeats) — a real client pumps these continuously.
    g_runframe=(void*)GetProcAddress(h,"Steam_RunFrame");
    g_bgetcb  =(void*)GetProcAddress(h,"Steam_BGetCallback");
    g_freecb  =(void*)GetProcAddress(h,"Steam_FreeLastCallback");
    // Steam_RunFrame is NOT an export of 10.87 (41 exports, checked against the PE export table) —
    // NULL here is expected, not a fault. steamclient runs its own work on tier0 job threads; the
    // BGetCallback pump is only the consumer-side dequeue.
    printf("GPH pump exports RunFrame=%p (expected null on 10.87) BGetCallback=%p FreeLastCallback=%p\n",
           g_runframe,g_bgetcb,g_freecb);
    for(int w=0;w<30;w++){ pump_frame(eng_pipe); Sleep(100); }

    // ---- Persona-state diagnostic: is OUR account Online or Offline? ----
    // A game won't surface to anyone if our persona is Offline. Read it via the public
    // ISteamFriends017 (ISteamClient slot 8 = GetISteamFriends; friends slot 0 = GetPersonaName,
    // slot 2 = GetPersonaState). Known slots (shim-proven) — no RE needed.
    void* isteamclient=nullptr; { int r2=0; __try{ isteamclient=ci("SteamClient021",&r2); }__except(EXCEPTION_EXECUTE_HANDLER){} }
    void* friends=nullptr;
    if(isteamclient){ typedef void*(*F)(void*,int32_t,int32_t,const char*); void** cvt=*(void***)isteamclient;
        __try{ friends=((F)cvt[8])(isteamclient,eng_user,eng_pipe,"SteamFriends017"); }__except(EXCEPTION_EXECUTE_HANDLER){} }
    auto fr_state=[&](void* f)->int{ if(!f) return -1; typedef int(*F)(void*); void** vt=*(void***)f; int v=-2; __try{ v=((F)vt[2])(f); }__except(EXCEPTION_EXECUTE_HANDLER){} return v; };
    auto fr_name =[&](void* f)->const char*{ if(!f) return "?"; typedef const char*(*F)(void*); void** vt=*(void***)f; const char* s=nullptr; __try{ s=((F)vt[0])(f); }__except(EXCEPTION_EXECUTE_HANDLER){} return s?s:"?"; };
    printf("GPH FRIENDS iface=%p personaName=\"%s\" personaState=%d (0=Offline 1=Online 2=Busy 3=Away 4=Snooze)\n",
           friends, fr_name(friends), fr_state(friends)); fflush(stdout);

    // ---- Server-truth oracle: what does the CM think WE are playing? ----
    // Until now the only way to answer "did it work?" was to ask the owner to look at their
    // Steam client, which capped us at roughly one experiment per human round-trip. But the
    // client's friends-state for a SteamID is populated from the CM's ClientPersonaState
    // broadcast, so reading our OWN entry back reflects what the server published, not what we
    // asked for. ISteamFriends slot 8 = GetFriendGamePlayed(CSteamID byval, FriendGameInfo_t*).
    // FriendGameInfo_t = { CGameID gameID; uint32 ip; uint16 gamePort; uint16 queryPort;
    //                      CSteamID lobby } = 24 bytes.
    //
    // CALIBRATION IS MANDATORY BEFORE TRUSTING THIS. We already know the profile is NOT showing
    // a game right now, so a first read of 0 means the oracle tracks the server; a read of
    // 1966720 means it is only echoing our own send locally and is worthless as an oracle.
    struct FriendGameInfo { uint64_t gameID; uint32_t ip; uint16_t gamePort; uint16_t queryPort; uint64_t lobby; };
    auto fr_gameplayed=[&](void* f, uint64_t id, FriendGameInfo* out)->bool{
        if(!f) return false; typedef bool(*F)(void*,uint64_t,FriendGameInfo*);
        void** vt=*(void***)f; bool r=false; memset(out,0,sizeof(*out));
        __try{ r=((F)vt[8])(f,id,out); }__except(EXCEPTION_EXECUTE_HANDLER){ r=false; }
        return r;
    };
    // POSITIVE CONTROL for the oracle. Reading 0 for ourselves is ambiguous: it could mean "the
    // server says we aren't playing" (oracle works, answer is no) or "this call never populates
    // anything" (oracle is dead and would read 0 forever, including on success). Distinguish the
    // two by asking the same call about our FRIENDS: their game state can only have come from the
    // CM's ClientPersonaState broadcast, so a single friend reading back non-zero proves the
    // mechanism is live. Prints counts and raw gameIDs only — no names, no friend list dumped.
    // GetFriendByIndex returns CSteamID by value, which MSVC passes via a hidden return pointer as
    // the SECOND argument (CSteamID has user-declared ctors, so it is not returned in RAX).
    // `online` is the intermediate signal that keeps a zero result interpretable: if friends are
    // reading back as online then the persona pipeline is delivering server state and only the
    // game field is empty (nobody is playing); if online is 0 too, we simply haven't received
    // friend persona data yet and the control says nothing either way.
    auto oracle_control=[&](const char* when){
        typedef int(*CntFn)(void*,int);
        typedef uint64_t*(*IdxFn)(void*,uint64_t*,int,int);
        typedef int(*StFn)(void*,uint64_t);
        void** fvt = friends ? *(void***)friends : nullptr;
        int n=0; __try{ if(fvt) n=((CntFn)fvt[3])(friends,0x04); }__except(EXCEPTION_EXECUTE_HANDLER){ n=-1; }
        int inGame=0, online=0, zeroIds=0; char ids[160]; int io=0; ids[0]=0;
        // ABI probe on the first few entries. 66 PersonaStateChange callbacks arrive on our pipe, so
        // the engine HAS this data; reading zeros for all 64 friends points at how we call
        // GetFriendByIndex, not at an inert subsystem. The hidden-retptr convention returns the
        // retptr itself in RAX, so comparing the return against &fid identifies the ABI outright:
        // ret==&fid means retptr honoured, ret looking like a SteamID64 means it returns plainly.
        char abi[256]; int ao=0; abi[0]=0;
        for(int i=0;i<n && i<400;i++){
            uint64_t fid=0; uint64_t* r=nullptr;
            __try{ r=((IdxFn)fvt[4])(friends,&fid,i,0x04); }__except(EXCEPTION_EXECUTE_HANDLER){ break; }
            if(i<4 && ao<200){
                // Cross-check the slot region: GetFriendPersonaName(7) and GetFriendRelationship(5).
                // If the name is real, slots 5-8 are mapped correctly and a 0 state is genuine data
                // rather than another misread; if it is empty, the mapping past slot 4 is suspect.
                typedef const char*(*NmFn)(void*,uint64_t); typedef int(*RelFn)(void*,uint64_t);
                const char* nm="<exc>"; int rel=-1;
                __try{ nm=((NmFn)fvt[7])(friends,fid); }__except(EXCEPTION_EXECUTE_HANDLER){ nm="<exc>"; }
                __try{ rel=((RelFn)fvt[5])(friends,fid); }__except(EXCEPTION_EXECUTE_HANDLER){ rel=-1; }
                char safe[40]; safe[0]=0;
                char nb=0;
                if(nm && probe_read((void*)nm,&nb,1)){ strncpy(safe,nm,sizeof(safe)-1); safe[sizeof(safe)-1]=0; }
                else snprintf(safe,sizeof(safe),"<unreadable>");
                ao+=snprintf(abi+ao,sizeof(abi)-ao,"[%d fid=%llu name=\"%s\" rel=%d st=%d] ",
                             i,(unsigned long long)fid,safe,rel,
                             (int)((StFn)fvt[6])(friends,fid));
            }
            if(!fid){ zeroIds++; continue; }
            int st=0; __try{ st=((StFn)fvt[6])(friends,fid); }__except(EXCEPTION_EXECUTE_HANDLER){ st=0; }
            if(st>0) online++;
            FriendGameInfo gi; if(!fr_gameplayed(friends,fid,&gi)) continue;
            if(gi.gameID){ inGame++; if(io<120) io+=snprintf(ids+io,sizeof(ids)-io,"%llu ",(unsigned long long)gi.gameID); }
        }
        printf("GPH ORACLE-POSITIVE-CONTROL[%s] friends=%d online=%d inGame=%d zeroIds=%d gameIDs=[%s]\n",
               when,n,online,inGame,zeroIds,ids);
        printf("GPH ORACLE-ABI[%s] %s\n",when,abi); fflush(stdout);
        return inGame;
    };
    oracle_control("startup");

    // ---- Locate IClientFriends by version-string sweep (GH_FIND_FRIENDS=1) ----
    // The friends subsystem looks inert in this process (online=0 for all friends, no 0x2cc),
    // and the theory is that a bare host with no steamwebhelper never brings it up. IClientFriends
    // is the gateway, but its engine accessor slot is unknown and blind-calling slots means wild
    // calls. Self-identifying sweep instead: a real accessor returns an interface for the version
    // string it serves and NULL for one it doesn't, so require both to call it a match.
    if(getenv("GH_FIND_FRIENDS")){
        typedef void*(*AccFn)(void*,int32_t,int32_t,const char*);
        void** evt=(void**)*(uintptr_t*)engine;
        for(int i=5;i<40;i++){
            void* good=nullptr; void* bogus=(void*)1;
            __try{ good=((AccFn)evt[i])(engine,eng_user,eng_pipe,"CLIENTFRIENDS_INTERFACE_VERSION"); }
            __except(EXCEPTION_EXECUTE_HANDLER){ continue; }
            if(!good) continue;
            __try{ bogus=((AccFn)evt[i])(engine,eng_user,eng_pipe,"GREENHOUSE_NOT_A_REAL_VERSION"); }
            __except(EXCEPTION_EXECUTE_HANDLER){ bogus=(void*)1; }
            printf("GPH FRIENDS-SWEEP slot=%d -> %p (bogus-version -> %p; a match must be non-null then NULL)%s\n",
                   i,good,bogus, (bogus==nullptr)?"  <== MATCH":""); fflush(stdout);
        }
    }

    // ---- Complete the friends/persona subsystem (GH_FRIENDS_COMPLETE) --------------------------
    // A full client, after logon, obtains the internal IClientFriends and (a) publishes its persona
    // (SetPersonaState) and (b) subscribes to the persona graph (RequestFriendData). Our bare bring-up
    // does neither, so the session is a read-only friends consumer that never registers as an active
    // presence PUBLISHER — the leading remaining hypothesis for why games-played is received but never
    // broadcast to friends (every wire-level difference has been ruled out). Bind IClientFriends
    // (VERIFY-then-call — never sweep-and-call: blind slot calls asserted the client once), dump its
    // vtable for offline slot ID, then SetPersonaState(Online). Friend-state arrival in the oracle is
    // the autonomous signal that the registration took.
    if(getenv("GH_FRIENDS_COMPLETE")){
        typedef void*(*AccFn)(void*,int32_t,int32_t,const char*);
        void** evt=(void**)*(uintptr_t*)engine;
        const char* selfName=fr_name(friends);   // ground truth (public ISteamFriends017 slot 0)
        // Dump the ALREADY-BOUND public ISteamFriends017 vtable (safe — no engine accessor). Lets us
        // identify RequestUserInformation (the friend-data / persona-graph subscription trigger) offline.
        if(friends){
            void** pfv=*(void***)friends; char pv[1100]; int po=0;
            for(int k=0;k<50 && po<(int)sizeof(pv)-20;k++){
                uintptr_t fn=0; __try{ fn=(uintptr_t)pfv[k]; }__except(EXCEPTION_EXECUTE_HANDLER){ fn=0; }
                po+=snprintf(pv+po,sizeof(pv)-po,"%d:%llx ",k,(unsigned long long)(fn?fn-B:0));
            }
            pv[po]=0; printf("GPH ISTEAMFRIENDS017-VTABLE(rva) %s\n",pv); fflush(stdout);
        }
        (void)evt;   // engine GetIClientFriends accessor can't be probed safely (it asserts uncatchably).
        // Drive the persona-graph subscription through the PUBLIC ISteamFriends017 (already bound, safe):
        // public slot 37 = RequestUserInformation (VERIFIED via wrapper disasm: pub34/35/36 -> int15/16/17 =
        // the 3 avatar getters, then pub37 -> int32 = RequestUserInformation — canonical ISteamFriends017
        // order). bRequireNameOnly=false requests full persona data per friend -> sends ClientRequestFriendData
        // (0x32f), which our headless session otherwise NEVER sends. Signal A = friend state then arrives
        // (online>0), which also revives GetFriendGamePlayed(self) as a real server-truth oracle.
        if(friends && !getenv("GH_NO_REQINFO")){
            void** fvt=*(void***)friends;
            typedef int      (*CntFn)(void*,int);
            typedef uint64_t*(*IdxFn)(void*,uint64_t*,int,int);
            typedef bool     (*RUIFn)(void*,uint64_t,bool);
            const int RUI_SLOT=37;
            int n=0; __try{ n=((CntFn)fvt[3])(friends,0x04); }__except(EXCEPTION_EXECUTE_HANDLER){ n=0; }
            int req=0;
            for(int i=0;i<n && i<400;i++){
                uint64_t fid=0; __try{ ((IdxFn)fvt[4])(friends,&fid,i,0x04); }__except(EXCEPTION_EXECUTE_HANDLER){ break; }
                if(!fid) continue;
                __try{ ((RUIFn)fvt[RUI_SLOT])(friends,fid,false); req++; }__except(EXCEPTION_EXECUTE_HANDLER){}
            }
            printf("GPH FRIENDS-COMPLETE: RequestUserInformation(slot %d,nameOnly=false) x%d over %d friends\n",RUI_SLOT,req,n); fflush(stdout);
            for(int w=0;w<60;w++){ pump_frame(eng_pipe); Sleep(100); }   // ~6s for ClientPersonaState to flow back
            oracle_control("after-requestinfo");   // Signal A: online>0 for friends?
            // Self-oracle re-check — if friend state now flows, GetFriendGamePlayed(self) reflects server truth.
            FriendGameInfo sg; uint64_t me=iu_getsteamid(clientUser); bool sok=fr_gameplayed(friends,me,&sg);
            printf("GPH SELF-ORACLE after RequestUserInformation: ok=%d serverGameID=%llu\n",sok,(unsigned long long)sg.gameID); fflush(stdout);
        }
        // ---- Reach the INTERNAL IClientFriends via the public wrapper's own load site ----
        // Public ISteamFriends017 wrappers are `mov rcx,[rcx+8]; mov rax,[rcx]; jmp [rax+slot*8]` — the
        // internal IClientFriends pointer is stored at public_object+8. Extract it directly (no engine
        // accessor, no assert), verify via GetPersonaName(int slot0)==account, dump its vtable (to ID
        // RequestFriendData), then publish persona via SetPersonaState(int slot5,Online) — stable early
        // slot, gated on int slot4=GetPersonaState==1.
        if(friends && !getenv("GH_NO_ICF")){
            void* icf=nullptr;
            if(!probe_read((void*)((uintptr_t)friends+8),&icf,sizeof(icf))) icf=nullptr;
            const char* inm=nullptr;
            if(icf){ __try{ typedef const char*(*GN)(void*); void** iv=*(void***)icf; inm=((GN)iv[0])(icf); }__except(EXCEPTION_EXECUTE_HANDLER){ inm=nullptr; } }
            char nmb[24]; int nb=0; if(inm){ for(int q=0;q<12;q++){ char c=0; if(!probe_read((void*)((uintptr_t)inm+q),&c,1)) break; nb+=snprintf(nmb+nb,sizeof(nmb)-nb,"%02x",(unsigned char)c);} } nmb[nb]=0;
            printf("GPH INTERNAL-ICF=%p (public+8) inm=%p bytes=%s self=\"%s\"\n",icf,(void*)inm,nmb,selfName?selfName:"?"); fflush(stdout);
            // Dump the internal vtable UNCONDITIONALLY (safe reads) + verify via GetPersonaState(slot4), not slot0.
            void* icfvt=nullptr;
            if(icf && (uintptr_t)icf>0x10000 && probe_read(icf,&icfvt,8) && icfvt){
                char vt[1100]; int vo=0;
                for(int k=0;k<50 && vo<(int)sizeof(vt)-20;k++){ uintptr_t fn=0; if(!probe_read((void*)((uintptr_t)icfvt+8*k),&fn,8)) fn=0; vo+=snprintf(vt+vo,sizeof(vt)-vo,"%d:%llx ",k,(unsigned long long)(fn && fn>B ? fn-B:0)); }
                vt[vo]=0; printf("GPH INTERNAL-ICF-VTABLE(rva) %s\n",vt); fflush(stdout);
                void** iv=(void**)icfvt;
                int ps=-99; __try{ typedef int(*GP)(void*); ps=((GP)iv[4])(icf); }__except(EXCEPTION_EXECUTE_HANDLER){ ps=-98; }
                printf("GPH INTERNAL-ICF slot4 GetPersonaState=%d (expect 1=Online)\n",ps); fflush(stdout);
                if(!getenv("GH_NO_SETSTATE") && ps>=0 && ps<=7){
                    typedef void(*SP)(void*,int);
                    if(getenv("GH_FORCE_CHANGESTATUS")){
                        // Phase-2 route (b): a plain SetPersonaState(Online) is a NO-OP when already Online
                        // (engine dedupes -> emits no ClientChangeStatus). Force a REAL state change (Away=3,
                        // transient/safe) then back to Online(1) so the engine FLUSHES a 0x2cc each time. If our
                        // persona then publishes, the server should start including friend online status ->
                        // oracle_control's online-count flips from 0 to >0.
                        __try{ ((SP)iv[5])(icf,3); printf("GPH FORCE-CS SetPersonaState(Away=3)\n"); }__except(EXCEPTION_EXECUTE_HANDLER){ printf("GPH FORCE-CS Away EXC\n"); }
                        fflush(stdout);
                        for(int w=0;w<30;w++){ pump_frame(eng_pipe); Sleep(100); }
                        oracle_control("after-setstate-away");
                        __try{ ((SP)iv[5])(icf,1); printf("GPH FORCE-CS SetPersonaState(Online=1)\n"); }__except(EXCEPTION_EXECUTE_HANDLER){ printf("GPH FORCE-CS Online EXC\n"); }
                        fflush(stdout);
                        for(int w=0;w<60;w++){ pump_frame(eng_pipe); Sleep(100); }
                        int ps2=-99; __try{ ps2=((int(*)(void*))iv[4])(icf); }__except(EXCEPTION_EXECUTE_HANDLER){ ps2=-98; }
                        printf("GPH FORCE-CS post GetPersonaState=%d\n",ps2); fflush(stdout);
                    } else {
                        __try{ ((SP)iv[5])(icf,1); printf("GPH INTERNAL-ICF SetPersonaState(Online) called (int slot 5)\n"); }
                        __except(EXCEPTION_EXECUTE_HANDLER){ printf("GPH INTERNAL-ICF SetPersonaState EXC\n"); }
                    }
                } else printf("GPH INTERNAL-ICF SetPersonaState SKIPPED (slot4=%d)\n",ps);
                for(int w=0;w<40;w++){ pump_frame(eng_pipe); Sleep(100); }
                oracle_control("after-setpersonastate");
            } else printf("GPH INTERNAL-ICF vtable unreadable (icfvt=%p)\n",icfvt);
        }
    }

    // ---- Persona publication: get the session into the friends/persona subsystem ----
    // The control above reads online=0 for all 64 friends, i.e. our session receives no persona
    // state for anyone, while retail's EMsg inventory includes 0x2cc = ClientChangeStatus — the
    // message that publishes persona status and opts the session into the persona service. We
    // never send it. Since games-played surfaces to friends *through* the persona system, an
    // inert persona subsystem is a better candidate for the wall than anything content-level.
    //
    // Rather than hand-build CMsgClientChangeStatus (we only have GamesPlayed's ctor/vtbl/
    // descriptor), let the engine build it: ISteamFriends slot 1 = SetPersonaName. We pass back
    // the name the account ALREADY has, read from slot 0 — a semantic no-op for the owner, no
    // visible account change, but it should still drive a ClientChangeStatus onto the wire.
    if(!getenv("GH_NO_PERSONA_PUBLISH")){
        const char* cur=fr_name(friends);
        if(friends && cur && cur[0] && strcmp(cur,"?")!=0){
            char keep[128]; snprintf(keep,sizeof(keep),"%s",cur);   // slot 0 returns engine-owned memory
            typedef void(*SetNameFn)(void*,const char*);
            void** fvt=*(void***)friends;
            __try{ ((SetNameFn)fvt[1])(friends,keep); printf("GPH persona-publish: SetPersonaName(\"%s\") (unchanged value)\n",keep); }
            __except(EXCEPTION_EXECUTE_HANDLER){ printf("GPH persona-publish EXC\n"); }
            for(int w=0;w<30;w++){ pump_frame(eng_pipe); Sleep(100); }
            oracle_control("after-persona-publish");
        }
    }

    const uint64_t selfID = iu_getsteamid(clientUser);   // ask the engine, no hardcoded id
    { FriendGameInfo gi; bool got=fr_gameplayed(friends,selfID,&gi);
      printf("GPH ORACLE-CALIBRATION got=%d gameID=%llu (expect 0 while the profile shows no game; "
             "if this reads %u the oracle is a local echo, not server truth)\n",
             got?1:0,(unsigned long long)gi.gameID,kAppId); fflush(stdout); }

    // ---- Games-played send loop: re-assert every ~5s so a client heartbeat can't blank it ----
    // ---- GH_MANUAL: advertise LC to build a valid games-list entry (no AddClientGame crash),
    // optionally hand-attach our pid, then rely on the client's own state-sync to broadcast. ----
    if(getenv("GH_MANUAL")){
        void* ccm0=find_connected_ccm(B,false);
        void* cu0 = ccm0 ? (void*)((uintptr_t)ccm0-0x48) : nullptr;
        uint32_t nr0 = call_i(clientUser,cuvt[131],"NumGamesRunning");
        // AdvertiseGame (IClientUser slot 119 on 10.41): (self, const CSteamID* srv, uint32 ip, uint16 port, uint16 qport)
        uint64_t zeroSid=0;
        typedef void(*AdvFn)(void*,const void*,uint32_t,uint16_t,uint16_t);
        __try{ ((AdvFn)cuvt[119])(clientUser,&zeroSid,0,0,0); }__except(EXCEPTION_EXECUTE_HANDLER){ printf("GPH MANUAL AdvertiseGame EXC\n"); }
        uint32_t nr1 = call_i(clientUser,cuvt[131],"NumGamesRunning");
        int anyrun = call_i(clientUser,cuvt[92],"BIsAnyGameRunning");
        uintptr_t arr = cu0 ? probe_ptr((void*)((uintptr_t)cu0+(rva::GamesListCount-0x10))) : 0;
        uint32_t cnt = cu0 ? probe_u32((void*)((uintptr_t)cu0+rva::GamesListCount)) : 0;
        uint64_t cg0 = arr ? *(uint64_t*)(arr+8) : 0;   // entry-0 CGameID
        printf("GPH MANUAL: NumGamesRunning %u->%u BIsAnyGameRunning=%d cu=%p arr=0x%llx count=%u entry0.cgid=0x%llx\n",
               nr0,nr1,anyrun,cu0,(unsigned long long)arr,cnt,(unsigned long long)cg0); fflush(stdout);
        // AdvertiseGame gave us an allocated, valid entry but cgid=0 (app context wasn't LC). Hand-fix
        // the CGameID to LC (+8 and +0x10) and seed the entry's appid-array (+0x210 -> [1966720],
        // +0x220 = 1) so the entry reads as "playing LC". Then the loop watches the server oracle.
        if(arr && cnt>=1 && !getenv("GH_ADDGAME")){   // when GH_ADDGAME follows, let AddClientGame populate the entry (no fragile hand-write)
            static uint32_t s_appid = kAppId;
            __try{
                *(uint64_t*)(arr+0x08) = (uint64_t)kAppId;   // CGameID (type-0 app)
                *(uint64_t*)(arr+0x10) = (uint64_t)kAppId;   // owner CGameID
                *(uintptr_t*)(arr+0x210) = (uintptr_t)&s_appid;  // appid-array ptr
                *(uint32_t*)(arr+0x220) = 1;                     // appid-array count
                printf("GPH MANUAL: hand-set entry cgid=0x%llx appid-array=[%u]\n",
                       (unsigned long long)*(uint64_t*)(arr+8), s_appid);
            }__except(EXCEPTION_EXECUTE_HANDLER){ printf("GPH MANUAL: entry write EXC\n"); }
            fflush(stdout);
        }
        g_manual_cu = cu0; g_manual_arr = arr;   // for the oracle loop to keep re-asserting/attaching
    }

    // GH_APPS_SWEEP: RTTI-name every safe engine accessor slot to locate IClientApps (the licenses/apps
    // subsystem) so we can test the license hypothesis. Safe slots from the shim's kGhEngineAccessorSlots.
    if(getenv("GH_APPS_SWEEP")){
        static const int slots[]={7,8,9,10,13,15,16,17,18,21,22,23,24,25,27,34,35,43,44,46,47,50,51,52,54,55,57,58,59,60,62,63,64,65,67,69,70,71,72,74,75,76,79};
        void** evt=(void**)*(uintptr_t*)engine;
        for(unsigned si=0;si<sizeof(slots)/sizeof(int);si++){
            void* iface=call_accessor(engine,evt[slots[si]],eng_user,eng_pipe,2500);
            intptr_t iv=(intptr_t)iface;
            const char* nm=(iv>0x10000)?gh_rtti(iface):"";
            if(iv>0x10000 && nm[0]) printf("GPH APPS-SWEEP slot %d -> %p  %s\n",slots[si],iface,nm);
            else printf("GPH APPS-SWEEP slot %d -> %p (nil/hang/no-rtti)\n",slots[si],iface);
            fflush(stdout);
        }
        printf("GPH APPS-SWEEP done\n"); fflush(stdout);
    }
    // GH_APPS_ACQUIRE: bring up the apps/licenses subsystem (IClientApps slot 17, IClientAppManager slot 43)
    // so the license hypothesis gets a full test — the interface proxies exist and any lazy init fires.
    if(getenv("GH_APPS_ACQUIRE")){
        void** evt=(void**)*(uintptr_t*)engine;
        void* apps=call_accessor(engine,evt[17],eng_user,eng_pipe,3000);
        void* appmgr=call_accessor(engine,evt[43],eng_user,eng_pipe,3000);
        printf("GPH APPS-ACQUIRE IClientApps(17)=%p %s  IClientAppManager(43)=%p %s\n",
               apps,((intptr_t)apps>0x10000)?gh_rtti(apps):"", appmgr,((intptr_t)appmgr>0x10000)?gh_rtti(appmgr):"");
        fflush(stdout);
        for(int w=0;w<40;w++){ pump_frame(eng_pipe); Sleep(100); }   // let any app/license init settle
    }
    // GH_DL_SPIKE (Phase B0): a persistent RPC DRIVER, so ONE login (one QR scan) serves many
    // experiments. After login it polls C:\gh\dl_params.txt and executes one command per change,
    // dumping BOTH a decoded protobuf tree and RAW HEX (so exact wire layouts can be reversed
    // host-side without re-logging-in). This validates route B's core thesis: can our minimal
    // engine drive Steam's content ServiceMethods over IClientUnifiedMessages?
    //   Commands (first token in dl_params.txt):
    //     servers [cell]                       ContentServerDirectory.GetServersForSteamPipe#1
    //     mcode <app> <depot> <manifest> [br]  ContentServerDirectory.GetManifestRequestCode#1
    //     raw <ServiceMethod#n> <hexproto>     send ANY method with an arbitrary proto body
    if(getenv("GH_DL_SPIKE")){
        static unsigned char resp[262144]; unsigned char req[4096];
        // Acquire IClientAppManagerMap (engine slot 43) for the download-manager experiment (Route A test).
        void* appmgr=nullptr;
        { void** evt=(void**)*(uintptr_t*)engine; appmgr=call_accessor(engine,evt[43],eng_user,eng_pipe,3000);
          if((intptr_t)appmgr<=0x10000) appmgr=nullptr; }
        printf("GPH DL appmgr(43)=%p %s\n",appmgr, appmgr?gh_rtti(appmgr):""); fflush(stdout);
        // SEH-guarded call of an IClientAppManagerMap vtable slot as uint32 fn(this, uint32 appid).
        // Safe for read-only (this,appid)->int methods; used to anchor the vtable by matching a known
        // return (GetAppBuildID(1966720)==22825947). Do NOT use on InstallApp (wrong arity -> crash).
        auto amgr_call=[&](int slot, uint32_t appid)->uint32_t{
            if(!appmgr) return 0xFFFFFFFEu;
            void** vt=*(void***)appmgr; typedef uint32_t(*F)(void*,uint32_t); uint32_t r=0xFFFFFFFFu;
            __try{ r=((F)vt[slot])(appmgr,appid); }__except(EXCEPTION_EXECUTE_HANDLER){ r=0xDEAD0000u|(uint32_t)slot; }
            return r;
        };
        printf("GPH DL === driver up (ium=%p appmgr=%p). commands via C:\\gh\\dl_params.txt ===\n",ium,appmgr); fflush(stdout);
        dl_log("DL driver up (logged in, ready for commands)");
        CreateDirectoryA("C:\\gh",nullptr);
        auto hexdump=[&](const unsigned char* b, uint32_t n){
            printf("GPH DL HEX[%u]: ",n);
            for(uint32_t i=0;i<n && i<4096;i++) printf("%02x",b[i]);
            printf("\n"); fflush(stdout);
            // also to the immediate log, as a single hex string (cap 8KB)
            static char hx[16400]; uint32_t m=n>8000?8000:n; int o=0;
            o+=snprintf(hx+o,sizeof(hx)-o,"HEX[%u]: ",n);
            for(uint32_t i=0;i<m;i++) o+=snprintf(hx+o,sizeof(hx)-o,"%02x",b[i]);
            dl_log(hx);
        };
        auto do_call=[&](const char* method, const unsigned char* rq, int rn){
            uint32_t rlen=0; int res=-1;
            bool ok=um_call(ium,eng_pipe,method,rq,(uint32_t)rn,resp,sizeof(resp),&rlen,&res);
            char line[256]; snprintf(line,sizeof(line),"CALL %s ok=%d EResult=%d len=%u",method,ok,res,rlen);
            printf("GPH DL %s\n",line); dl_log(line); fflush(stdout);
            if(ok && rlen){ hexdump(resp,rlen); printf("GPH DL --- tree ---\n"); pb_dump(resp,rlen,0); fflush(stdout); }
            return ok;
        };
        auto hexbyte=[&](char c)->int{ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; };

        char last[1024]={0};
        for(int iter=0; iter<1000000; ++iter){
            // read the command file
            char cur[1024]={0};
            HANDLE fh=CreateFileA("C:\\gh\\dl_params.txt",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
            if(fh!=INVALID_HANDLE_VALUE){ DWORD rd=0; ReadFile(fh,cur,sizeof(cur)-1,&rd,nullptr); CloseHandle(fh);
                while(rd>0 && (cur[rd-1]=='\r'||cur[rd-1]=='\n'||cur[rd-1]==' ')) cur[--rd]=0; }
            if(cur[0] && strcmp(cur,last)!=0){
                strcpy(last,cur);
                printf("GPH DL >>> cmd: %s\n",cur); fflush(stdout);
                char verb[32]={0}, a1[128]={0}, a2[128]={0}, a3[128]={0}, a4[128]={0};
                int nf=sscanf(cur,"%31s %127s %127s %127s %127s",verb,a1,a2,a3,a4);
                if(!strcmp(verb,"servers")){
                    int rn=0; if(nf>=2) rn+=pb_int(req+rn,1,strtoul(a1,0,10)); rn+=pb_int(req+rn,2,20);
                    do_call("ContentServerDirectory.GetServersForSteamPipe#1",req,rn);
                } else if(!strcmp(verb,"mcode") && nf>=4){
                    int rn=0; rn+=pb_int(req+rn,1,strtoul(a1,0,10)); rn+=pb_int(req+rn,2,strtoul(a2,0,10));
                    rn+=pb_int(req+rn,3,strtoull(a3,0,10)); rn+=pb_str(req+rn,4, nf>=5?a4:"public");
                    if(do_call("ContentServerDirectory.GetManifestRequestCode#1",req,rn)){
                        uint64_t code=0; if(pb_find(resp,0,1,nullptr,nullptr,&code)){} }
                } else if(!strcmp(verb,"raw") && nf>=3){
                    // a2 = hex proto body
                    int rn=0; for(char* p=a2; p[0]&&p[1]; p+=2){ int hi=hexbyte(p[0]),lo=hexbyte(p[1]); if(hi<0||lo<0)break; req[rn++]=(unsigned char)((hi<<4)|lo); }
                    do_call(a1,req,rn);
                } else if(!strcmp(verb,"amgrvt") && nf>=2){
                    // dump live IClientAppManagerMap vtable RVAs (rel to steamclient base B)
                    int n=(int)strtoul(a1,0,10);
                    if(appmgr){ void** vt=*(void***)appmgr;
                        for(int s=0;s<n;s++){ uintptr_t f=(uintptr_t)vt[s]; char l[64];
                            snprintf(l,sizeof(l),"amgrvt[%d] rva=0x%llx",s,(unsigned long long)(f-B)); printf("GPH DL %s\n",l); dl_log(l);} }
                    else dl_log("amgrvt: no appmgr");
                    fflush(stdout);
                } else if(!strcmp(verb,"amgr") && nf>=3){
                    // call one slot as uint32 fn(this,appid)
                    int slot=(int)strtoul(a1,0,10); uint32_t app=(uint32_t)strtoul(a2,0,10);
                    uint32_t r=amgr_call(slot,app);
                    char l[96]; snprintf(l,sizeof(l),"amgr slot=%d app=%u -> 0x%08x (%u)",slot,app,r,r);
                    printf("GPH DL %s\n",l); dl_log(l); fflush(stdout);
                } else if(!strcmp(verb,"amgrsweep") && nf>=4){
                    // sweep slots [lo,hi) calling (this,appid); match GetAppBuildID==22825947 to anchor
                    uint32_t app=(uint32_t)strtoul(a1,0,10); int lo=(int)strtoul(a2,0,10), hi=(int)strtoul(a3,0,10);
                    for(int s=lo;s<hi;s++){ uint32_t r=amgr_call(s,app);
                        char l[96]; snprintf(l,sizeof(l),"amgrsweep slot=%d -> 0x%08x (%u)%s",s,r,r,(r==22825947u)?" <== GetAppBuildID!":"");
                        printf("GPH DL %s\n",l); dl_log(l); fflush(stdout);
                        pump_frame(eng_pipe); Sleep(30); }
                } else printf("GPH DL ??? unknown cmd\n");
                fflush(stdout);
            }
            pump_frame(eng_pipe); Sleep(400);
        }
    }
    printf("GPH === entering games-played re-assert loop (appid=%u) ===\n",kAppId); fflush(stdout);
    // Run indefinitely: each QR scan costs the owner a manual step, so one login must serve MANY
    // experiments. Keeping the process (and therefore the logged-in session) alive lets us drive
    // further tests into it with frida instead of relaunching + rescanning each time.
    for(int t=0; t<100000; ++t){       // 5s cadence
        bool verbose=(t<3);            // dump the CCM topology on the first few ticks
        void* ccm=find_connected_ccm(B,verbose);
        uint32_t sess = ccm ? probe_u32((void*)((uintptr_t)ccm+0x5f4)) : 0;
        // GH_SEND_CS (Phase-2 route a, step 1): send an EMPTY ClientChangeStatus (0x2cc) through the CONNECTED CM
        // once — the persona/session-registration message retail sends that we never do. Uses the now-correct
        // 10.41 send RVAs. Watch the oracle: do friends flip online>0? (re-do the "empty 0x2cc no effect" test
        // properly: right CM, right time, working RVAs). Then the normal HWMARSHAL/ADDGAME games-played follows.
        static bool s_cs_done=false;
        if(getenv("GH_SEND_CS") && !s_cs_done && ccm && sess){
            s_cs_done=true;
            // Populated ClientChangeStatus: persona_state=1 (Online). GH_CS_FLAGS (hex) optionally sets
            // persona_state_flags(f6). Empty 0x2cc already proven no-effect -> test the ONLINE announce.
            bool setf=getenv("GH_CS_FLAGS")!=nullptr;
            uint32_t flags=setf?(uint32_t)strtoul(getenv("GH_CS_FLAGS"),nullptr,16):0;
            for(int r=0;r<3;r++){
                bool okcs=send_changestatus(B,ccm,1,setf,flags,/*dumpBody=*/r==0);
                printf("GPH SEND-CS ClientChangeStatus(Online) #%d ccm=%p sess=%u flags=%s(0x%x) ok=%d\n",
                       r,ccm,sess,setf?"y":"n",flags,okcs?1:0); fflush(stdout);
                for(int w=0;w<20;w++){ pump_frame(eng_pipe); Sleep(100); }
            }
            // GH_WATCH_SELF: subscribe to our OWN persona so the server pushes the account's cross-device
            // game presence to THIS session (a 2nd same-account session otherwise never receives self-game —
            // that's why the old observer was blind even to a real LC launch). RequestUserInformation(self).
            if(getenv("GH_WATCH_SELF") && friends){
                void** fvt=*(void***)friends;
                typedef bool(*RUIFn)(void*,uint64_t,bool);
                __try{ ((RUIFn)fvt[37])(friends,selfID,false); printf("GPH WATCH-SELF RequestUserInformation(self=%llu)\n",(unsigned long long)selfID); }
                __except(EXCEPTION_EXECUTE_HANDLER){ printf("GPH WATCH-SELF EXC\n"); }
                fflush(stdout);
            }
            for(int w=0;w<60;w++){ pump_frame(eng_pipe); Sleep(100); }   // ~6s for server persona push
            oracle_control("after-send-cs");
            FriendGameInfo sg; fr_gameplayed(friends,selfID,&sg);
            printf("GPH SEND-CS post-oracle serverGameID=%llu\n",(unsigned long long)sg.gameID); fflush(stdout);
        }
        // GH_HWMARSHAL: arm the HW-breakpoint engine-thread marshal ONCE (mirrors the shim), then just
        // watch the games-list count go 0->1 + the doomed-job suppressor + the oracle. No other sends.
        static bool s_hw_armed=false;
        if(getenv("GH_HWMARSHAL")){
            if(!s_hw_armed && ccm && sess){
                if(getenv("GH_ADDGAME_WAIT")){   // Phase-0: hold at the gate so an external frida can attach + arm its RecvPkt capture before games-played fires
                    printf("GPH HWMARSHAL WAITING FOR GO (create C:\\gh41\\go.txt) ccm=%p\n",ccm); fflush(stdout);
                    for(int gw=0; gw<240; ++gw){ if(GetFileAttributesA("C:\\gh41\\go.txt")!=INVALID_FILE_ATTRIBUTES) break; Sleep(500); }
                    printf("GPH HWMARSHAL GO\n"); fflush(stdout);
                }
                s_hw_armed=true; hw_marshal_arm(B,ccm);
            }
            uint32_t cnt = ccm ? probe_u32((void*)((uintptr_t)ccm-0x48+rva::GamesListCount)) : 0;
            FriendGameInfo gi; fr_gameplayed(friends,selfID,&gi);
            printf("GPH HW-TICK t=%d sess=%u glistCount=%u suppress=%ld serverGameID=%llu\n",
                   t, sess, cnt, g_hw_suppress, (unsigned long long)gi.gameID); fflush(stdout);
            for(int w=0;w<50;w++){ pump_frame(eng_pipe); Sleep(100); }
            continue;
        }
        // Phase B repro: one-shot AddClientGame once the CM has a live session. If GH_ADDGAME is set,
        // this replaces the hand-built send — we're testing the REAL register path on the minimal engine.
        static bool s_addgame_done=false;
        if(getenv("GH_ADDGAME") && !s_addgame_done && ccm && sess){
            s_addgame_done=true;
            if(getenv("GH_ADDGAME_WAIT")){   // let an external frida attach before the call
                printf("GPH ADDGAME WAITING FOR GO (create C:\\gh41\\go.txt) ccm=%p cu=%p\n",ccm,(void*)((uintptr_t)ccm-0x48)); fflush(stdout);
                for(int gw=0; gw<120; ++gw){ if(GetFileAttributesA("C:\\gh41\\go.txt")!=INVALID_FILE_ATTRIBUTES) break; Sleep(500); }
                printf("GPH ADDGAME GO\n"); fflush(stdout);
            }
            test_addclientgame(B,ccm);
            for(int w=0;w<50;w++){ pump_frame(eng_pipe); Sleep(100); }   // let any queued job run / deadline fire
            printf("GPH ADDGAME still-alive after 5s (no crash) t=%d\n",t); fflush(stdout);
            continue;
        }
        // When GH_ADDGAME drives the real register path, suppress the manual games-played send so the
        // oracle can ONLY be flipped by AddClientGame's own broadcast (clean attribution).
        // GH_NOSEND: send nothing games-played-related, just read the oracle — lets an external frida
        // engine-thread marshal be the ONLY thing that can flip serverGameID (clean attribution).
        bool ok = (ccm && !getenv("GH_ADDGAME") && !getenv("GH_NOSEND")) ? send_gp_via(B,ccm,kAppId,/*dumpHdr=*/t==0) : false;
        // Retail beats roughly every 9s; at a 5s cadence every other tick is the closest match.
        // NOTE: send_empty_emsg uses the 10.87 send-path RVAs (rva::Ctor/TypeVtbl/... are NOT #ifdef'd for
        // GH_SC1041) -> calling it on the 10.41 build jumps into wrong code and can destabilise the process.
        // Guard both loop sends off on 10.41 (and whenever GH_NOSEND) so the process survives for capture.
#ifdef GH_SC1041
        bool hb = false, cs = false;
#else
        bool hb = (ccm && !getenv("GH_NOSEND") && (t%2)==0) ? send_empty_emsg(B,ccm,0x2bf) : false;
        // ClientChangeStatus: retail sends this as the friends subsystem comes up and we never do.
        bool cs = (ccm && !getenv("GH_NOSEND") && (t==0 || (t%12)==0)) ? send_empty_emsg(B,ccm,0x2cc) : false;
#endif
        FriendGameInfo gi; fr_gameplayed(friends,selfID,&gi);
        if((t%12)==11){ oracle_control("periodic"); cb_report("periodic"); }  // ~1min
        if(t==0) cb_report("startup");   // what the engine posted to our pipe during bring-up+login
        printf("GPH GP-TICK t=%d personaCM=%p sess=%u ok=%d hb=%d cs=%d personaState=%d serverGameID=%llu\n",
               t,ccm,sess,ok?1:0,hb?1:0,cs?1:0,fr_state(friends),(unsigned long long)gi.gameID); fflush(stdout);
        for(int w=0;w<50;w++){ pump_frame(eng_pipe); Sleep(100); }   // ~5s of continuous RunFrame + callback pump
    }
    printf("GPH end\n"); fflush(stdout);
    return 0;
}
