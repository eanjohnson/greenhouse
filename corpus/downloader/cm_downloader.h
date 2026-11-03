// cm_downloader.h -- in-process Steam depot downloader library (no steamclient engine).
//
// Extracted from the proven gh_downloader.cpp so it can be compiled BOTH as the
// standalone gh_downloader.exe and directly into the loader (guest_loader.cpp under
// GH_LAUNCHER) as a callable library: the launcher runs DownloadDepot() on its UI
// thread with a progress callback that draws the D2D progress bar.
//
// Everything lives in namespace ghdl to avoid symbol collisions with the loader.
// The heavy lifting (crypto/protobuf/CM/CDN/manifest/chunk decode) is byte-exact
// validated -- see GREENHOUSE.md 29.5/29.6 and the steam-cdn-download memory.
//
// Requires linking: winhttp.lib ws2_32.lib bcrypt.lib + a zlib inflate subset,
// and zstd.exe / 7za.exe reachable via CreateProcess (the loader's surrogate).
#pragma once
#ifndef GH_CM_DOWNLOADER_H
#define GH_CM_DOWNLOADER_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "zlib.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace ghdl {

#ifndef STATUS_SUCCESS
#define GHDL_STATUS_SUCCESS ((NTSTATUS)0)
#else
#define GHDL_STATUS_SUCCESS STATUS_SUCCESS
#endif

typedef std::vector<uint8_t> Bytes;
// progress callback: phase string, 0..100 percent, files done/total, bytes written
typedef void(*ProgressFn)(void* ctx,const char* phase,int percent,int fdone,int ftot,unsigned long long bytes);

// CreateProcessW indirection: the standalone exe uses the real Win32 call; when
// compiled into the loader (which cannot spawn real processes in the container),
// the loader points this at its in-process surrogate (Bed_CreateProcessW), which
// already runs zstd/7za-style guests. Set BEFORE calling DownloadDepot.
typedef BOOL(WINAPI* CreateProcessWFn)(LPCWSTR,LPWSTR,LPSECURITY_ATTRIBUTES,LPSECURITY_ATTRIBUTES,BOOL,DWORD,LPVOID,LPCWSTR,LPSTARTUPINFOW,LPPROCESS_INFORMATION);
inline CreateProcessWFn g_createProcessW = &::CreateProcessW;
// CreateFileW indirection for SCRATCH files only. The surrogate child (zstd/7za)
// reaches the filesystem through the loader's VFS bed, which does NOT coincide
// with the loader's own ::CreateFileW ("vfs mirror" vs "direct LocalState"). So
// the loader must read/write the shared scratch files through the SAME bed
// (Bed_CreateFileW) the child uses, else they see different bytes. Standalone exe
// leaves this as ::CreateFileW. (Real game-tree files stay on ::CreateFileW.)
typedef HANDLE(WINAPI* CreateFileWFn)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
inline CreateFileWFn g_createFileW = &::CreateFileW;
// Directory for scratch files (decompress in/out). Empty => cwd (the standalone
// exe under Wine). In the loader set this to a WRITABLE absolute dir (LocalState)
// so both the loader and the surrogate child resolve the same scratch files.
inline std::wstring g_tempDir;
inline std::wstring gh_tmp(const wchar_t* name){ return g_tempDir.empty()? std::wstring(name) : (g_tempDir+name); }
// Optional diagnostic sink (loader sets to Diag). Throttled by the caller.
typedef void(*LogFn)(const char* msg);
inline LogFn g_logFn = nullptr;
inline int g_logBudget = 0;
inline void ghlog(const std::string& s){ if(g_logFn && g_logBudget>0){ g_logBudget--; g_logFn(("GHDL "+s).c_str()); } }
// Unbudgeted sink for the 20XX.exe experiment (plan: never rely on g_logBudget).
inline void ghlog_always(const std::string& s){ if(g_logFn) g_logFn(("GHDL "+s).c_str()); }
// Cancel hook: the host's "user pressed B" query. Polled before each CDN request,
// chunk and workshop item; a request already in flight runs to its timeout.
typedef int(*CancelFn)();
inline CancelFn g_cancelFn = nullptr;
inline bool cancelled(){ return g_cancelFn && g_cancelFn(); }
inline bool is_20xx_exe(const std::string& rel){
    size_t sl=rel.find_last_of("/\\"); const char* b=(sl==std::string::npos)?rel.c_str():rel.c_str()+sl+1;
    return _stricmp(b,"20XX.exe")==0;
}
// Write launch-critical files first so a full LocalState quota cannot leave a
// 0-byte main PE after the bar hits 100% (ERROR_DISK_FULL = 112).
inline int file_write_prio(const std::string& name){
    size_t sl=name.find_last_of("/\\"); const char* b=(sl==std::string::npos)?name.c_str():name.c_str()+sl+1;
    const char* dot=strrchr(b,'.');
    if(dot && _stricmp(dot,".exe")==0) return 0;
    if(_stricmp(b,"steam_api64.dll")==0 || _stricmp(b,"steam_api.dll")==0) return 1;
    if(dot && _stricmp(dot,".dll")==0) return 2;
    return 3;
}
inline void trace_exe_reset(){
    std::wstring p=gh_tmp(L"gh_exe_trace.txt");
    HANDLE h=CreateFileW(p.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h!=INVALID_HANDLE_VALUE) CloseHandle(h);
}
inline void trace_exe(const std::string& line){
    ghlog_always(line);
    std::wstring p=gh_tmp(L"gh_exe_trace.txt");
    HANDLE h=CreateFileW(p.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h!=INVALID_HANDLE_VALUE){
        std::string s=line+"\r\n"; DWORD w=0; WriteFile(h,s.data(),(DWORD)s.size(),&w,nullptr); CloseHandle(h);
    }
}
inline std::string hex_bytes(const uint8_t* d,size_t n){
    std::string o; o.resize(n*2);
    for(size_t i=0;i<n;i++){ static const char* H="0123456789abcdef"; o[i*2]=H[d[i]>>4]; o[i*2+1]=H[d[i]&0xf]; }
    return o;
}

static const uint32_t PROTO_MASK = 0x80000000u;
enum : uint32_t {
    EMsg_Multi=1, EMsg_ServiceMethodResponse=147, EMsg_ServiceMethodCallFromClient=151,
    EMsg_ChannelEncryptRequest=1303, EMsg_ChannelEncryptResponse=1304, EMsg_ChannelEncryptResult=1305,
    EMsg_ClientLogOnResponse=751, EMsg_ClientLogon=5514,
    EMsg_ClientGetDepotDecryptionKey=5438, EMsg_ClientGetDepotDecryptionKeyResponse=5439,
    EMsg_ClientLicenseList=780,
    EMsg_ClientPICSProductInfoRequest=8903, EMsg_ClientPICSProductInfoResponse=8904,
    EMsg_ClientPICSAccessTokenRequest=8905, EMsg_ClientPICSAccessTokenResponse=8906,
};
static const uint64_t ANON_STEAMID = ((uint64_t)1<<56)|((uint64_t)10<<52);
enum { FLAG_ENCRYPTED=4, FLAG_DIRECTORY=64, FLAG_SYMLINK=512 };

static const unsigned char kUniverseMod[128] = {
  0xdf,0xec,0x1a,0xd6,0x2c,0x10,0x66,0x2c,0x17,0x35,0x3a,0x14,0xb0,0x7c,0x59,0x11,
  0x7f,0x9d,0xd3,0xd8,0x2b,0x7a,0xe3,0xe0,0x15,0xcd,0x19,0x1e,0x46,0xe8,0x7b,0x87,
  0x74,0xa2,0x18,0x46,0x31,0xa9,0x03,0x14,0x79,0x82,0x8e,0xe9,0x45,0xa2,0x49,0x12,
  0xa9,0x23,0x68,0x73,0x89,0xcf,0x69,0xa1,0xb1,0x61,0x46,0xbd,0xc1,0xbe,0xbf,0xd6,
  0x01,0x1b,0xd8,0x81,0xd4,0xdc,0x90,0xfb,0xfe,0x4f,0x52,0x73,0x66,0xcb,0x95,0x70,
  0xd7,0xc5,0x8e,0xba,0x1c,0x7a,0x33,0x75,0xa1,0x62,0x34,0x46,0xbb,0x60,0xb7,0x80,
  0x68,0xfa,0x13,0xa7,0x7a,0x8a,0x37,0x4b,0x9e,0xc6,0xf4,0x5d,0x5f,0x3a,0x99,0xf9,
  0x9e,0xc4,0x3a,0xe9,0x63,0xa2,0xbb,0x88,0x19,0x28,0xe0,0xe7,0x14,0xc0,0x42,0x89,
};

// ==================== protobuf ====================
inline void pb_varint(Bytes& b,uint64_t v){ while(v>=0x80){b.push_back((uint8_t)(v|0x80));v>>=7;} b.push_back((uint8_t)v);}
inline void pb_tag(Bytes& b,uint32_t f,uint32_t w){ pb_varint(b,((uint64_t)f<<3)|w);}
inline void pb_u32(Bytes& b,uint32_t f,uint32_t v){ pb_tag(b,f,0); pb_varint(b,v);}
inline void pb_u64(Bytes& b,uint32_t f,uint64_t v){ pb_tag(b,f,0); pb_varint(b,v);}
inline void pb_bool(Bytes& b,uint32_t f,bool v){ pb_tag(b,f,0); pb_varint(b,v?1:0);}
inline void pb_fix64(Bytes& b,uint32_t f,uint64_t v){ pb_tag(b,f,1); for(int i=0;i<8;i++) b.push_back((uint8_t)(v>>(8*i)));}
inline void pb_str(Bytes& b,uint32_t f,const std::string& s){ pb_tag(b,f,2); pb_varint(b,s.size()); b.insert(b.end(),s.begin(),s.end());}
inline void pb_bytes(Bytes& b,uint32_t f,const Bytes& d){ pb_tag(b,f,2); pb_varint(b,d.size()); b.insert(b.end(),d.begin(),d.end());}

struct PbReader {
    const uint8_t* p; const uint8_t* end;
    PbReader(const uint8_t* d,size_t n):p(d),end(d+n){}
    uint64_t varint(){ uint64_t v=0;int s=0; while(p<end){uint8_t b=*p++; v|=(uint64_t)(b&0x7f)<<s; if(!(b&0x80))break; s+=7;} return v;}
    bool next(uint32_t& f,uint32_t& w){ if(p>=end)return false; uint64_t k=varint(); f=(uint32_t)(k>>3); w=(uint32_t)(k&7); return true;}
    void skip(uint32_t w){ switch(w){case 0:varint();break;case 1:p+=8;break;case 5:p+=4;break;case 2:{uint64_t n=varint();p+=n;break;}default:p=end;break;} }
    uint64_t fix64(){ uint64_t v=0; for(int i=0;i<8&&p<end;i++) v|=(uint64_t)(*p++)<<(8*i); return v;}
    Bytes lendelim(){ uint64_t n=varint(); size_t take=(p+n<=end)?(size_t)n:(size_t)(end-p); Bytes o(p,p+take); p+=take; return o;}
};

// ==================== BCrypt crypto ====================
inline bool gen_random(uint8_t* b,size_t n){ return BCryptGenRandom(NULL,b,(ULONG)n,BCRYPT_USE_SYSTEM_PREFERRED_RNG)==GHDL_STATUS_SUCCESS; }
inline bool rsa_oaep_encrypt(const Bytes& plain,Bytes& out){
    BCRYPT_ALG_HANDLE hAlg=0; BCRYPT_KEY_HANDLE hKey=0; bool ok=false;
    if(BCryptOpenAlgorithmProvider(&hAlg,BCRYPT_RSA_ALGORITHM,NULL,0)!=GHDL_STATUS_SUCCESS) return false;
    Bytes blob(sizeof(BCRYPT_RSAKEY_BLOB)); BCRYPT_RSAKEY_BLOB* h=(BCRYPT_RSAKEY_BLOB*)blob.data();
    h->Magic=BCRYPT_RSAPUBLIC_MAGIC; h->BitLength=1024; h->cbPublicExp=1; h->cbModulus=128; h->cbPrime1=0; h->cbPrime2=0;
    blob.push_back(0x11); blob.insert(blob.end(),kUniverseMod,kUniverseMod+128);
    if(BCryptImportKeyPair(hAlg,NULL,BCRYPT_RSAPUBLIC_BLOB,&hKey,blob.data(),(ULONG)blob.size(),0)==GHDL_STATUS_SUCCESS){
        BCRYPT_OAEP_PADDING_INFO pad={BCRYPT_SHA1_ALGORITHM,NULL,0}; ULONG need=0;
        if(BCryptEncrypt(hKey,(PUCHAR)plain.data(),(ULONG)plain.size(),&pad,NULL,0,NULL,0,&need,BCRYPT_PAD_OAEP)==GHDL_STATUS_SUCCESS){
            out.resize(need); ULONG got=0;
            if(BCryptEncrypt(hKey,(PUCHAR)plain.data(),(ULONG)plain.size(),&pad,NULL,0,out.data(),need,&got,BCRYPT_PAD_OAEP)==GHDL_STATUS_SUCCESS){out.resize(got);ok=true;}
        }
        BCryptDestroyKey(hKey);
    }
    BCryptCloseAlgorithmProvider(hAlg,0); return ok;
}
inline bool aes256(const uint8_t key[32],int mode,bool enc,const uint8_t* iv,const Bytes& in,Bytes& out){
    BCRYPT_ALG_HANDLE hAlg=0; BCRYPT_KEY_HANDLE hKey=0; bool ok=false;
    if(BCryptOpenAlgorithmProvider(&hAlg,BCRYPT_AES_ALGORITHM,NULL,0)!=GHDL_STATUS_SUCCESS) return false;
    const wchar_t* chain=(mode==0)?BCRYPT_CHAIN_MODE_ECB:BCRYPT_CHAIN_MODE_CBC;
    BCryptSetProperty(hAlg,BCRYPT_CHAINING_MODE,(PUCHAR)chain,(ULONG)((wcslen(chain)+1)*sizeof(wchar_t)),0);
    if(BCryptGenerateSymmetricKey(hAlg,&hKey,NULL,0,(PUCHAR)key,32,0)==GHDL_STATUS_SUCCESS){
        uint8_t ivc[16]; if(iv) memcpy(ivc,iv,16); PUCHAR pIv=(mode==1)?ivc:NULL; ULONG ivLen=(mode==1)?16:0; ULONG need=0;
        NTSTATUS st=enc?BCryptEncrypt(hKey,(PUCHAR)in.data(),(ULONG)in.size(),NULL,pIv,ivLen,NULL,0,&need,0)
                       :BCryptDecrypt(hKey,(PUCHAR)in.data(),(ULONG)in.size(),NULL,pIv,ivLen,NULL,0,&need,0);
        if(st==GHDL_STATUS_SUCCESS){ out.resize(need); ULONG got=0; if(iv)memcpy(ivc,iv,16);
            st=enc?BCryptEncrypt(hKey,(PUCHAR)in.data(),(ULONG)in.size(),NULL,pIv,ivLen,out.data(),need,&got,0)
                  :BCryptDecrypt(hKey,(PUCHAR)in.data(),(ULONG)in.size(),NULL,pIv,ivLen,out.data(),need,&got,0);
            if(st==GHDL_STATUS_SUCCESS){out.resize(got);ok=true;} }
        BCryptDestroyKey(hKey);
    }
    BCryptCloseAlgorithmProvider(hAlg,0); return ok;
}
inline bool hmac_sha1(const uint8_t* secret,size_t slen,const Bytes& data,uint8_t out[20]){
    BCRYPT_ALG_HANDLE hAlg=0; BCRYPT_HASH_HANDLE hH=0; bool ok=false;
    if(BCryptOpenAlgorithmProvider(&hAlg,BCRYPT_SHA1_ALGORITHM,NULL,BCRYPT_ALG_HANDLE_HMAC_FLAG)!=GHDL_STATUS_SUCCESS) return false;
    if(BCryptCreateHash(hAlg,&hH,NULL,0,(PUCHAR)secret,(ULONG)slen,0)==GHDL_STATUS_SUCCESS){
        if(BCryptHashData(hH,(PUCHAR)data.data(),(ULONG)data.size(),0)==GHDL_STATUS_SUCCESS && BCryptFinishHash(hH,out,20,0)==GHDL_STATUS_SUCCESS) ok=true;
        BCryptDestroyHash(hH);
    }
    BCryptCloseAlgorithmProvider(hAlg,0); return ok;
}
inline bool sha1_hash(const Bytes& data,uint8_t out[20]){
    BCRYPT_ALG_HANDLE hAlg=0; BCRYPT_HASH_HANDLE hH=0; bool ok=false;
    if(BCryptOpenAlgorithmProvider(&hAlg,BCRYPT_SHA1_ALGORITHM,NULL,0)!=GHDL_STATUS_SUCCESS) return false;
    if(BCryptCreateHash(hAlg,&hH,NULL,0,NULL,0,0)==GHDL_STATUS_SUCCESS){
        if(BCryptHashData(hH,(PUCHAR)data.data(),(ULONG)data.size(),0)==GHDL_STATUS_SUCCESS && BCryptFinishHash(hH,out,20,0)==GHDL_STATUS_SUCCESS) ok=true;
        BCryptDestroyHash(hH);
    }
    BCryptCloseAlgorithmProvider(hAlg,0); return ok;
}
inline bool pkcs7_unpad(Bytes& b){ if(b.empty())return false; uint8_t p=b.back(); if(p<1||p>16||p>b.size())return false; b.resize(b.size()-p); return true; }
inline Bytes pkcs7_pad(const Bytes& in){ size_t p=16-(in.size()%16); Bytes o=in; o.insert(o.end(),p,(uint8_t)p); return o; }
inline bool sym_encrypt_hmac(const uint8_t key[32],const uint8_t hs[16],const Bytes& plain,Bytes& out){
    uint8_t prefix[3]; if(!gen_random(prefix,3))return false;
    Bytes hin(prefix,prefix+3); hin.insert(hin.end(),plain.begin(),plain.end());
    uint8_t h[20]; if(!hmac_sha1(hs,16,hin,h))return false; uint8_t iv[16]; memcpy(iv,h,13); memcpy(iv+13,prefix,3);
    Bytes ei,ct; if(!aes256(key,0,true,NULL,Bytes(iv,iv+16),ei))return false; if(!aes256(key,1,true,iv,pkcs7_pad(plain),ct))return false;
    out=ei; out.insert(out.end(),ct.begin(),ct.end()); return true;
}
inline bool sym_decrypt_hmac(const uint8_t key[32],const uint8_t hs[16],const Bytes& ct,Bytes& out){
    if(ct.size()<32)return false; Bytes ei(ct.begin(),ct.begin()+16),body(ct.begin()+16,ct.end());
    Bytes iv; if(!aes256(key,0,false,NULL,ei,iv))return false; if(iv.size()!=16)return false;
    Bytes plain; if(!aes256(key,1,false,iv.data(),body,plain))return false; if(!pkcs7_unpad(plain))return false; out=std::move(plain); return true;
}
inline bool sym_decrypt(const uint8_t key[32],const Bytes& ct,Bytes& out){
    if(ct.size()<32)return false; Bytes iv; if(!aes256(key,0,false,NULL,Bytes(ct.begin(),ct.begin()+16),iv))return false; if(iv.size()!=16)return false;
    Bytes plain; if(!aes256(key,1,false,iv.data(),Bytes(ct.begin()+16,ct.end()),plain))return false; if(!pkcs7_unpad(plain))return false; out=std::move(plain); return true;
}
inline Bytes b64decode(const std::string& s){
    static int T[256]; static bool init=false;
    if(!init){ for(int i=0;i<256;i++)T[i]=-1; const char* a="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; for(int i=0;i<64;i++)T[(uint8_t)a[i]]=i; init=true; }
    Bytes out; int val=0,bits=-8;
    for(unsigned char c:s){ if(c=='=')break; int d=T[c]; if(d<0)continue; val=(val<<6)|d; bits+=6; if(bits>=0){out.push_back((uint8_t)((val>>bits)&0xff)); bits-=8;} }
    return out;
}

// ==================== decompression (shell zstd.exe / 7za.exe) ====================
// Byte length of the zstd frame at d[0..] (magic 28 b5 2f fd). 0 on parse error.
// Needed so we can hand zstd EXACTLY one frame -- in file (`-o`) mode zstd errors
// out and deletes its output when it sees the trailing Steam footer bytes.
inline size_t zstd_frame_len(const uint8_t* d,size_t n){
    if(n<6 || !(d[0]==0x28&&d[1]==0xb5&&d[2]==0x2f&&d[3]==0xfd)) return 0;
    size_t p=4; uint8_t fhd=d[p++];
    int fcs=(fhd>>6)&3, single=(fhd>>5)&1, checksum=(fhd>>2)&1, did=fhd&3;
    if(!single){ if(p>=n)return 0; p++; }                 // window descriptor
    p += (did==0?0:did==1?1:did==2?2:4);                  // dictionary id
    p += (fcs==0?(single?1:0):fcs==1?2:fcs==2?4:8);       // frame content size
    if(p>n) return 0;
    for(;;){                                              // block sequence
        if(p+3>n) return 0;
        uint32_t bh=d[p]|(d[p+1]<<8)|(d[p+2]<<16); p+=3;
        int last=bh&1, btype=(bh>>1)&3; uint32_t bsz=bh>>3;
        p += (btype==1?1:bsz);                            // RLE stores 1 byte, else bsz
        if(p>n) return 0;
        if(last) break;
    }
    if(checksum) p+=4;
    return p<=n ? p : 0;
}
// IN-PROCESS decoders (bundled: corpus/downloader/vendor/zstddeclib.c + LzmaDec.c).
// Shelling zstd/7za through the loader's CreateProcess surrogate is infeasible: the
// tools link msvcrt.dll (single shared instance) so their stdin fd 0 is cached before
// we can redirect it, and the surrogate gives each child a private vfs overlay that
// blocks file sharing. Decoding in-process sidesteps all of it -- no child at all.
extern "C" {
    size_t   ZSTD_decompress(void* dst, size_t dstCapacity, const void* src, size_t srcSize);
    unsigned ZSTD_isError(size_t code);
    size_t   ZSTD_findFrameCompressedSize(const void* src, size_t srcSize);   // exact frame length
    struct GhISzAlloc { void*(*Alloc)(const void*,size_t); void(*Free)(const void*,void*); };
    int LzmaDecode(unsigned char* dest, size_t* destLen, const unsigned char* src, size_t* srcLen,
                   const unsigned char* propData, unsigned propSize, int finishMode,
                   int* status, const GhISzAlloc* alloc);
}
inline void* gh_lzalloc(const void*, size_t s){ return malloc(s); }
inline void  gh_lzfree(const void*, void* a){ free(a); }
inline GhISzAlloc g_ghLzAlloc = { &gh_lzalloc, &gh_lzfree };
inline bool raw_inflate(const uint8_t* in,size_t inlen,size_t hint,Bytes& out){
    out.assign(hint?hint:inlen*8,0); z_stream s; memset(&s,0,sizeof(s)); if(inflateInit2(&s,-15)!=Z_OK)return false;
    s.next_in=(Bytef*)in; s.avail_in=(uInt)inlen; s.next_out=out.data(); s.avail_out=(uInt)out.size(); int r;
    do{ r=inflate(&s,Z_NO_FLUSH); if(s.avail_out==0&&r!=Z_STREAM_END){size_t u=out.size();out.resize(out.size()*2);s.next_out=out.data()+u;s.avail_out=(uInt)(out.size()-u);} }while(r==Z_OK);
    size_t prod=out.size()-s.avail_out; inflateEnd(&s); if(r!=Z_STREAM_END)return false; out.resize(prod); return true;
}
inline bool unzip_first(const Bytes& zip,Bytes& out){
    if(zip.size()<30||zip[0]!=0x50||zip[1]!=0x4b||zip[2]!=0x03||zip[3]!=0x04)return false;
    uint16_t method; memcpy(&method,&zip[8],2); uint32_t comp,uncomp; memcpy(&comp,&zip[18],4); memcpy(&uncomp,&zip[22],4);
    uint16_t fnl,exl; memcpy(&fnl,&zip[26],2); memcpy(&exl,&zip[28],2); size_t data=30+(size_t)fnl+exl;
    if(data+comp>zip.size())return false;
    if(method==0){out.assign(zip.begin()+data,zip.begin()+data+comp);return true;} if(method==8)return raw_inflate(&zip[data],comp,uncomp,out); return false;
}
// One-shot zstd of an exact [src,src+slen) slice. ZSTD_decompress accepts
// concatenated frames, which is what SteamKit/JavaSteam feed it (the whole
// VSZa middle, not a single hand-trimmed frame).
inline bool zstd_slice(const uint8_t* src,size_t slen,uint32_t want,Bytes& out){
    if(!src||slen==0||want==0) return false;
    out.resize(want);
    size_t r=ZSTD_decompress(out.data(),want,src,slen);
    return !ZSTD_isError(r) && r==want;
}
inline bool decode_chunk(const Bytes& enc,const uint8_t depotkey[32],uint32_t cb_original,Bytes& out){
    Bytes cont; if(!sym_decrypt(depotkey,enc,cont)) return false;
    auto mag4=[&](){ if(cont.size()<4) return std::string("n=")+std::to_string(cont.size()); char b[16]; sprintf(b,"%02x%02x%02x%02x",cont[0],cont[1],cont[2],cont[3]); return std::string(b); };
    if(cont.size()>=4 && cont[0]=='V'&&cont[1]=='S'&&cont[2]=='Z'&&cont[3]=='a'){   // zstd (SteamKit VZstdUtil)
        // Modern VSZa: 8-byte header (magic+crc32) + zstd + 15-byte footer ending "zsv".
        // Older LC-era chunks: 10-byte footer ending "zv". Feed the WHOLE middle to
        // zstd (concatenated frames); trimming to the first frame left 20XX.exe at 0 bytes.
        const size_t n=cont.size();
        bool zsv = n>=23 && cont[n-3]=='z' && cont[n-2]=='s' && cont[n-1]=='v';
        bool zv  = n>=14 && cont[n-2]=='z' && cont[n-1]=='v';
        if(zsv && zstd_slice(cont.data()+8, n-8-15, cb_original, out)) return true;
        if(zv  && zstd_slice(cont.data()+8, n-8-10, cb_original, out)) return true;
        if(zv  && zstd_slice(cont.data()+4, n-4-10, cb_original, out)) return true;
        size_t z=0; bool found=false;
        for(size_t i=0;i+4<=n;i++){ if(cont[i]==0x28&&cont[i+1]==0xb5&&cont[i+2]==0x2f&&cont[i+3]==0xfd){z=i;found=true;break;} }
        if(found){
            size_t end = zsv ? n-15 : (zv ? n-10 : n);
            if(z<end && zstd_slice(cont.data()+z, end-z, cb_original, out)) return true;
            size_t avail=n-z, flen=ZSTD_findFrameCompressedSize(cont.data()+z,avail);
            if(!ZSTD_isError(flen)&&flen>0&&flen<=avail && zstd_slice(cont.data()+z,flen,cb_original,out)) return true;
        }
        ghlog("zstd fail mag="+mag4()+" n="+std::to_string(n)+" want="+std::to_string(cb_original)+" zsv="+std::to_string(zsv));
        return false;
    }
    if(cont.size()>=4 && cont[0]==0x50&&cont[1]==0x4b&&cont[2]==0x03&&cont[3]==0x04) return unzip_first(cont,out);  // legacy ZIP/deflate
    if(cont.size()>=22 && cont[0]=='V'&&cont[1]=='Z'){                              // LZMA-alone
        out.resize(cb_original);
        size_t destLen=cb_original, srcLen=cont.size()-22;                          // body = cont[12 .. size-10)
        int status=0;
        int res=LzmaDecode(out.data(),&destLen, cont.data()+12,&srcLen, cont.data()+7,5, 1/*LZMA_FINISH_END*/, &status, &g_ghLzAlloc);
        if(res!=0 || destLen!=cb_original){ ghlog("lzma res="+std::to_string(res)+" dl="+std::to_string(destLen)+" want="+std::to_string(cb_original)); return false; }
        return true;
    }
    // Uncompressed chunk (decrypted payload IS the bytes). Some depot files store
    // already-packed PEs this way; treating them as unknown left a 0-byte dest.
    if(cb_original && cont.size()==cb_original){ out=cont; return true; }
    ghlog("decode unknown mag="+mag4()+" n="+std::to_string(cont.size())+" want="+std::to_string(cb_original));
    return false;
}

// ==================== Steam framing ====================
struct SteamMsg { uint32_t emsg=0; uint64_t steamid=0; int32_t sessionid=0; int32_t eresult=2; uint64_t jt=0,js=0; std::string errmsg,jobname; Bytes body; };
inline bool parse_frame(const uint8_t* d,size_t len,SteamMsg& o){
    if(len<4)return false; uint32_t raw; memcpy(&raw,d,4); bool proto=(raw&PROTO_MASK)!=0; o.emsg=raw&~PROTO_MASK;
    if(proto){ if(len<8)return false; uint32_t hl; memcpy(&hl,d+4,4); if(8+(size_t)hl>len)return false;
        PbReader hr(d+8,hl); uint32_t f,w; while(hr.next(f,w)){ if(f==1&&w==1)o.steamid=hr.fix64(); else if(f==2&&w==0)o.sessionid=(int32_t)hr.varint(); else if(f==10&&w==1)o.jt=hr.fix64(); else if(f==11&&w==1)o.js=hr.fix64(); else if(f==12&&w==2){Bytes b=hr.lendelim();o.jobname.assign(b.begin(),b.end());} else if(f==13&&w==0)o.eresult=(int32_t)hr.varint(); else if(f==14&&w==2){Bytes b=hr.lendelim();o.errmsg.assign(b.begin(),b.end());} else hr.skip(w);} o.body.assign(d+8+hl,d+len);
    } else { if(len<20)return false; o.body.assign(d+20,d+len); } return true;
}
inline bool gunzip(const Bytes& in,uint32_t osz,Bytes& out){
    out.assign(osz?osz:in.size()*4,0); z_stream s; memset(&s,0,sizeof(s)); if(inflateInit2(&s,15+32)!=Z_OK)return false;
    s.next_in=(Bytef*)in.data(); s.avail_in=(uInt)in.size(); s.next_out=out.data(); s.avail_out=(uInt)out.size();
    int r=inflate(&s,Z_FINISH); size_t prod=out.size()-s.avail_out; inflateEnd(&s); if(r!=Z_STREAM_END&&r!=Z_OK)return false; out.resize(prod); return true;
}
inline void collect(const uint8_t* d,size_t len,std::vector<SteamMsg>& q){
    SteamMsg m; if(!parse_frame(d,len,m))return;
    if(m.emsg==EMsg_Multi){ PbReader r(m.body.data(),m.body.size()); uint32_t f,w,osz=0; Bytes pl;
        while(r.next(f,w)){ if(f==1&&w==0)osz=(uint32_t)r.varint(); else if(f==2&&w==2)pl=r.lendelim(); else r.skip(w);}
        Bytes flat; if(osz>0){ if(!gunzip(pl,osz,flat))return; } else flat=pl;
        size_t off=0; while(off+4<=flat.size()){ uint32_t sz; memcpy(&sz,flat.data()+off,4); off+=4; if(off+sz>flat.size())break; collect(flat.data()+off,sz,q); off+=sz; }
    } else q.push_back(std::move(m));
}
inline Bytes build_proto_frame(uint32_t emsg,const Bytes& hdr,const Bytes& body){
    Bytes f; uint32_t e=emsg|PROTO_MASK; f.insert(f.end(),(uint8_t*)&e,(uint8_t*)&e+4);
    uint32_t hl=(uint32_t)hdr.size(); f.insert(f.end(),(uint8_t*)&hl,(uint8_t*)&hl+4);
    f.insert(f.end(),hdr.begin(),hdr.end()); f.insert(f.end(),body.begin(),body.end()); return f;
}
inline Bytes make_nonproto_hdr(uint32_t emsg){ Bytes h; h.insert(h.end(),(uint8_t*)&emsg,(uint8_t*)&emsg+4); int64_t n=-1; for(int i=0;i<2;i++)h.insert(h.end(),(uint8_t*)&n,(uint8_t*)&n+8); return h; }

inline bool recv_all(SOCKET s,uint8_t* b,size_t n){ size_t g=0; while(g<n){int r=recv(s,(char*)b+g,(int)(n-g),0); if(r<=0)return false; g+=r;} return true; }
inline bool send_all(SOCKET s,const uint8_t* b,size_t n){ size_t g=0; while(g<n){int r=send(s,(const char*)b+g,(int)(n-g),0); if(r<=0)return false; g+=r;} return true; }

// Forward decls: these are defined below CmClient but used by its PICS methods.
inline Bytes http_fetch(const std::wstring& host,const std::wstring& path);
inline std::wstring towide(const std::string& s);

// ==================== text VDF (Valve KeyValues) mini-parser ====================
// PICS ClientPICSProductInfoResponse delivers each app's appinfo as a TEXT VDF blob
// (JavaSteam PICSProductInfo.readAsText). We only need to walk it to depots -> <id> ->
// manifests -> <branch> -> gid, so a tiny ordered-tree parser suffices. Keys are
// case-insensitive (Steam semantics). Handles quoted/unquoted tokens, // comments,
// and \\ \" \n \t escapes inside quotes.
struct VdfNode {
    std::string value;                                        // leaf value ("" for objects)
    std::vector<std::pair<std::string,VdfNode>> kids;         // ordered subkeys
    const VdfNode* find(const char* k) const {
        for(auto& c:kids) if(_stricmp(c.first.c_str(),k)==0) return &c.second; return nullptr; }
};
struct VdfParser {
    const char* p; const char* e;
    VdfParser(const char* d,size_t n):p(d),e(d+n){}
    void ws(){ for(;;){ while(p<e&&(*p==' '||*p=='\t'||*p=='\r'||*p=='\n'))p++;
        if(p+1<e&&p[0]=='/'&&p[1]=='/'){ while(p<e&&*p!='\n')p++; } else break; } }
    bool tok(std::string& o){ ws(); if(p>=e||*p=='{'||*p=='}')return false; o.clear();
        if(*p=='"'){ p++; while(p<e&&*p!='"'){ if(*p=='\\'&&p+1<e){ p++; char c=*p++; o.push_back(c=='n'?'\n':c=='t'?'\t':c); } else o.push_back(*p++); } if(p<e)p++; return true; }
        while(p<e&&*p!=' '&&*p!='\t'&&*p!='\r'&&*p!='\n'&&*p!='{'&&*p!='}') o.push_back(*p++); return !o.empty(); }
    void obj(VdfNode& n){ for(;;){ ws(); if(p>=e)return; if(*p=='}'){p++;return;}
        std::string k; if(!tok(k)){ if(p<e&&*p=='}'){p++;} return; }
        ws(); if(p<e&&*p=='{'){ p++; VdfNode c; obj(c); n.kids.emplace_back(std::move(k),std::move(c)); }
        else { std::string v; tok(v); VdfNode c; c.value=std::move(v); n.kids.emplace_back(std::move(k),std::move(c)); } } }
};
struct DepotInfo { uint32_t depot=0; uint64_t manifest=0; std::string branch; uint32_t app=0; };   // branch the manifest came from; app = owning app for depotfromapp (0 = this app)
inline bool vdf_all_digits(const std::string& s){ if(s.empty())return false; for(char c:s) if(c<'0'||c>'9')return false; return true; }
// Resolve the downloadable depots (numeric depot id + public-branch manifest gid) from
// an appinfo VDF. Skips: named sub-sections (branches/baselanguages/...), depots with an
// oslist that excludes windows, other-language depots (config.language set and not english)
// and lowviolence depots. A depotfromapp depot comes back with manifest=0 and app=<owner>:
// its manifest, key and manifest code live under the owning app (HL2 220 pulls the Source
// engine/episode depots this way), resolved by get_app_depots. DLC/unowned depots are left
// in -- get_depot_key filters them at download time (no key => depot skipped).
inline void parse_app_depots_vdf(const Bytes& vdf,const std::string& branch,std::vector<DepotInfo>& out){
    VdfNode root; VdfParser((const char*)vdf.data(),vdf.size()).obj(root);
    if(root.kids.empty())return; const VdfNode* app=&root.kids[0].second;
    const VdfNode* depots=app->find("depots"); if(!depots)return;
    for(auto& kv:depots->kids){ if(!vdf_all_digits(kv.first))continue; const VdfNode& d=kv.second;
        const VdfNode* cfg=d.find("config");
        if(cfg){ const VdfNode* os=cfg->find("oslist"); if(os&&!os->value.empty()&&os->value.find("windows")==std::string::npos) continue;
            const VdfNode* lang=cfg->find("language"); if(lang&&!lang->value.empty()&&_stricmp(lang->value.c_str(),"english")!=0) continue;
            const VdfNode* lv=cfg->find("lowviolence"); if(lv&&lv->value=="1") continue; }
        if(const VdfNode* fa=d.find("depotfromapp")){
            DepotInfo di; di.depot=(uint32_t)strtoul(kv.first.c_str(),0,10); di.app=(uint32_t)strtoul(fa->value.c_str(),0,10);
            if(di.depot&&di.app) out.push_back(di); continue; }
        const VdfNode* mans=d.find("manifests"); if(!mans)continue;
        // A beta lists only the depots it changes; the rest come from public.
        std::string from=branch; const VdfNode* pub=mans->find(branch.c_str());
        if(!pub&&branch!="public"){ pub=mans->find("public"); from="public"; }
        if(!pub)continue;
        std::string gid; if(!pub->kids.empty()){ const VdfNode* g=pub->find("gid"); if(g)gid=g->value; } else gid=pub->value;
        if(gid.empty())continue;
        DepotInfo di; di.depot=(uint32_t)strtoul(kv.first.c_str(),0,10); di.manifest=strtoull(gid.c_str(),0,10); di.branch=from;
        if(di.depot&&di.manifest) out.push_back(di); }
}

struct CmClient {
    SOCKET s=INVALID_SOCKET; uint8_t key[32]={0},hmac[16]={0}; bool secured=false;
    uint64_t steamid=ANON_STEAMID; int32_t sessionid=0; uint64_t jobid=1;
    std::vector<uint32_t> licenses; bool want_licenses=false;   // captured during token logon
    bool recv_message(Bytes& out){ uint8_t hd[8]; if(!recv_all(s,hd,8))return false; uint32_t len; memcpy(&len,hd,4); if(memcmp(hd+4,"VT01",4))return false; out.resize(len); return len?recv_all(s,out.data(),len):true; }
    bool send_message(const Bytes& m){ uint8_t pre[8]; uint32_t len=(uint32_t)m.size(); memcpy(pre,&len,4); memcpy(pre+4,"VT01",4); return send_all(s,pre,8)&&send_all(s,m.data(),m.size()); }
    bool send_enc(const Bytes& p){ if(!secured)return send_message(p); Bytes e; if(!sym_encrypt_hmac(key,hmac,p,e))return false; return send_message(e); }
    bool recv_dec(Bytes& p){ Bytes r; if(!recv_message(r))return false; if(!secured){p=std::move(r);return true;} return sym_decrypt_hmac(key,hmac,r,p); }
    Bytes header(uint64_t jid=0xFFFFFFFFFFFFFFFFull,const std::string& method=""){
        // jobid_source is field 10 in this build (confirmed: content-service um() works with 10,
        // dies with 11). The response echoes it back as jobid_target in field 11.
        Bytes h; pb_fix64(h,1,steamid); if(sessionid)pb_u32(h,2,(uint32_t)sessionid);
        if(jid!=0xFFFFFFFFFFFFFFFFull)pb_fix64(h,10,jid); if(!method.empty())pb_str(h,12,method); return h; }
    // er (optional) receives the response's EResult.
    bool um(const std::string& method,const Bytes& body,Bytes& respbody,int32_t* er=nullptr){
        uint64_t myjid=jobid;
        Bytes f=build_proto_frame(EMsg_ServiceMethodCallFromClient,header(jobid++,method),body); if(!send_enc(f))return false;
        // Read past the post-login flood (hundreds of EMsg 766 persona-state updates for the
        // account's friends) until OUR response arrives (jobid_target echoes our jobid_source).
        // Time-bounded rather than message-count-bounded: a big friends list otherwise buries
        // the ServiceMethodResponse past any fixed count.
        bool have=false; SteamMsg best; DWORD start=GetTickCount(); int reads=0;
        while(!have && GetTickCount()-start<30000){ Bytes raw; if(!recv_dec(raw)){ ghlog("um recv-fail wsa="+std::to_string(WSAGetLastError())+" reads="+std::to_string(reads)); break; } reads++; std::vector<SteamMsg> q; collect(raw.data(),raw.size(),q);
            for(auto&m:q){ if(m.emsg==EMsg_ServiceMethodResponse){
                if(m.jt==myjid || m.js==myjid){ best=m; have=true;
                    if(m.eresult!=1) ghlog("um "+method+" er="+std::to_string(m.eresult)+(m.errmsg.empty()?"":" "+m.errmsg));
                    break; }
                if(best.emsg==0){ best=m; }   // fallback: first seen
            } }
            (void)reads; }
        if(best.emsg==0) return false;
        if(er) *er=best.eresult;
        respbody=best.body; return true; }

    // ---- QR device auth over this connection (no steamclient engine) ----
    // The same two calls the shim makes through the engine (corpus/shims/gh_steam_qr.h),
    // and like the engine they need login_anon() first: the TCP CM closes the socket on
    // a non-authed call (EMsg 9804) sent with no logon, with or without a steamid in the
    // header (cm_qr_test modes none/zero, 2026-10-18).
    // Only the user scans the challenge URL. The refresh token qr_poll may return is
    // a SECRET: callers persist it and scrub it, and nothing here logs it.
    struct QrSession { uint64_t client_id=0; Bytes request_id; std::string url; float interval=5.f; };
    int32_t qr_begin(const std::string& device,QrSession& qs){
        Bytes dd; pb_str(dd,1,device); pb_u32(dd,2,1);              // device_details: name, platform SteamClient
        Bytes body; pb_str(body,1,device); pb_u32(body,2,1);         // device_friendly_name, platform_type
        pb_bytes(body,3,dd); pb_str(body,4,"Client");                // device_details, website_id
        Bytes r; int32_t er=2;
        if(!um("Authentication.BeginAuthSessionViaQR#1",body,r,&er)) return -1;
        if(er!=1) return er;
        PbReader pr(r.data(),r.size()); uint32_t f,w;
        while(pr.next(f,w)){
            if(f==1&&w==0) qs.client_id=pr.varint();
            else if(f==2&&w==2){ Bytes b=pr.lendelim(); qs.url.assign(b.begin(),b.end()); }
            else if(f==3&&w==2) qs.request_id=pr.lendelim();
            else if(f==4&&w==5){ uint32_t v=0; for(int i=0;i<4&&pr.p<pr.end;i++) v|=(uint32_t)(*pr.p++)<<(8*i); memcpy(&qs.interval,&v,4); }
            else pr.skip(w);
        }
        return (qs.client_id && !qs.url.empty()) ? 1 : -2;
    }
    // One poll. Follows challenge rotation in place (qs.client_id / qs.url change;
    // rotated reports it). Returns the EResult; token/account are set only once
    // the user has approved on their phone.
    int32_t qr_poll(QrSession& qs,bool& remote,bool& rotated,std::string& token,std::string& account){
        remote=false; rotated=false;
        Bytes body; pb_u64(body,1,qs.client_id); pb_bytes(body,2,qs.request_id);
        Bytes r; int32_t er=2;
        if(!um("Authentication.PollAuthSessionStatus#1",body,r,&er)) return -1;
        if(er!=1) return er;
        PbReader pr(r.data(),r.size()); uint32_t f,w;
        while(pr.next(f,w)){
            if(f==1&&w==0){ uint64_t v=pr.varint(); if(v&&v!=qs.client_id){ qs.client_id=v; rotated=true; } }
            else if(f==2&&w==2){ Bytes b=pr.lendelim(); if(b.size()>1){ qs.url.assign(b.begin(),b.end()); rotated=true; } }
            else if(f==3&&w==2){ Bytes b=pr.lendelim(); token.assign(b.begin(),b.end()); }   // refresh_token (SECRET)
            else if(f==5&&w==0) remote=pr.varint()!=0;
            else if(f==6&&w==2){ Bytes b=pr.lendelim(); account.assign(b.begin(),b.end()); }
            else pr.skip(w);
        }
        return 1;
    }
    bool connect_and_secure(){
        WSADATA wsa; WSAStartup(MAKEWORD(2,2),&wsa); Bytes jb;
        { HINTERNET hS=WinHttpOpen(L"gh",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,0,0,0); HINTERNET hC=WinHttpConnect(hS,L"api.steampowered.com",443,0);
          HINTERNET hR=WinHttpOpenRequest(hC,L"GET",L"/ISteamDirectory/GetCMList/v1/?cellid=0&format=json",NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
          DWORD sf=SECURITY_FLAG_IGNORE_UNKNOWN_CA|SECURITY_FLAG_IGNORE_CERT_CN_INVALID|SECURITY_FLAG_IGNORE_CERT_DATE_INVALID|SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE; WinHttpSetOption(hR,WINHTTP_OPTION_SECURITY_FLAGS,&sf,sizeof(sf));
          if(WinHttpSendRequest(hR,0,0,0,0,0,0)&&WinHttpReceiveResponse(hR,NULL)){ DWORD a; do{a=0;WinHttpQueryDataAvailable(hR,&a); if(a){Bytes c(a);DWORD g;WinHttpReadData(hR,c.data(),a,&g);jb.insert(jb.end(),c.begin(),c.begin()+g);}}while(a>0);}
          WinHttpCloseHandle(hR);WinHttpCloseHandle(hC);WinHttpCloseHandle(hS); }
        std::string j(jb.begin(),jb.end()); size_t k=j.find("\"serverlist\""); if(k==std::string::npos)return false;
        size_t br=j.find('[',k),q1=j.find('"',br),q2=j.find('"',q1+1); std::string cm=j.substr(q1+1,q2-q1-1);
        std::string host=cm.substr(0,cm.find(':')),port=cm.substr(cm.find(':')+1);
        struct addrinfo hints{},*res=0; hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
        if(getaddrinfo(host.c_str(),port.c_str(),&hints,&res)!=0||!res)return false;
        s=socket(res->ai_family,res->ai_socktype,res->ai_protocol);
        if(::connect(s,res->ai_addr,(int)res->ai_addrlen)!=0)return false; freeaddrinfo(res);
        { DWORD tmo=25000; setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(char*)&tmo,sizeof(tmo)); }  // don't block forever past the flood
        Bytes raw; if(!recv_dec(raw))return false; SteamMsg m; parse_frame(raw.data(),raw.size(),m);
        if(m.emsg!=EMsg_ChannelEncryptRequest)return false; Bytes challenge(m.body.begin()+8,m.body.end());
        uint8_t sk[32]; gen_random(sk,32); Bytes oaep(sk,sk+32); oaep.insert(oaep.end(),challenge.begin(),challenge.end());
        Bytes enc; if(!rsa_oaep_encrypt(oaep,enc))return false; uint32_t crc=(uint32_t)crc32(0,enc.data(),(uInt)enc.size());
        Bytes body; uint32_t one=1,ks=128,zero=0; body.insert(body.end(),(uint8_t*)&one,(uint8_t*)&one+4); body.insert(body.end(),(uint8_t*)&ks,(uint8_t*)&ks+4);
        Bytes k128=enc; k128.resize(128,0); body.insert(body.end(),k128.begin(),k128.end());
        body.insert(body.end(),(uint8_t*)&crc,(uint8_t*)&crc+4); body.insert(body.end(),(uint8_t*)&zero,(uint8_t*)&zero+4);
        Bytes resp=make_nonproto_hdr(EMsg_ChannelEncryptResponse); resp.insert(resp.end(),body.begin(),body.end());
        if(!send_message(resp))return false;
        Bytes r2; if(!recv_dec(r2))return false; SteamMsg m2; parse_frame(r2.data(),r2.size(),m2);
        uint32_t er=0; memcpy(&er,m2.body.data(),4); if(m2.emsg!=EMsg_ChannelEncryptResult||er!=1)return false;
        memcpy(key,sk,32); memcpy(hmac,sk,16); secured=true; return true;
    }
    void parse_licenses(const Bytes& body){
        PbReader r(body.data(),body.size()); uint32_t f,w;
        while(r.next(f,w)){ if(w==2&&(f==2||f==3)){ Bytes lic=r.lendelim(); PbReader lr(lic.data(),lic.size()); uint32_t lf,lw; uint32_t pkg=0;
                while(lr.next(lf,lw)){ if(lw==0){ uint64_t v=lr.varint(); if(lf==1)pkg=(uint32_t)v; } else lr.skip(lw); }
                if(pkg)licenses.push_back(pkg);
            } else r.skip(w); }
    }
    bool await_logon(){
        bool logged=false, ok=false; int after=0;
        for(int i=0;i<300;i++){ Bytes raw; if(!recv_dec(raw))break; std::vector<SteamMsg> q; collect(raw.data(),raw.size(),q);
            for(auto&m:q){
                if(m.emsg==EMsg_ClientLogOnResponse){ if(m.steamid)steamid=m.steamid; sessionid=m.sessionid;   // don't clobber our sid with a 0 header
                    PbReader r(m.body.data(),m.body.size()); uint32_t f,w; int32_t er=0; while(r.next(f,w)){if(f==1&&w==0)er=(int32_t)r.varint();else r.skip(w);} logged=true; ok=(er==1);
                    ghlog("logon-resp er="+std::to_string(er)+" cm.steamid="+std::to_string(m.steamid)+" sess="+std::to_string(m.sessionid)); }
                else if(m.emsg==EMsg_ClientLicenseList){ parse_licenses(m.body); }
            }
            if(logged){ if(!ok)return false; if(!want_licenses||!licenses.empty())break; if(++after>=60)break; }   // read on for the license list (token login only)
        }
        return ok;
    }
    bool login_anon(){ Bytes body; pb_u32(body,1,65580); pb_u32(body,5,1561159470); if(!send_enc(build_proto_frame(EMsg_ClientLogon,header(),body)))return false; return await_logon(); }
    // Token logon. account_name (field 50) is REQUIRED for a full USER session — without it the
    // CM auths a degraded session and Player.* service calls return InvalidSteamID (JavaSteam
    // always sets accountName + accessToken). Also send client_os_type + machine fields.
    bool login_token(uint64_t sid,const std::string& token,const std::string& account=""){
        steamid=sid; want_licenses=true; Bytes body;
        pb_u32(body,1,65580);                       // protocol_version
        pb_u32(body,5,1561159470);                  // client_package_version
        pb_u32(body,7,20);                          // client_os_type (Windows)
        pb_bool(body,8,true);                        // should_remember_password
        if(!account.empty()) pb_str(body,50,account); // account_name  <-- the fix
        pb_bool(body,102,true);                      // supports_rate_limit_response
        pb_str(body,108,token);                      // access_token (refresh token)
        if(!send_enc(build_proto_frame(EMsg_ClientLogon,header(),body)))return false; return await_logon(); }
    bool get_depot_key(uint32_t app,uint32_t depot,Bytes& out){
        Bytes body; pb_u32(body,1,depot); pb_u32(body,2,app); if(!send_enc(build_proto_frame(EMsg_ClientGetDepotDecryptionKey,header(jobid++),body)))return false;
        for(int i=0;i<60;i++){ Bytes raw; if(!recv_dec(raw))return false; std::vector<SteamMsg> q; collect(raw.data(),raw.size(),q);
            for(auto&m:q) if(m.emsg==EMsg_ClientGetDepotDecryptionKeyResponse){ PbReader r(m.body.data(),m.body.size()); uint32_t f,w; int32_t er=0; Bytes key;
                while(r.next(f,w)){if(f==1&&w==0)er=(int32_t)r.varint();else if(f==3&&w==2)key=r.lendelim();else r.skip(w);} if(er!=1)return false; out=key; return true; } } return false;
    }
    bool get_content_servers(std::vector<std::string>& out){
        Bytes body; pb_u32(body,1,0); pb_u32(body,2,20); Bytes resp; if(!um("ContentServerDirectory.GetServersForSteamPipe#1",body,resp))return false;
        PbReader r(resp.data(),resp.size()); uint32_t f,w;
        while(r.next(f,w)){ if(f==1&&w==2){ Bytes srv=r.lendelim(); PbReader sr(srv.data(),srv.size()); uint32_t sf,sw; std::string host;
            while(sr.next(sf,sw)){ if(sf==8&&sw==2){Bytes b=sr.lendelim();host.assign(b.begin(),b.end());} else sr.skip(sw);} if(!host.empty())out.push_back(host); } else r.skip(w); } return !out.empty();
    }
    bool get_manifest_code(uint32_t app,uint32_t depot,uint64_t manifest,const std::string& branch,uint64_t& out){
        Bytes body; pb_u32(body,1,app); pb_u32(body,2,depot); pb_u64(body,3,manifest); pb_str(body,4,branch); Bytes resp; if(!um("ContentServerDirectory.GetManifestRequestCode#1",body,resp))return false;
        PbReader r(resp.data(),resp.size()); uint32_t f,w; out=0; while(r.next(f,w)){if(f==1&&w==0)out=r.varint();else r.skip(w);} return out!=0;
    }
    // PICS access token for an app (CMsgClientPICSAccessTokenRequest, EMsg 8905->8906). Many
    // apps need this token in the ProductInfo request or they come back missing_token. Returns
    // true if a response arrived (tok=0 means the app is public / token denied -- still usable).
    bool pics_access_token(uint32_t app,uint64_t& tok){
        tok=0; Bytes body; pb_u32(body,2,app);                    // appids = repeated field 2
        if(!send_enc(build_proto_frame(EMsg_ClientPICSAccessTokenRequest,header(jobid++),body)))return false;
        DWORD start=GetTickCount();
        while(GetTickCount()-start<20000){ Bytes raw; if(!recv_dec(raw))return false; std::vector<SteamMsg> q; collect(raw.data(),raw.size(),q);
            for(auto&m:q) if(m.emsg==EMsg_ClientPICSAccessTokenResponse){ PbReader r(m.body.data(),m.body.size()); uint32_t f,w;
                while(r.next(f,w)){ if(f==3&&w==2){ Bytes at=r.lendelim(); PbReader ar(at.data(),at.size()); uint32_t af,aw; uint32_t aid=0; uint64_t t=0;
                        while(ar.next(af,aw)){ if(af==1&&aw==0)aid=(uint32_t)ar.varint(); else if(af==2&&aw==0)t=ar.varint(); else ar.skip(aw); }
                        if(aid==app)tok=t; }
                    else r.skip(w); }
                return true; } }
        return false;
    }
    // PICS product info for an app (EMsg 8903->8904) -> the appinfo TEXT VDF (vdf). Handles
    // both inline delivery (AppInfo.buffer, drop trailing NUL) and HTTP delivery for large apps
    // (http_host + sha -> https://host/appinfo/<app>/sha/<hex>.txt.gz, gzip'd text VDF).
    bool pics_product_info(uint32_t app,uint64_t tok,Bytes& vdf){
        Bytes ai; pb_u32(ai,1,app); if(tok)pb_u64(ai,2,tok);      // AppInfo{ appid=1, access_token=2 }
        Bytes body; pb_bytes(body,2,ai); pb_bool(body,3,false);   // apps = repeated field 2; meta_data_only=false
        if(!send_enc(build_proto_frame(EMsg_ClientPICSProductInfoRequest,header(jobid++),body)))return false;
        DWORD start=GetTickCount();
        while(GetTickCount()-start<30000){ Bytes raw; if(!recv_dec(raw))return false; std::vector<SteamMsg> q; collect(raw.data(),raw.size(),q);
            for(auto&m:q){ if(m.emsg!=EMsg_ClientPICSProductInfoResponse)continue;
                std::string httpHost; Bytes sha; uint32_t sz=0; PbReader r(m.body.data(),m.body.size()); uint32_t f,w;
                while(r.next(f,w)){ if(f==1&&w==2){ Bytes appi=r.lendelim(); PbReader ar(appi.data(),appi.size()); uint32_t af,aw; uint32_t aid=0; Bytes buf,sh; uint32_t s2=0;
                        while(ar.next(af,aw)){ if(af==1&&aw==0)aid=(uint32_t)ar.varint(); else if(af==4&&aw==2)sh=ar.lendelim(); else if(af==5&&aw==2)buf=ar.lendelim(); else if(af==7&&aw==0)s2=(uint32_t)ar.varint(); else ar.skip(aw); }
                        if(aid==app){ if(!buf.empty()){ if(buf.back()==0)buf.pop_back(); vdf=std::move(buf); return true; } sha=sh; sz=s2; } }
                    else if(f==8&&w==2){ Bytes b=r.lendelim(); httpHost.assign(b.begin(),b.end()); }
                    else r.skip(w); }
                if(!httpHost.empty()&&!sha.empty()){ char sh[64]={0}; for(size_t i=0;i<sha.size();i++)sprintf(sh+i*2,"%02x",sha[i]);
                    char path[128]; sprintf(path,"/appinfo/%u/sha/%s.txt.gz",app,sh);
                    Bytes gz=http_fetch(towide(httpHost),towide(path)); Bytes txt; if(!gz.empty()&&gunzip(gz,0,txt)){ vdf=std::move(txt); return true; }
                    ghlog("pics http-appinfo fetch failed host="+httpHost); }
            }
        }
        return false;
    }
    // Resolve an app's downloadable depots+public-branch manifests (access token -> product info -> VDF walk).
    bool get_app_depots(uint32_t app,const std::string& branch,std::vector<DepotInfo>& out){
        uint64_t tok=0; pics_access_token(app,tok);              // best-effort; 0 = public app
        Bytes vdf; if(!pics_product_info(app,tok,vdf)){ ghlog("pics no appinfo app="+std::to_string(app)); return false; }
        parse_app_depots_vdf(vdf,branch,out);
        // depotfromapp: take the manifest from the owning app's own depot entry (that app's
        // public branch -- our branch name means nothing there). One appinfo fetch per owner.
        std::vector<std::pair<uint32_t,std::vector<DepotInfo>>> owners;
        for(size_t i=0;i<out.size();){
            DepotInfo& di=out[i]; if(!di.app){ i++; continue; }
            std::vector<DepotInfo>* od=nullptr; for(auto& o:owners) if(o.first==di.app) od=&o.second;
            if(!od){ owners.emplace_back(di.app,std::vector<DepotInfo>()); od=&owners.back().second;
                uint64_t otok=0; pics_access_token(di.app,otok); Bytes ov;
                if(pics_product_info(di.app,otok,ov)) parse_app_depots_vdf(ov,"public",*od);
                else ghlog("pics no appinfo owner app="+std::to_string(di.app)); }
            bool found=false;
            for(auto& o:*od) if(o.depot==di.depot&&!o.app&&o.manifest){ di.manifest=o.manifest; di.branch=o.branch; found=true; break; }
            ghlog("pics depot "+std::to_string(di.depot)+" fromapp="+std::to_string(di.app)+(found?" manifest="+std::to_string(di.manifest):" UNRESOLVED"));
            if(found) i++; else out.erase(out.begin()+i); }
        ghlog("pics app="+std::to_string(app)+" depots="+std::to_string(out.size()));
        return !out.empty();
    }
    // Owned library via Player.GetOwnedGames#1 (needs a token login so the ServiceMethod
    // header carries our authenticated steamid+session -- the engine's own unified-message
    // path returns InvalidSteamID). include_appinfo=true -> names in the same response.
    // CPlayer_GetOwnedGames_Request { fixed64 steamid=1; bool include_appinfo=2;
    //   bool include_played_free_games=3; }  Response { repeated Game games=2 { int32 appid=1; string name=2 } }.
    bool list_owned_games(uint64_t sid,std::vector<std::pair<uint32_t,std::string>>& out){
        ghlog("list_owned licenses(pkgs)="+std::to_string(licenses.size())+" conn.steamid="+std::to_string(steamid));
        // Player.GetOwnedGames#1 (matches GameNative/JavaSteam): appid + name in one call.
        // CRITICAL: request-body steamid is `uint64` = VARINT (pb_u64), NOT fixed64 -- unlike the
        // CMsgProtoBufHeader steamid which IS fixed64. Sending fixed64 here is a wiretype mismatch
        // the service reads as steamid=0 -> InvalidSteamID(19). include_appinfo=2, played_free=3,
        // free_sub=5, extended_appinfo=8 (field 7 is `language`, a string).
        Bytes body; pb_u64(body,1,sid); pb_bool(body,2,true); pb_bool(body,3,true); pb_bool(body,5,true); pb_bool(body,8,true);
        Bytes resp; if(!um("Player.GetOwnedGames#1",body,resp))return false;
        PbReader r(resp.data(),resp.size()); uint32_t f,w;
        while(r.next(f,w)){
            if(f==2&&w==2){ Bytes g=r.lendelim(); PbReader gr(g.data(),g.size()); uint32_t gf,gw; uint32_t appid=0; std::string name;
                while(gr.next(gf,gw)){ if(gf==1&&gw==0)appid=(uint32_t)gr.varint(); else if(gf==2&&gw==2){Bytes b=gr.lendelim();name.assign(b.begin(),b.end());} else gr.skip(gw); }
                if(appid)out.push_back(std::make_pair(appid,name)); }
            else r.skip(w);
        }
        ghlog("list_owned games="+std::to_string(out.size()));
        return true;
    }
};

// One-shot: connect + token-login + enumerate owned games. Returns 0 on success (out filled),
// negative on failure. The token stays local; only public appids+names come back.
inline int ListOwnedGames(uint64_t sid,const std::string& token,const std::string& account,std::vector<std::pair<uint32_t,std::string>>& out){
    if(!sid||token.empty())return -10;
    CmClient c; if(!c.connect_and_secure())return -1;
    if(!c.login_token(sid,token,account)){ if(c.s!=INVALID_SOCKET)closesocket(c.s); return -2; }
    bool ok=c.list_owned_games(sid,out); closesocket(c.s); return ok?0:-3;
}

inline Bytes http_fetch(const std::wstring& host,const std::wstring& path){
    Bytes out; HINTERNET hS=WinHttpOpen(L"gh",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,0,0,0); if(!hS)return out;
    // Bounded: a CDN host that accepts and then stalls must fail over, not hold
    // the caller for the WinHTTP defaults (GMod workshop item 2 sat 4+ min at 0%).
    WinHttpSetTimeouts(hS,15000,15000,30000,60000);
    HINTERNET hC=WinHttpConnect(hS,host.c_str(),443,0);
    if(hC){ HINTERNET hR=WinHttpOpenRequest(hC,L"GET",path.c_str(),NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
        if(hR){ DWORD sf=SECURITY_FLAG_IGNORE_UNKNOWN_CA|SECURITY_FLAG_IGNORE_CERT_CN_INVALID|SECURITY_FLAG_IGNORE_CERT_DATE_INVALID|SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE; WinHttpSetOption(hR,WINHTTP_OPTION_SECURITY_FLAGS,&sf,sizeof(sf));
            DWORD st=0,sl=sizeof(st);
            if(WinHttpSendRequest(hR,0,0,0,0,0,0)&&WinHttpReceiveResponse(hR,NULL)&&
               WinHttpQueryHeaders(hR,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&st,&sl,WINHTTP_NO_HEADER_INDEX)&&
               st==200){
                // ReadData-until-0: QueryDataAvailable can return 0 early and truncate big chunks.
                uint8_t buf[16384]; DWORD got=0;
                while(WinHttpReadData(hR,buf,sizeof(buf),&got)&&got>0) out.insert(out.end(),buf,buf+got);
            }
            WinHttpCloseHandle(hR);} WinHttpCloseHandle(hC);} WinHttpCloseHandle(hS); return out;
}
// 2 tries per CDN host, then the next host, 8 tries in all. Empty body =
// timeout / non-200 / connect fail / cancelled.
inline Bytes http_fetch_retry(const std::vector<std::string>& servers, const char* path){
    if(!path || servers.empty()) return Bytes();
    std::wstring wp=towide(path);
    int tries=0;
    for(const auto& host: servers){
        if(host.empty()) continue;
        std::wstring wh=towide(host);
        for(int a=0;a<2;a++){
            if(cancelled() || tries++>=8) return Bytes();
            Bytes b=http_fetch(wh,wp);
            if(!b.empty()) return b;
        }
    }
    return Bytes();
}
inline uint32_t adler32_buf(const uint8_t* d,size_t n){
    uint32_t a=1,b=0;
    for(size_t i=0;i<n;i++){ a=(a+d[i])%65521; b=(b+a)%65521; }
    return (b<<16)|a;
}
struct Chunk { Bytes sha; uint64_t offset=0; uint32_t cb_original=0,cb_compressed=0,crc=0; };
struct FileEntry { std::string name; uint64_t size=0; uint32_t flags=0; std::string linktarget; Bytes sha_content; std::vector<Chunk> chunks; };
inline bool parse_manifest(const Bytes& man,std::vector<FileEntry>& files,bool& filenames_encrypted){
    filenames_encrypted=false; size_t off=0;
    while(off+4<=man.size()){
        uint32_t magic; memcpy(&magic,&man[off],4); off+=4; if(magic==0x32C415ABu)break;
        if(off+4>man.size())break; uint32_t seclen; memcpy(&seclen,&man[off],4); off+=4; if(off+seclen>man.size())break;
        if(magic==0x71F617D0u){ PbReader pr(&man[off],seclen); uint32_t f,w;
            while(pr.next(f,w)){ if(f==1&&w==2){ Bytes mp=pr.lendelim(); PbReader mr(mp.data(),mp.size()); uint32_t mf,mw; FileEntry fe;
                while(mr.next(mf,mw)){ if(mf==1&&mw==2){Bytes b=mr.lendelim();fe.name.assign(b.begin(),b.end());}
                    else if(mf==2&&mw==0)fe.size=mr.varint(); else if(mf==3&&mw==0)fe.flags=(uint32_t)mr.varint();
                    else if(mf==5&&mw==2)fe.sha_content=mr.lendelim();
                    else if(mf==7&&mw==2){Bytes b=mr.lendelim();fe.linktarget.assign(b.begin(),b.end());}
                    else if(mf==6&&mw==2){ Bytes ck=mr.lendelim(); PbReader cr(ck.data(),ck.size()); uint32_t cf,cw; Chunk c;
                        while(cr.next(cf,cw)){
                            if(cf==1&&cw==2)c.sha=cr.lendelim();
                            else if(cf==2&&cw==5){ if(cr.p+4<=cr.end){ memcpy(&c.crc,cr.p,4); cr.p+=4; } }
                            else if(cf==3&&cw==0)c.offset=cr.varint();
                            else if(cf==4&&cw==0)c.cb_original=(uint32_t)cr.varint();
                            else if(cf==5&&cw==0)c.cb_compressed=(uint32_t)cr.varint();
                            else cr.skip(cw);
                        } fe.chunks.push_back(c);
                    } else mr.skip(mw); } files.push_back(std::move(fe)); } else pr.skip(w); }
        } else if(magic==0x1F4812BEu){ PbReader pr(&man[off],seclen); uint32_t f,w; while(pr.next(f,w)){ if(f==4&&w==0)filenames_encrypted=pr.varint()!=0; else pr.skip(w); } }
        off+=seclen;
    }
    return !files.empty();
}
// Bytes already under a folder (resumed files need no new space).
inline uint64_t folder_bytes(const std::wstring& dir){
    uint64_t n=0; WIN32_FIND_DATAW fd; HANDLE f=FindFirstFileW((dir+L"\\*").c_str(),&fd);
    if(f==INVALID_HANDLE_VALUE) return 0;
    do { if(!wcscmp(fd.cFileName,L".")||!wcscmp(fd.cFileName,L"..")) continue;
        if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) n+=folder_bytes(dir+L"\\"+fd.cFileName);
        else n+=((uint64_t)fd.nFileSizeHigh<<32)|fd.nFileSizeLow; } while(FindNextFileW(f,&fd));
    FindClose(f); return n;
}
// Free space DownloadApp leaves on the games drive (Tsushima's spill file and the
// Workshop live there too).
inline uint64_t g_dlReserve = 2ull << 30;
// Depots DownloadApp leaves out (the title config's steamSkipDepots, set by the add-on).
inline std::vector<uint32_t> g_dlSkipDepots;
inline void mkdirs(const std::wstring& path){ std::wstring cur; for(size_t i=0;i<path.size();i++){ wchar_t c=path[i]; cur+=c; if(c==L'\\'||c==L'/') CreateDirectoryW(cur.c_str(),NULL); } }
inline std::wstring towide(const std::string& s){ int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,NULL,0); std::wstring w(n?n-1:0,0); if(n)MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,&w[0],n); return w; }
inline uint64_t existing_size(const std::wstring& path){
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&fad)) return (uint64_t)-1;
    ULARGE_INTEGER u; u.LowPart=fad.nFileSizeLow; u.HighPart=fad.nFileSizeHigh; return u.QuadPart;
}
// Same dest path can appear twice (two depots, or a stub + a real mapping). Keep
// the mapping with more chunks so a later empty CREATE_ALWAYS cannot wipe a PE.
inline void merge_file_entries(std::vector<FileEntry>& files){
    std::vector<FileEntry> out;
    for(auto& fe:files){
        std::string key=fe.name; for(char& ch:key){ if(ch=='/')ch='\\'; if(ch>='A'&&ch<='Z')ch=(char)(ch-'A'+'a'); }
        size_t hit=(size_t)-1;
        for(size_t i=0;i<out.size();i++){
            std::string k=out[i].name; for(char& ch:k){ if(ch=='/')ch='\\'; if(ch>='A'&&ch<='Z')ch=(char)(ch-'A'+'a'); }
            if(k==key){ hit=i; break; }
        }
        if(hit==(size_t)-1){ out.push_back(std::move(fe)); continue; }
        if(fe.chunks.size()>out[hit].chunks.size() || (fe.chunks.size()==out[hit].chunks.size() && fe.size>out[hit].size))
            out[hit]=std::move(fe);
    }
    files.swap(out);
}

typedef void(*ShaTickFn)(void* ctx, uint64_t hashed, uint64_t total);
inline bool sha1_handle_range(HANDLE h, uint64_t off, uint64_t len, uint8_t out[20],
                              ShaTickFn tick=nullptr, void* tctx=nullptr){
    if(h==INVALID_HANDLE_VALUE || !len) return false;
    BCRYPT_ALG_HANDLE hAlg=0; BCRYPT_HASH_HANDLE hH=0; bool ok=false;
    if(BCryptOpenAlgorithmProvider(&hAlg,BCRYPT_SHA1_ALGORITHM,NULL,0)!=GHDL_STATUS_SUCCESS) return false;
    if(BCryptCreateHash(hAlg,&hH,NULL,0,NULL,0,0)==GHDL_STATUS_SUCCESS){
        LARGE_INTEGER li; li.QuadPart=(LONGLONG)off;
        if(SetFilePointerEx(h,li,NULL,FILE_BEGIN)){
            uint8_t buf[1<<16];
            uint64_t left=len, since=0;
            bool fail=false;
            while(left){
                DWORD n=(DWORD)((left>sizeof(buf))?sizeof(buf):left);
                DWORD got=0;
                if(!ReadFile(h,buf,n,&got,NULL) || got!=n){ fail=true; break; }
                if(BCryptHashData(hH,buf,got,0)!=GHDL_STATUS_SUCCESS){ fail=true; break; }
                left-=got; since+=got;
                if(tick && since>=(8u<<20)){ tick(tctx, len-left, len); since=0; }
            }
            if(!fail && BCryptFinishHash(hH,out,20,0)==GHDL_STATUS_SUCCESS) ok=true;
        }
        BCryptDestroyHash(hH);
    }
    BCryptCloseAlgorithmProvider(hAlg,0);
    return ok;
}
inline bool sha1_path_range(const std::wstring& path, uint64_t off, uint64_t len, uint8_t out[20],
                            ShaTickFn tick=nullptr, void* tctx=nullptr){
    HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(h==INVALID_HANDLE_VALUE) return false;
    bool ok=sha1_handle_range(h,off,len,out,tick,tctx);
    CloseHandle(h);
    return ok;
}
inline void write_install_marker(const std::wstring& outdir, uint32_t app, bool complete){
    std::wstring dir=outdir;
    if(!dir.empty() && dir.back()!=L'\\' && dir.back()!=L'/') dir+=L'\\';
    std::wstring p=dir+L"gh_install.txt";
    char buf[80]; int n=sprintf(buf,"app=%u\ncomplete=%u\n",app, complete?1u:0u);
    HANDLE h=CreateFileW(p.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h!=INVALID_HANDLE_VALUE){ DWORD w=0; WriteFile(h,buf,(DWORD)n,&w,nullptr); CloseHandle(h); }
}

struct RepairCtx {
    uint32_t depot=0;
    const Bytes* key=nullptr;
    const std::vector<std::string>* servers=nullptr;
    bool verify=false;
    ProgressFn cb=nullptr;
    void* cbctx=nullptr;
    int totalChunks=0;
    int totalFiles=0;
    int* nchunks=nullptr;
    int* nfiles=nullptr;
    int* failures=nullptr;
    unsigned long long* totbytes=nullptr;
    bool* disk_full=nullptr;
};
inline int repair_pct(const RepairCtx& x){
    if(!x.nchunks || !x.totalChunks) return 99;
    int pct=2 + ((*x.nchunks)*97)/x.totalChunks;
    if(pct<2) pct=2; if(pct>99) pct=99;
    return pct;
}
inline void repair_cb(const RepairCtx& x, const char* ph){
    if(x.cb) x.cb(x.cbctx, ph, repair_pct(x), x.nfiles?*x.nfiles:0, x.totalFiles, x.totbytes?*x.totbytes:0);
}
inline void sha_tick_repair(void* p, uint64_t, uint64_t){
    RepairCtx* x=(RepairCtx*)p;
    repair_cb(*x, x->verify ? "checking files" : "downloading");
}
inline bool fetch_decode_chunk(uint32_t depot, const Bytes& key, const Chunk& ck,
                               const std::vector<std::string>& servers, Bytes& raw){
    if(key.size()!=32 || ck.sha.empty()) return false;
    char sh[64]={0}; for(size_t i=0;i<ck.sha.size() && i<20;i++) sprintf(sh+i*2,"%02x",ck.sha[i]);
    char cp[128]; sprintf(cp,"/depot/%u/chunk/%s",depot,sh);
    Bytes enc=http_fetch_retry(servers,cp);
    if(enc.empty()||!decode_chunk(enc,key.data(),ck.cb_original,raw)) return false;
    uint8_t dg[20]; bool shaOk=sha1_hash(raw,dg)&&ck.sha.size()==20&&memcmp(dg,ck.sha.data(),20)==0;
    uint32_t ad=adler32_buf(raw.data(),raw.size());
    bool adlerOk=ck.crc && ad==ck.crc;
    return shaOk || adlerOk;
}

// Resume: skip files whose size already matches. Partial / missing files keep
// good on-disk chunks (SHA1 of [offset, cb_original)) and only CDN-fetch the rest.
// Verify: hash size-matching files against sha_content (else every chunk) and
// repair mismatches in place. Never CREATE_ALWAYS — that discarded a partial
// multi-GB asset on the next pass.
inline bool write_or_repair_file(const FileEntry& fe, const std::wstring& full, RepairCtx& x, bool trace){
    if(x.disk_full && *x.disk_full) return false;
    uint64_t before=existing_size(full);
    bool missing=(before==(uint64_t)-1);
    uint64_t have=missing?0:before;
    const char* busy = x.verify ? "checking files" : "downloading";
    const char* fetch_ph = x.verify ? "repairing" : "downloading";

    if(!x.verify && fe.size && have==fe.size){
        if(x.nfiles) (*x.nfiles)++;
        if(x.totbytes) *x.totbytes += fe.size;
        if(x.nchunks) *x.nchunks += (int)fe.chunks.size();
        if(trace) trace_exe("SKIP complete");
        repair_cb(x, fetch_ph);
        return true;
    }
    if(x.verify && fe.size && have==fe.size && fe.sha_content.size()==20){
        repair_cb(x, busy);
        uint8_t dg[20];
        if(sha1_path_range(full,0,fe.size,dg,&sha_tick_repair,&x) && memcmp(dg,fe.sha_content.data(),20)==0){
            if(x.nfiles) (*x.nfiles)++;
            if(x.totbytes) *x.totbytes += fe.size;
            if(x.nchunks) *x.nchunks += (int)fe.chunks.size();
            if(trace) trace_exe("SKIP sha_content");
            repair_cb(x, busy);
            return true;
        }
        if(trace) trace_exe("sha_content mismatch, repairing");
        ghlog(std::string("verify mismatch ")+fe.name);
    }
    if(fe.chunks.empty()){
        ghlog(std::string("no-chunks ")+fe.name+" size="+std::to_string(fe.size));
        if(trace) trace_exe("SKIP no-chunks");
        return true;
    }

    HANDLE h=CreateFileW(full.c_str(),GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(h==INVALID_HANDLE_VALUE){
        DWORD err=GetLastError();
        if(x.failures) (*x.failures)++;
        if(trace) trace_exe(std::string("OPEN fail err=")+std::to_string(err));
        return false;
    }
    int file_fail=0;
    uint64_t wrote_new=0;
    for(size_t ci=0;ci<fe.chunks.size();ci++){
        if(x.disk_full && *x.disk_full) break;
        if(cancelled()){ file_fail++; if(x.failures) (*x.failures)++; break; }
        const Chunk& ck=fe.chunks[ci];
        uint32_t orig=ck.cb_original;
        bool on_disk = !missing && orig>0 && have>=ck.offset+(uint64_t)orig;
        bool chunk_ok=false;
        if(on_disk && ck.sha.size()==20){
            uint8_t dg[20];
            if(sha1_handle_range(h,ck.offset,orig,dg) && memcmp(dg,ck.sha.data(),20)==0) chunk_ok=true;
        }
        if(chunk_ok){
            if(x.nchunks) (*x.nchunks)++;
            if(x.totbytes) *x.totbytes += orig;
            if(trace) trace_exe(std::string("  SKIP chunk i=")+std::to_string(ci)+" off="+std::to_string(ck.offset));
            continue;
        }
        Bytes raw;
        if(!x.key || !x.servers || !fetch_decode_chunk(x.depot,*x.key,ck,*x.servers,raw)){
            if(x.failures) (*x.failures)++;
            file_fail++;
            if(x.nchunks) (*x.nchunks)++;
            ghlog(std::string("chunk fail ")+fe.name+" off="+std::to_string(ck.offset)+" want="+std::to_string(orig));
            if(trace) trace_exe(std::string("  CKfail i=")+std::to_string(ci)+" orig="+std::to_string(orig)+
                               " off="+std::to_string(ck.offset));
            continue;
        }
        LARGE_INTEGER li; li.QuadPart=(LONGLONG)ck.offset;
        SetFilePointerEx(h,li,NULL,FILE_BEGIN);
        DWORD w=0; BOOL wok=WriteFile(h,raw.data(),(DWORD)raw.size(),&w,NULL); DWORD werr=wok?0:GetLastError();
        if(x.totbytes) *x.totbytes += w;
        wrote_new+=w;
        if(have < ck.offset+(uint64_t)w) have=ck.offset+(uint64_t)w;
        if(x.nchunks) (*x.nchunks)++;
        if(trace) trace_exe(std::string("  CKok i=")+std::to_string(ci)+" raw="+std::to_string(raw.size())+
                            " w="+std::to_string(w)+" off="+std::to_string(ck.offset));
        if(!wok || w!=(DWORD)raw.size()){
            if(x.failures) (*x.failures)++;
            file_fail++;
            ghlog_always(std::string("write fail ")+fe.name+" err="+std::to_string(werr)+" w="+std::to_string(w)+" raw="+std::to_string(raw.size()));
            if(werr==ERROR_DISK_FULL || werr==39){
                if(x.disk_full) *x.disk_full=true;
                if(x.cb) x.cb(x.cbctx,"storage full",repair_pct(x),x.nfiles?*x.nfiles:0,x.totalFiles,x.totbytes?*x.totbytes:0);
            }
            break;
        }
        repair_cb(x, fetch_ph);
    }
    if(file_fail==0 && fe.size){
        LARGE_INTEGER end; end.QuadPart=(LONGLONG)fe.size;
        if(SetFilePointerEx(h,end,NULL,FILE_BEGIN)) SetEndOfFile(h);
    }
    CloseHandle(h);
    if(x.disk_full && *x.disk_full && wrote_new==0 && missing) DeleteFileW(full.c_str());
    if(trace) trace_exe(std::string("FILE done wrote_new=")+std::to_string(wrote_new)+" want="+std::to_string(fe.size)+
                        " fail="+std::to_string(file_fail));
    if(file_fail && fe.size && wrote_new<fe.size)
        ghlog(std::string("short file ")+fe.name+" wrote="+std::to_string(wrote_new)+" want="+std::to_string(fe.size));
    if(x.nfiles) (*x.nfiles)++;
    repair_cb(x, fetch_ph);
    return file_fail==0;
}

// One manifest into outdir over an already logged-on connection. The workshop sync
// fetches every subscribed item this way on a single logon (one logon per item
// would be dozens of logons a minute). depotkey/servers are the caller's, fetched
// once per depot. Same return contract as DownloadDepot; never closes c.
inline int DownloadManifestOn(CmClient& c,const Bytes& depotkey,const std::vector<std::string>& servers,
                              uint32_t app,uint32_t depot,uint64_t manifest,const std::wstring& outdir,
                              const std::string& branch,ProgressFn cb,void* ctx){
    #define GHDL_CB(ph,pc,fd,ft,by) do{ if(cb) cb(ctx,ph,pc,fd,ft,by); }while(0)
    GHDL_CB("fetching-manifest",2,0,0,0);
    uint64_t mcode; if(!c.get_manifest_code(app,depot,manifest,branch,mcode)){ GHDL_CB("error-manifest",0,0,0,0); return -5; }
    char mp[256]; sprintf(mp,"/depot/%u/manifest/%llu/5/%llu",depot,(unsigned long long)manifest,(unsigned long long)mcode);
    Bytes zip=http_fetch_retry(servers,mp); Bytes man; if(!unzip_first(zip,man)){ GHDL_CB("error-manifest-dl",0,0,0,0); return -6; }
    std::vector<FileEntry> files; bool fenc=false; if(!parse_manifest(man,files,fenc)){ GHDL_CB("error-parse",0,0,0,0); return -7; }
    if(fenc){ for(auto& fe:files){ Bytes p; if(sym_decrypt(depotkey.data(),b64decode(fe.name),p)){ fe.name.assign(p.begin(),p.end()); while(!fe.name.empty()&&fe.name.back()=='\0')fe.name.pop_back(); } } }
    int total_chunks=0; for(auto& fe:files) total_chunks+=(int)fe.chunks.size();
    GHDL_CB("downloading",2,0,(int)files.size(),0);
    mkdirs(outdir+L"\\");
    int nfiles=0,nchunks=0,failures=0; unsigned long long totbytes=0; bool disk_full=false;
    RepairCtx rx; rx.depot=depot; rx.key=&depotkey; rx.servers=&servers; rx.verify=false;
    rx.cb=cb; rx.cbctx=ctx; rx.totalChunks=total_chunks; rx.totalFiles=(int)files.size();
    rx.nchunks=&nchunks; rx.nfiles=&nfiles; rx.failures=&failures; rx.totbytes=&totbytes; rx.disk_full=&disk_full;
    for(auto& fe:files){
        std::string rel=fe.name; for(char& ch:rel) if(ch=='/')ch='\\';
        std::wstring full=outdir+L"\\"+towide(rel);
        if(fe.flags&FLAG_DIRECTORY){ mkdirs(full+L"\\"); continue; }
        if(fe.flags&FLAG_SYMLINK) continue;
        size_t sl=full.find_last_of(L"\\/"); if(sl!=std::wstring::npos) mkdirs(full.substr(0,sl+1));
        write_or_repair_file(fe,full,rx,false);
        if(disk_full) break;
        if(cancelled()){ if(!failures) failures=1; break; }
    }
    DeleteFileW(gh_tmp(L"_ghdl_in.zst").c_str()); DeleteFileW(gh_tmp(L"_ghdl_in.lzma").c_str());
    DeleteFileW(gh_tmp(L"_ghdl_out.bin").c_str());
    GHDL_CB(disk_full?"storage full":(failures?"error-chunks":"done"), disk_full?99:100, nfiles,(int)files.size(),totbytes);
    #undef GHDL_CB
    return disk_full ? (failures?failures:1) : failures;
}

// ==================== the library entry point ====================
// outdir = full destination path for the game tree (e.g. <LocalState>\game\<name>).
// token empty => anonymous logon. cb may be null. Returns 0 on success, >0 chunk
// failures, <0 on a setup failure (negated phase). progress phases:
//   connecting(0) logon(1) fetching-manifest(2) downloading(2..99) done(100)
inline int DownloadDepot(uint32_t app,uint32_t depot,uint64_t manifest,const std::wstring& outdir,
                         const std::string& branch,const std::string& token,uint64_t token_sid,
                         ProgressFn cb,void* ctx){
    #define GHDL_CB(ph,pc,fd,ft,by) do{ if(cb) cb(ctx,ph,pc,fd,ft,by); }while(0)
    GHDL_CB("connecting",0,0,0,0);
    CmClient c;
    if(!c.connect_and_secure()){ GHDL_CB("error-connect",0,0,0,0); return -1; }
    GHDL_CB("logon",1,0,0,0);
    bool ok = (!token.empty()&&token_sid) ? c.login_token(token_sid,token) : c.login_anon();
    if(!ok){ GHDL_CB("error-logon",0,0,0,0); closesocket(c.s); return -2; }
    Bytes depotkey; if(!c.get_depot_key(app,depot,depotkey)||depotkey.size()!=32){ GHDL_CB("error-depotkey",0,0,0,0); closesocket(c.s); return -3; }
    std::vector<std::string> servers; if(!c.get_content_servers(servers)){ GHDL_CB("error-servers",0,0,0,0); closesocket(c.s); return -4; }
    #undef GHDL_CB
    int rc=DownloadManifestOn(c,depotkey,servers,app,depot,manifest,outdir,branch,cb,ctx);
    closesocket(c.s); WSACleanup();
    return rc;
}

// ==================== whole-app download (appid -> all depots) ====================
// Resolves an app's depots via PICS (no pre-staged depot/manifest), then downloads every
// windows-branch depot the account can obtain a key for, into <outdir> as one game tree.
// Reuses ONE CM connection for all depots. Two passes: (1) fetch each depot's key+manifest
// to count total chunks so the progress bar is accurate; (2) fetch/decode/verify/write all
// chunks. verify=true hashes size-matching files (Steam "verify integrity") and repairs
// mismatches in place; verify=false skips size-matching files and only fills holes
// (resume). Requires a token login (owned depots need the user's session). Returns 0 on
// full success, >0 = chunk failures, <0 = a setup failure (negated phase).
inline int DownloadApp(uint32_t app,const std::wstring& outdir,const std::string& branch,
                       const std::string& token,uint64_t token_sid,const std::string& account,
                       ProgressFn cb,void* ctx,bool verify=false){
    #define GHDL_CB(ph,pc,fd,ft,by) do{ if(cb) cb(ctx,ph,pc,fd,ft,by); }while(0)
    GHDL_CB("connecting",0,0,0,0);
    trace_exe_reset();
    { std::string dest(outdir.begin(), outdir.end());
      trace_exe(std::string("BEGIN dest=")+dest+" app="+std::to_string(app)+" verify="+std::to_string((int)verify)); }
    CmClient c;
    if(!c.connect_and_secure()){ GHDL_CB("error-connect",0,0,0,0); trace_exe("END rc=-1 connect"); return -1; }
    GHDL_CB("signing in",1,0,0,0);
    bool ok = (!token.empty()&&token_sid) ? c.login_token(token_sid,token,account) : c.login_anon();
    if(!ok){ GHDL_CB("error-logon",0,0,0,0); closesocket(c.s); trace_exe("END rc=-2 logon"); return -2; }
    GHDL_CB("finding files",1,0,0,0);
    std::vector<DepotInfo> depots;
    if(!c.get_app_depots(app,branch,depots)){ GHDL_CB("error-appinfo",0,0,0,0); closesocket(c.s); return -3; }
    std::vector<std::string> servers; if(!c.get_content_servers(servers)){ GHDL_CB("error-servers",0,0,0,0); closesocket(c.s); return -4; }

    // Pass 1: per-depot key + manifest; build the work list and count chunks.
    struct DW { uint32_t depot; Bytes key; std::vector<FileEntry> files; };
    std::vector<DW> work; int totalChunks=0,totalFiles=0;
    for(auto& d:depots){
        if(std::find(g_dlSkipDepots.begin(),g_dlSkipDepots.end(),d.depot)!=g_dlSkipDepots.end()){
            ghlog_always("depot "+std::to_string(d.depot)+" skipped (title config)"); continue; }
        uint32_t kapp=d.app?d.app:app;   // depotfromapp: key + manifest code belong to the owner
        Bytes key; if(!c.get_depot_key(kapp,d.depot,key)||key.size()!=32){ ghlog("skip depot(no key) "+std::to_string(d.depot)); continue; }
        uint64_t mcode; if(!c.get_manifest_code(kapp,d.depot,d.manifest,d.branch.empty()?branch:d.branch,mcode)){ ghlog("skip depot(no mcode) "+std::to_string(d.depot)); continue; }
        char mp[256]; sprintf(mp,"/depot/%u/manifest/%llu/5/%llu",d.depot,(unsigned long long)d.manifest,(unsigned long long)mcode);
        Bytes zip=http_fetch_retry(servers,mp); Bytes man; if(!unzip_first(zip,man)){ ghlog("skip depot(no manifest) "+std::to_string(d.depot)); continue; }
        std::vector<FileEntry> files; bool fenc=false; if(!parse_manifest(man,files,fenc)) continue;
        if(fenc){ for(auto& fe:files){ Bytes p; if(sym_decrypt(key.data(),b64decode(fe.name),p)){ fe.name.assign(p.begin(),p.end()); while(!fe.name.empty()&&fe.name.back()=='\0')fe.name.pop_back(); } } }
        merge_file_entries(files);
        int empty=0, exeChunks=-1; uint64_t exeSz=0;
        for(auto& fe:files){
            totalChunks+=(int)fe.chunks.size();
            if(fe.size && fe.chunks.empty() && !(fe.flags&FLAG_DIRECTORY)) empty++;
            if(is_20xx_exe(fe.name)){
                exeChunks=(int)fe.chunks.size(); exeSz=fe.size;
                trace_exe(std::string("MANIFEST depot=")+std::to_string(d.depot)+" name="+fe.name+
                          " size="+std::to_string(fe.size)+" flags="+std::to_string(fe.flags)+
                          " nchunks="+std::to_string(fe.chunks.size()));
                for(size_t i=0;i<fe.chunks.size();i++){
                    const Chunk& ck=fe.chunks[i];
                    trace_exe(std::string("  CK ")+std::to_string(i)+
                              " sha="+hex_bytes(ck.sha.empty()?nullptr:ck.sha.data(),ck.sha.size())+
                              " off="+std::to_string(ck.offset)+
                              " orig="+std::to_string(ck.cb_original)+
                              " comp="+std::to_string(ck.cb_compressed)+
                              " crc="+std::to_string(ck.crc));
                }
            }
        }
        ghlog_always("depot "+std::to_string(d.depot)+" files="+std::to_string(files.size())+" chunks="+std::to_string(totalChunks)+" empty="+std::to_string(empty)+" exe.chunks="+std::to_string(exeChunks)+" exe.sz="+std::to_string(exeSz));
        totalFiles+=(int)files.size();
        DW dw; dw.depot=d.depot; dw.key=std::move(key); dw.files=std::move(files); work.push_back(std::move(dw));
        GHDL_CB("finding files",2,0,totalFiles,0);
    }
    if(work.empty()){ GHDL_CB("error-nodepots",0,0,0,0); closesocket(c.s); trace_exe("END rc=-8 nodepots"); return -8; }

    // Space check before writing anything: the whole app must fit, less what a resume
    // already has on disk, with g_dlReserve left over. Each depot's size is logged.
    { uint64_t want=0;
      for(auto& dw:work){ uint64_t b=0; for(auto& fe:dw.files) if(!(fe.flags&FLAG_DIRECTORY)) b+=fe.size;
          ghlog_always("depot "+std::to_string(dw.depot)+" bytes="+std::to_string(b)+" ("+std::to_string(b>>20)+" MB)"); want+=b; }
      const uint64_t have=folder_bytes(outdir); const uint64_t need=want>have?want-have:0;
      std::wstring root=outdir; size_t cut=root.find_last_of(L"\\/"); if(cut!=std::wstring::npos) root.resize(cut+1);
      ULARGE_INTEGER fr{}; const bool haveFree=GetDiskFreeSpaceExW(root.c_str(),&fr,nullptr,nullptr)!=0;
      ghlog_always("space app="+std::to_string(app)+" want="+std::to_string(want>>20)+"MB have="+std::to_string(have>>20)+
                   "MB need="+std::to_string(need>>20)+"MB free="+(haveFree?std::to_string(fr.QuadPart>>20)+"MB":std::string("?"))+
                   " reserve="+std::to_string(g_dlReserve>>20)+"MB");
      if(haveFree && need+g_dlReserve>fr.QuadPart){
          GHDL_CB("storage full",0,0,totalFiles,need); closesocket(c.s); trace_exe("END rc=-9 space"); return -9; } }

    // Pass 2: download every chunk across all depots, with a single global counter.
    // Exes first: LocalState quota fills mid-walk (ERROR_DISK_FULL 112) and used to
    // leave 20XX.exe at 0 bytes while the bar still reached 100%.
    for(auto& dw:work){
        std::stable_sort(dw.files.begin(), dw.files.end(), [](const FileEntry& a, const FileEntry& b){
            int pa=file_write_prio(a.name), pb=file_write_prio(b.name);
            if(pa!=pb) return pa<pb;
            return a.name<b.name;
        });
    }
    GHDL_CB(verify?"checking files":"downloading",2,0,totalFiles,0);
    mkdirs(outdir+L"\\");
    write_install_marker(outdir, app, false);
    int nchunks=0,nfiles=0,failures=0; unsigned long long totbytes=0;
    bool disk_full=false;
    for(auto& dw:work){
        if(disk_full) break;
        RepairCtx rx; rx.depot=dw.depot; rx.key=&dw.key; rx.servers=&servers; rx.verify=verify;
        rx.cb=cb; rx.cbctx=ctx; rx.totalChunks=totalChunks; rx.totalFiles=totalFiles;
        rx.nchunks=&nchunks; rx.nfiles=&nfiles; rx.failures=&failures; rx.totbytes=&totbytes; rx.disk_full=&disk_full;
        for(auto& fe:dw.files){
            if(disk_full) break;
            std::string rel=fe.name; for(char& ch:rel) if(ch=='/')ch='\\';
            std::wstring full=outdir+L"\\"+towide(rel);
            if(fe.flags&FLAG_DIRECTORY){ mkdirs(full+L"\\"); continue; }
            if(fe.flags&FLAG_SYMLINK) continue;
            size_t sl=full.find_last_of(L"\\/"); if(sl!=std::wstring::npos) mkdirs(full.substr(0,sl+1));
            bool trace=is_20xx_exe(rel);
            if(trace){
                uint64_t before=existing_size(full);
                std::string dp(full.begin(), full.end());
                trace_exe(std::string("FILE dest=")+dp+" existing="+(before==(uint64_t)-1?"missing":std::to_string(before))+
                          " size="+std::to_string(fe.size)+" flags="+std::to_string(fe.flags)+
                          " nchunks="+std::to_string(fe.chunks.size()));
            }
            write_or_repair_file(fe,full,rx,trace);
        }
    }
    if(!disk_full && failures==0) write_install_marker(outdir, app, true);
    GHDL_CB(disk_full?"storage full":(failures?"error-chunks":"done"), disk_full?99:100, nfiles,totalFiles,totbytes);
    closesocket(c.s); WSACleanup();
    trace_exe(std::string("END rc=")+std::to_string(failures)+" nfiles="+std::to_string(nfiles)+" nchunks="+std::to_string(nchunks)+" disk_full="+std::to_string(disk_full)+" verify="+std::to_string((int)verify));
    #undef GHDL_CB
    return disk_full ? (failures?failures:1) : failures;
}

} // namespace ghdl
#endif // GH_CM_DOWNLOADER_H
