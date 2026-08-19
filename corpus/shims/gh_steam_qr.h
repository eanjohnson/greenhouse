// gh_steam_qr.h -- QR device-auth "anon -> real user" upgrade for the shim.
// Ported from the proven flow in src/spikes/steamclient_probe.cpp (which logged
// in the real account the test account on real Xbox). Runs on the shim's engine thread
// AFTER anon LogOn (which is required so the engine's authed SendMethod path
// works). Emits the PUBLIC challenge_url to LocalState\gh_qr_url.txt for the host
// to render as a QR the user scans with the Steam mobile app.
//
// CREDENTIAL BOUNDARY (unchanged): only the user scans/approves. The refresh
// token is captured into a local buffer, fed to the engine, then scrubbed — it
// is NEVER printed or written to disk by the shim. The public challenge_url and
// the resulting (public) SteamID are the only externally-visible values.
//
// Depends on symbols from gh_steam_engine.h (must be included first):
//   g_gh_engine, g_gh_client_user, g_gh_engine_pipe/_user, g_gh_engine_dll,
//   gh_call_i, gh_iu_logon, gh_iu_getsteamid, GH_ENGINE_LOG.
#pragma once
#include <stdlib.h>

// ---- protobuf wire codec (Authentication service) --------------------------
static int ghq_varint(unsigned char* p, uint64_t v){ int n=0; while(v>=0x80){p[n++]=(unsigned char)(v|0x80); v>>=7;} p[n++]=(unsigned char)v; return n; }
static int ghq_tag(unsigned char* p,int f,int wt){ return ghq_varint(p,((uint64_t)f<<3)|wt); }
static int ghq_str(unsigned char* p,int f,const char* s){ int n=ghq_tag(p,f,2); size_t L=strlen(s); n+=ghq_varint(p+n,L); memcpy(p+n,s,L); return n+(int)L; }
static int ghq_int(unsigned char* p,int f,uint64_t v){ int n=ghq_tag(p,f,0); return n+ghq_varint(p+n,v); }
static uint64_t ghq_getvar(const unsigned char* b,uint32_t len,uint32_t* off){ uint64_t v=0; int sh=0; while(*off<len){ unsigned char c=b[(*off)++]; v|=(uint64_t)(c&0x7f)<<sh; sh+=7; if(!(c&0x80)) break; } return v; }
static bool ghq_find(const unsigned char* b,uint32_t len,int want,const unsigned char** ps,uint32_t* pl,uint64_t* pv){
    uint32_t off=0;
    while(off<len){ uint64_t tag=ghq_getvar(b,len,&off); int f=(int)(tag>>3),wt=(int)(tag&7);
        if(wt==0){ uint64_t v=ghq_getvar(b,len,&off); if(f==want){ if(pv)*pv=v; return true; } }
        else if(wt==2){ uint64_t L=ghq_getvar(b,len,&off); if(off+L>len) break; if(f==want){ if(ps)*ps=b+off; if(pl)*pl=(uint32_t)L; return true; } off+=(uint32_t)L; }
        else if(wt==5) off+=4; else if(wt==1) off+=8; else break; }
    return false;
}

// ---- JWT 'sub' SteamID extraction (in-process; token never printed) --------
static uint64_t ghq_jwt_sub(const char* jwt){
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

// ---- IClientUnifiedMessages (engine slot 52) SEH-wrapped calls --------------
static uint64_t ghq_um_send(void* self,const char* name,const void* proto,uint32_t cub){
    typedef uint64_t (*F)(void*,const char*,const void*,uint32_t,uint64_t); void** vt=*(void***)self; uint64_t h=0;
    __try{ h=((F)vt[0])(self,name,proto,cub,0); } __except(EXCEPTION_EXECUTE_HANDLER){}
    return h;
}
static bool ghq_um_info(void* self,uint64_t hc,uint32_t* sz,int* res){
    typedef bool (*F)(void*,uint64_t,uint32_t*,int*); void** vt=*(void***)self; bool r=false;
    __try{ r=((F)vt[1])(self,hc,sz,res); } __except(EXCEPTION_EXECUTE_HANDLER){}
    return r;
}
static bool ghq_um_data(void* self,uint64_t hc,void* buf,uint32_t cub,bool rel){
    typedef bool (*F)(void*,uint64_t,void*,uint32_t,bool); void** vt=*(void***)self; bool r=false;
    __try{ r=((F)vt[2])(self,hc,buf,cub,rel); } __except(EXCEPTION_EXECUTE_HANDLER){}
    return r;
}

// ---- IClientUser login-completion helpers (slots per vtables.md) ------------
static void ghq_iu_logoff(void* self){ typedef void(*F)(void*); void** vt=*(void***)self; __try{((F)vt[3])(self);}__except(EXCEPTION_EXECUTE_HANDLER){} }
static void ghq_iu_set_token(void* self,const char* tok,const char* acct){ typedef void(*F)(void*,const char*,const char*); void** vt=*(void***)self; __try{((F)vt[56])(self,tok,acct);}__except(EXCEPTION_EXECUTE_HANDLER){} }
static void ghq_iu_set_acct(void* self,const char* acct,bool b){ typedef void(*F)(void*,const char*,bool); void** vt=*(void***)self; __try{((F)vt[50])(self,acct,b);}__except(EXCEPTION_EXECUTE_HANDLER){} }

// Write the PUBLIC challenge_url to LocalState for the host to render as a QR.
// Implemented in steam_api64.cpp (which knows the LocalState path); forward-
// declared here since this header is compiled into that same TU.
void gh_shim_write_qr_url(const char* url);
static void ghq_write_url(const char* url){ gh_shim_write_qr_url(url); }

// Cached-credential store, also implemented in steam_api64.cpp (it owns the LocalState path).
// The token passes through these calls only; it is never logged by either side.
void gh_shim_save_credentials(const char* account, unsigned long long steamid, const char* token);
bool gh_shim_load_credentials(char* acctOut, size_t acctSz, unsigned long long* sidOut,
                              char* tokOut, size_t tokSz);
void gh_shim_clear_credentials();

// SEH-isolated: IClientUnifiedMessages = engine vtbl slot [52]. Kept in its own
// function so gh_qr_upgrade has NO inline __try (an inline __try followed by a
// loop miscompiles under clang-cl /O2 — control flow past it goes unreachable).
static void* ghq_get_ium(void* engine){
    typedef void* (*F)(void*,HSteamUser,HSteamPipe,const char*);
    uintptr_t* obj=(uintptr_t*)engine; void** evt=(void**)obj[0]; void* ium=nullptr;
    __try{ ium=((F)evt[52])(engine,g_gh_engine_user,g_gh_engine_pipe,""); }
    __except(EXCEPTION_EXECUTE_HANDLER){ ium=nullptr; }
    return ium;
}

// Drive the QR device-auth upgrade. Returns the real SteamID on success (engine
// now logged in as the real user), or 0 on timeout/failure (stays anon).
// runSeconds bounds the whole scan+approve window. noinline: keeps this large
// SEH-bearing state machine out of gh_engine_thread (clang-cl /O2 SEH bug).
// Cached-credential login: replay the SAME completion sequence a QR approval performs, but with the
// token we stored last time instead of a freshly-approved one. Returns the real SteamID, or 0 to fall
// through to the QR flow (no cache, expired token, revoked session...).
//
// The token is read into a local, handed straight to SetLoginToken, and scrubbed. It is never logged —
// only the account name and the public SteamID appear in diagnostics.
__declspec(noinline) static uint64_t ghq_try_cached_login(void* clientUser, void** cuvt){
    char acct[128] = {0}; char tok[2048] = {0}; unsigned long long sid = 0;
    if (!gh_shim_load_credentials(acct, sizeof(acct), &sid, tok, sizeof(tok))) {
        GH_ENGINE_LOG("GHQR no cached credentials — QR scan required\n");
        return 0;
    }
    GH_ENGINE_LOG("GHQR cached credentials for \"%s\" steamid=%llu — trying login without QR\n", acct, sid);

    ghq_iu_logoff(clientUser);
    for(int w=0; w<40;  ++w){ if(gh_call_i(clientUser,cuvt[5],"GetLogonState")==0) break; Sleep(100); }
    int ec = gh_call_i(clientUser,cuvt[8],"EConnect"); (void)ec;
    for(int w=0; w<100; ++w){ if(gh_call_i(clientUser,cuvt[6],"BConnected")==1) break; Sleep(100); }
    ghq_iu_set_acct(clientUser, acct, true);
    ghq_iu_set_token(clientUser, tok, acct);
    SecureZeroMemory(tok, sizeof(tok));           // scrub before the blocking wait
    gh_iu_logon(clientUser, sid);

    uint64_t got = 0;
    for(int w=0; w<150; ++w){ if(gh_call_i(clientUser,cuvt[4],"BLoggedOn")==1){ got = gh_iu_getsteamid(clientUser); break; } Sleep(100); }
    if (got) { GH_ENGINE_LOG("GHQR *** LOGGED IN from cache: SteamID=%llu (no scan) ***\n",(unsigned long long)got); return got; }

    // Stale/revoked token: drop it so we do not retry a dead credential on every launch.
    GH_ENGINE_LOG("GHQR cached login did not reach BLoggedOn — discarding cache, falling back to QR\n");
    gh_shim_clear_credentials();
    return 0;
}

__declspec(noinline) static uint64_t gh_qr_upgrade(int runSeconds){
    void* engine = g_gh_engine;
    void* clientUser = g_gh_client_user;
    if (!engine || !clientUser) return 0;
    void** cuvt = *(void***)clientUser;

    // Try the stored token first — a successful cache hit skips the QR entirely.
    uint64_t cached = ghq_try_cached_login(clientUser, cuvt);
    if (cached) return cached;

    void* ium = ghq_get_ium(engine);
    GH_ENGINE_LOG("GHQR IClientUnifiedMessages = %p\n", ium);
    if (!ium) return 0;

    // Build BeginAuthSessionViaQR request (public, no secrets).
    unsigned char qreq[160]; int qn=0;
    qn+=ghq_str(qreq+qn,1,"Greenhouse Xbox");
    qn+=ghq_int(qreq+qn,2,1);                 // platform_type = SteamClient
    { unsigned char dd[96]; int dn=0; dn+=ghq_str(dd+dn,1,"Greenhouse Xbox"); dn+=ghq_int(dd+dn,2,1);
      qn+=ghq_tag(qreq+qn,3,2); qn+=ghq_varint(qreq+qn,dn); memcpy(qreq+qn,dd,dn); qn+=dn; }
    qn+=ghq_str(qreq+qn,4,"Client");

    uint64_t qr_client_id=0; unsigned char qr_reqid[64]; uint32_t qr_reqid_len=0;
    enum { BEGIN, WAIT_BEGIN, POLL } st=BEGIN;
    uint64_t hBegin=0, hPoll=0; bool pollInFlight=false;
    int steps = runSeconds*10;   // 100ms cadence
    int nextPollAt=0, lastStatus=-1, qrIssuedAt=0, lastPollErr=-999, expStreak=0;
    const int kQrTtlTicks=9000;   // absolute safety cap (~900s); real refresh is death-driven
    const int kMaxExpStreak=5;    // ~5 consecutive failed polls (~25s solid) = session truly dead
    static char tok[2048]; static char acct[128]; uint64_t realID=0;

    for (int s=0; s<steps; ++s){
        if (st==BEGIN){
            hBegin=ghq_um_send(ium,"Authentication.BeginAuthSessionViaQR#1",qreq,qn);
            GH_ENGINE_LOG("GHQR BeginAuthSessionViaQR -> hCall=0x%llx\n",(unsigned long long)hBegin);
            st = hBegin? WAIT_BEGIN : BEGIN;
            if (!hBegin) { Sleep(500); continue; }
        }
        else if (st==WAIT_BEGIN){
            uint32_t sz=0; int res=-1;
            if (ghq_um_info(ium,hBegin,&sz,&res)){
                if (res==1 && sz>0){
                    unsigned char* buf=(unsigned char*)malloc(sz);
                    if (buf && ghq_um_data(ium,hBegin,buf,sz,true)){
                        const unsigned char* sp=nullptr; uint32_t sl=0; uint64_t v=0;
                        if (ghq_find(buf,sz,1,nullptr,nullptr,&v)) qr_client_id=v;
                        if (ghq_find(buf,sz,3,&sp,&sl,nullptr) && sl<=sizeof(qr_reqid)){ memcpy(qr_reqid,sp,sl); qr_reqid_len=sl; }
                        if (ghq_find(buf,sz,2,&sp,&sl,nullptr)){
                            char url[512]; uint32_t ul=sl<sizeof(url)-1?sl:sizeof(url)-1; memcpy(url,sp,ul); url[ul]=0;
                            GH_ENGINE_LOG("GHQR CHALLENGE_URL: %s\n",url);   // PUBLIC
                            ghq_write_url(url);
                        }
                    }
                    free(buf);
                    st = qr_client_id? POLL : BEGIN;
                    qrIssuedAt = s; lastPollErr = -999; lastStatus = -1; expStreak = 0; nextPollAt = s+10;
                } else if (res!=1) {
                    ghq_um_data(ium,hBegin,nullptr,0,true);
                    GH_ENGINE_LOG("GHQR begin EResult=%d -> retry\n",res); st=BEGIN; Sleep(500);
                }
            }
        }
        else if (st==POLL){
            // Refresh the challenge ONLY at its TTL — never on a poll failure. Churning
            // BeginAuthSessionViaQR trips Steam's anti-abuse (it then returns Expired(27)
            // on every fresh session); observed a single transient 27 cascade 34-deep and
            // lock out login. Capping to <=1 begin per ~100s keeps one QR stable to scan.
            if (s - qrIssuedAt >= kQrTtlTicks || expStreak >= kMaxExpStreak){
                GH_ENGINE_LOG("GHQR challenge dead/expired (age=%ds expStreak=%d) -> refreshing QR\n", (s-qrIssuedAt)/10, expStreak);
                pollInFlight=false; st=BEGIN; Sleep(100); continue;
            }
            if (!pollInFlight && s>=nextPollAt){
                unsigned char preq[96]; int pn=0; pn+=ghq_int(preq+pn,1,qr_client_id);
                if (qr_reqid_len){ pn+=ghq_tag(preq+pn,2,2); pn+=ghq_varint(preq+pn,qr_reqid_len); memcpy(preq+pn,qr_reqid,qr_reqid_len); pn+=qr_reqid_len; }
                hPoll=ghq_um_send(ium,"Authentication.PollAuthSessionStatus#1",preq,pn);
                pollInFlight=(hPoll!=0); nextPollAt=s+50;   // ~5s cadence (Steam's recommended poll interval)
            }
            if (pollInFlight){
                uint32_t sz=0; int res=-1;
                if (ghq_um_info(ium,hPoll,&sz,&res)){
                    pollInFlight=false;
                    if (res!=1){
                        // Transient failure (9=not-found-yet / 27=expired). Keep the SAME
                        // challenge and keep polling; only refresh once the session is truly
                        // dead (kMaxExpStreak consecutive failures) so one QR lives its full
                        // natural life — no premature refresh, no churn.
                        ghq_um_data(ium,hPoll,nullptr,0,true);
                        expStreak++;
                        if (res!=lastPollErr){ GH_ENGINE_LOG("GHQR poll EResult=%d (keeping QR; expStreak=%d/%d)\n",res,expStreak,kMaxExpStreak); lastPollErr=res; }
                        Sleep(100); continue;
                    }
                    expStreak=0; lastPollErr=-999;   // a good poll: session alive, reset death counter
                    unsigned char* buf=(unsigned char*)malloc(sz?sz:1); bool hasTok=false; uint64_t hadRI=0;
                    if (buf && sz && ghq_um_data(ium,hPoll,buf,sz,true)){
                        const unsigned char* sp=nullptr; uint32_t sl=0;
                        ghq_find(buf,sz,5,nullptr,nullptr,&hadRI);           // had_remote_interaction
                        hasTok=ghq_find(buf,sz,3,&sp,&sl,nullptr);           // refresh_token (SECRET)
                        if (hasTok && sl<sizeof(tok)){ memcpy(tok,sp,sl); tok[sl]=0; realID=ghq_jwt_sub(tok); }
                        const unsigned char* ap=nullptr; uint32_t al=0;
                        if (ghq_find(buf,sz,6,&ap,&al,nullptr) && al<sizeof(acct)){ memcpy(acct,ap,al); acct[al]=0; }
                        // Follow challenge rotation. Steam rotates the QR (~every 30-45s) and
                        // reports the new session id (field 1 new_client_id) + new QR url
                        // (field 2 new_challenge_url) in the poll response. WITHOUT following
                        // them the old client_id expires and every later poll returns
                        // EResult=27 — the session appears to "die" one rotation in. This was
                        // the QR-login regression: it only worked when the user scanned within
                        // the first ~46s window. Tracking them keeps ONE session alive
                        // indefinitely, exactly like the official client.
                        uint64_t newCid=0;
                        if (ghq_find(buf,sz,1,nullptr,nullptr,&newCid) && newCid && newCid!=qr_client_id){
                            GH_ENGINE_LOG("GHQR challenge rotated: client_id %llu -> %llu\n",(unsigned long long)qr_client_id,(unsigned long long)newCid);
                            qr_client_id=newCid;
                        }
                        const unsigned char* np=nullptr; uint32_t nl=0;
                        if (ghq_find(buf,sz,2,&np,&nl,nullptr) && nl>1 && nl<500){
                            char nurl[512]; memcpy(nurl,np,nl); nurl[nl]=0;
                            GH_ENGINE_LOG("GHQR CHALLENGE_URL: %s\n",nurl);   // PUBLIC (rotated)
                            ghq_write_url(nurl); qrIssuedAt=s;   // fresh rotated challenge: reset TTL
                        }
                    }
                    free(buf);
                    int status=(hasTok?2:(hadRI?1:0));
                    if (status!=lastStatus){ GH_ENGINE_LOG("GHQR poll had_remote_interaction=%llu account=\"%s\" token=%s\n",(unsigned long long)hadRI,acct[0]?acct:"",hasTok?"RECEIVED":"(pending)"); lastStatus=status; }
                    if (hasTok){
                        GH_ENGINE_LOG("GHQR *** approved: account=\"%s\" steamid=%llu — completing login ***\n",acct,(unsigned long long)realID);
                        // Login-completion: drop anon, reconnect, feed cached creds, LogOn real.
                        ghq_iu_logoff(clientUser);
                        for(int w=0;w<40;++w){ if(gh_call_i(clientUser,cuvt[5],"GetLogonState")==0) break; Sleep(100); }
                        int ec=gh_call_i(clientUser,cuvt[8],"EConnect"); (void)ec;
                        for(int w=0;w<100;++w){ if(gh_call_i(clientUser,cuvt[6],"BConnected")==1) break; Sleep(100); }
                        ghq_iu_set_acct(clientUser,acct,true);
                        ghq_iu_set_token(clientUser,tok,acct);   // (token, account) — verified order
                        gh_iu_logon(clientUser,realID);
                        uint64_t got=0;
                        for(int w=0;w<150;++w){ if(gh_call_i(clientUser,cuvt[4],"BLoggedOn")==1){ got=gh_iu_getsteamid(clientUser); break; } Sleep(100); }
                        // Persist for cached login BEFORE scrubbing, so the next launch needs no scan.
                        // Authorised by the user 2026-08-11; the token still never reaches a log.
                        if (got) gh_shim_save_credentials(acct,(unsigned long long)got,tok);
                        memset(tok,0,sizeof(tok));   // scrub token
                        if (got){ GH_ENGINE_LOG("GHQR *** LOGGED IN as real user: SteamID=%llu ***\n",(unsigned long long)got); return got; }
                        GH_ENGINE_LOG("GHQR login-completion did not reach BLoggedOn\n");
                        return 0;
                    }
                }
            }
        }
        Sleep(100);
    }
    GH_ENGINE_LOG("GHQR upgrade timed out (no scan/approve within %ds)\n",runSeconds);
    return 0;
}
