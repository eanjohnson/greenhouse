// cm_tcp_test.cpp -- B1b: standalone Steam CM client (TCP transport) in C++.
//
// Ports ValvePython/steam's proven TCP path (its only real transport): the WS
// hand-roll never worked (server closes after logon), so this is the validated
// route. Flow:
//   CM discovery (WebAPI GetCMList) -> TCP :27017 -> ChannelEncrypt handshake
//   (RSA-OAEP session key + AES-256 ECB-IV/CBC + HMAC-SHA1) -> anon ClientLogon
//   -> ClientGetDepotDecryptionKey -> print key.
// All symmetric crypto via BCrypt/CNG; crc32 + gzip via zlib. No steamclient engine.
//
// Build (cross, from repo root) -- see corpus/downloader/build_cm_test.sh
// Run:  cm_tcp_test.exe [depot=232256] [app=232250] [ip:port]
//   default target = TF2 dedicated-server depot (anon-accessible): expect eresult=1, 32-byte key.

#define WIN32_LEAN_AND_MEAN
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
#include "zlib.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0)
#endif

typedef std::vector<uint8_t> Bytes;

static const uint32_t PROTO_MASK = 0x80000000u;
enum : uint32_t {
    EMsg_Multi                              = 1,
    EMsg_ServiceMethodResponse              = 147,
    EMsg_ServiceMethodCallFromClient        = 151,
    EMsg_ChannelEncryptRequest              = 1303,
    EMsg_ChannelEncryptResponse             = 1304,
    EMsg_ChannelEncryptResult               = 1305,
    EMsg_ClientLogOnResponse                = 751,
    EMsg_ClientLogon                        = 5514,
    EMsg_ClientGetDepotDecryptionKey        = 5438,
    EMsg_ClientGetDepotDecryptionKeyResponse= 5439,
};
// AnonUser steamid: universe Public(1)<<56 | type AnonUser(10)<<52  == 0x01a0000000000000
static const uint64_t ANON_STEAMID = ((uint64_t)1 << 56) | ((uint64_t)10 << 52);

// Steam Universe (Public) RSA-1024 public key: modulus (big-endian) + exponent 0x11
static const unsigned char kUniverseMod[128] = {
  0xdf,0xec,0x1a,0xd6,0x2c,0x10,0x66,0x2c,0x17,0x35,0x3a,0x14,
  0xb0,0x7c,0x59,0x11,0x7f,0x9d,0xd3,0xd8,0x2b,0x7a,0xe3,0xe0,
  0x15,0xcd,0x19,0x1e,0x46,0xe8,0x7b,0x87,0x74,0xa2,0x18,0x46,
  0x31,0xa9,0x03,0x14,0x79,0x82,0x8e,0xe9,0x45,0xa2,0x49,0x12,
  0xa9,0x23,0x68,0x73,0x89,0xcf,0x69,0xa1,0xb1,0x61,0x46,0xbd,
  0xc1,0xbe,0xbf,0xd6,0x01,0x1b,0xd8,0x81,0xd4,0xdc,0x90,0xfb,
  0xfe,0x4f,0x52,0x73,0x66,0xcb,0x95,0x70,0xd7,0xc5,0x8e,0xba,
  0x1c,0x7a,0x33,0x75,0xa1,0x62,0x34,0x46,0xbb,0x60,0xb7,0x80,
  0x68,0xfa,0x13,0xa7,0x7a,0x8a,0x37,0x4b,0x9e,0xc6,0xf4,0x5d,
  0x5f,0x3a,0x99,0xf9,0x9e,0xc4,0x3a,0xe9,0x63,0xa2,0xbb,0x88,
  0x19,0x28,0xe0,0xe7,0x14,0xc0,0x42,0x89,
};

// ============================================================ protobuf writer
static void pb_varint(Bytes& b, uint64_t v){ while(v>=0x80){b.push_back((uint8_t)(v|0x80));v>>=7;} b.push_back((uint8_t)v);}
static void pb_tag(Bytes& b,uint32_t f,uint32_t w){ pb_varint(b,((uint64_t)f<<3)|w);}
static void pb_u32(Bytes& b,uint32_t f,uint32_t v){ pb_tag(b,f,0); pb_varint(b,v);}
static void pb_bool(Bytes& b,uint32_t f,bool v){ pb_tag(b,f,0); pb_varint(b,v?1:0);}
static void pb_u64(Bytes& b,uint32_t f,uint64_t v){ pb_tag(b,f,0); pb_varint(b,v);}
static void pb_fix64(Bytes& b,uint32_t f,uint64_t v){ pb_tag(b,f,1); for(int i=0;i<8;i++) b.push_back((uint8_t)(v>>(8*i)));}
static void pb_str(Bytes& b,uint32_t f,const std::string& s){ pb_tag(b,f,2); pb_varint(b,s.size()); b.insert(b.end(),s.begin(),s.end());}
static void pb_bytes(Bytes& b,uint32_t f,const Bytes& d){ pb_tag(b,f,2); pb_varint(b,d.size()); b.insert(b.end(),d.begin(),d.end());}

// ============================================================ protobuf reader
struct PbReader {
    const uint8_t* p; const uint8_t* end;
    PbReader(const uint8_t* d,size_t n):p(d),end(d+n){}
    uint64_t varint(){ uint64_t v=0;int s=0; while(p<end){uint8_t b=*p++; v|=(uint64_t)(b&0x7f)<<s; if(!(b&0x80))break; s+=7;} return v;}
    bool next(uint32_t& f,uint32_t& w){ if(p>=end)return false; uint64_t k=varint(); f=(uint32_t)(k>>3); w=(uint32_t)(k&7); return true;}
    void skip(uint32_t w){ switch(w){case 0:varint();break;case 1:p+=8;break;case 5:p+=4;break;case 2:{uint64_t n=varint();p+=n;break;}default:p=end;break;} }
    uint64_t fix64(){ uint64_t v=0; for(int i=0;i<8&&p<end;i++) v|=(uint64_t)(*p++)<<(8*i); return v;}
    Bytes lendelim(){ uint64_t n=varint(); size_t take=(p+n<=end)?(size_t)n:(size_t)(end-p); Bytes o(p,p+take); p+=take; return o;}
};

// ============================================================ BCrypt crypto
static bool gen_random(uint8_t* buf, size_t n){
    return BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == STATUS_SUCCESS;
}
// RSA-OAEP(SHA1) encrypt with the Universe public key
static bool rsa_oaep_encrypt(const Bytes& plain, Bytes& out){
    BCRYPT_ALG_HANDLE hAlg=0; BCRYPT_KEY_HANDLE hKey=0; bool ok=false;
    if(BCryptOpenAlgorithmProvider(&hAlg,BCRYPT_RSA_ALGORITHM,NULL,0)!=STATUS_SUCCESS) return false;
    // build BCRYPT_RSAPUBLIC_BLOB = header + exponent + modulus
    Bytes blob(sizeof(BCRYPT_RSAKEY_BLOB));
    BCRYPT_RSAKEY_BLOB* h=(BCRYPT_RSAKEY_BLOB*)blob.data();
    h->Magic=BCRYPT_RSAPUBLIC_MAGIC; h->BitLength=1024; h->cbPublicExp=1; h->cbModulus=128; h->cbPrime1=0; h->cbPrime2=0;
    blob.push_back(0x11);                                   // exponent (big-endian, 1 byte)
    blob.insert(blob.end(), kUniverseMod, kUniverseMod+128);// modulus (big-endian)
    if(BCryptImportKeyPair(hAlg,NULL,BCRYPT_RSAPUBLIC_BLOB,&hKey,blob.data(),(ULONG)blob.size(),0)==STATUS_SUCCESS){
        BCRYPT_OAEP_PADDING_INFO pad={ BCRYPT_SHA1_ALGORITHM, NULL, 0 };
        ULONG need=0;
        if(BCryptEncrypt(hKey,(PUCHAR)plain.data(),(ULONG)plain.size(),&pad,NULL,0,NULL,0,&need,BCRYPT_PAD_OAEP)==STATUS_SUCCESS){
            out.resize(need); ULONG got=0;
            if(BCryptEncrypt(hKey,(PUCHAR)plain.data(),(ULONG)plain.size(),&pad,NULL,0,out.data(),need,&got,BCRYPT_PAD_OAEP)==STATUS_SUCCESS){
                out.resize(got); ok=true;
            }
        }
        BCryptDestroyKey(hKey);
    }
    BCryptCloseAlgorithmProvider(hAlg,0);
    return ok;
}
// AES-256 helper. mode: 0=ECB, 1=CBC. iv (16) used for CBC (copied; BCrypt mutates it).
static bool aes256(const uint8_t key[32], int mode, bool encrypt, const uint8_t* iv, const Bytes& in, Bytes& out){
    BCRYPT_ALG_HANDLE hAlg=0; BCRYPT_KEY_HANDLE hKey=0; bool ok=false;
    if(BCryptOpenAlgorithmProvider(&hAlg,BCRYPT_AES_ALGORITHM,NULL,0)!=STATUS_SUCCESS) return false;
    const wchar_t* chain = (mode==0)?BCRYPT_CHAIN_MODE_ECB:BCRYPT_CHAIN_MODE_CBC;
    BCryptSetProperty(hAlg,BCRYPT_CHAINING_MODE,(PUCHAR)chain,(ULONG)((wcslen(chain)+1)*sizeof(wchar_t)),0);
    if(BCryptGenerateSymmetricKey(hAlg,&hKey,NULL,0,(PUCHAR)key,32,0)==STATUS_SUCCESS){
        uint8_t ivcopy[16]; if(iv) memcpy(ivcopy,iv,16);
        ULONG need=0; PUCHAR pIv = (mode==1)? ivcopy : NULL; ULONG ivLen=(mode==1)?16:0;
        NTSTATUS st = encrypt
            ? BCryptEncrypt(hKey,(PUCHAR)in.data(),(ULONG)in.size(),NULL,pIv,ivLen,NULL,0,&need,0)
            : BCryptDecrypt(hKey,(PUCHAR)in.data(),(ULONG)in.size(),NULL,pIv,ivLen,NULL,0,&need,0);
        if(st==STATUS_SUCCESS){
            out.resize(need); ULONG got=0;
            if(iv) memcpy(ivcopy,iv,16); // reset (query call may have advanced it)
            st = encrypt
                ? BCryptEncrypt(hKey,(PUCHAR)in.data(),(ULONG)in.size(),NULL,pIv,ivLen,out.data(),need,&got,0)
                : BCryptDecrypt(hKey,(PUCHAR)in.data(),(ULONG)in.size(),NULL,pIv,ivLen,out.data(),need,&got,0);
            if(st==STATUS_SUCCESS){ out.resize(got); ok=true; }
        }
        BCryptDestroyKey(hKey);
    }
    BCryptCloseAlgorithmProvider(hAlg,0);
    return ok;
}
static bool hmac_sha1(const uint8_t* secret,size_t slen,const Bytes& data,uint8_t out[20]){
    BCRYPT_ALG_HANDLE hAlg=0; BCRYPT_HASH_HANDLE hH=0; bool ok=false;
    if(BCryptOpenAlgorithmProvider(&hAlg,BCRYPT_SHA1_ALGORITHM,NULL,BCRYPT_ALG_HANDLE_HMAC_FLAG)!=STATUS_SUCCESS) return false;
    if(BCryptCreateHash(hAlg,&hH,NULL,0,(PUCHAR)secret,(ULONG)slen,0)==STATUS_SUCCESS){
        if(BCryptHashData(hH,(PUCHAR)data.data(),(ULONG)data.size(),0)==STATUS_SUCCESS &&
           BCryptFinishHash(hH,out,20,0)==STATUS_SUCCESS) ok=true;
        BCryptDestroyHash(hH);
    }
    BCryptCloseAlgorithmProvider(hAlg,0);
    return ok;
}
static Bytes pkcs7_pad(const Bytes& in){ size_t p=16-(in.size()%16); Bytes o=in; o.insert(o.end(),p,(uint8_t)p); return o; }
static bool pkcs7_unpad(Bytes& b){ if(b.empty())return false; uint8_t p=b.back(); if(p<1||p>16||p>b.size())return false; b.resize(b.size()-p); return true; }

// symmetric_encrypt_HMAC: prefix(3) -> hmac -> iv=hmac[:13]+prefix -> ECB(iv)+CBC(body)
static bool sym_encrypt_hmac(const uint8_t key[32], const uint8_t hmac_secret[16], const Bytes& plain, Bytes& out){
    uint8_t prefix[3]; if(!gen_random(prefix,3)) return false;
    Bytes hin; hin.insert(hin.end(),prefix,prefix+3); hin.insert(hin.end(),plain.begin(),plain.end());
    uint8_t h[20]; if(!hmac_sha1(hmac_secret,16,hin,h)) return false;
    uint8_t iv[16]; memcpy(iv,h,13); memcpy(iv+13,prefix,3);
    Bytes enc_iv, ct;
    if(!aes256(key,0,true,NULL,Bytes(iv,iv+16),enc_iv)) return false;
    if(!aes256(key,1,true,iv,pkcs7_pad(plain),ct)) return false;
    out.clear(); out.insert(out.end(),enc_iv.begin(),enc_iv.end()); out.insert(out.end(),ct.begin(),ct.end());
    return true;
}
// symmetric_decrypt_HMAC
static bool sym_decrypt_hmac(const uint8_t key[32], const uint8_t hmac_secret[16], const Bytes& ct, Bytes& out){
    if(ct.size()<32) return false;
    Bytes enc_iv(ct.begin(),ct.begin()+16), body(ct.begin()+16,ct.end());
    Bytes iv; if(!aes256(key,0,false,NULL,enc_iv,iv)) return false;   // ECB-decrypt IV
    if(iv.size()!=16) return false;
    Bytes plain; if(!aes256(key,1,false,iv.data(),body,plain)) return false;
    if(!pkcs7_unpad(plain)) return false;
    // verify HMAC
    Bytes hin; hin.insert(hin.end(),iv.begin()+13,iv.end()); hin.insert(hin.end(),plain.begin(),plain.end());
    uint8_t h[20]; if(!hmac_sha1(hmac_secret,16,hin,h)) return false;
    if(memcmp(h,iv.data(),13)!=0){ printf("  [warn] HMAC mismatch\n"); /* continue anyway for diag */ }
    out=std::move(plain); return true;
}

// ============================================================ Steam framing
struct SteamMsg { uint32_t emsg=0; uint64_t steamid=0; int32_t sessionid=0; int32_t eresult=0; Bytes body; };

static bool parse_frame(const uint8_t* data,size_t len,SteamMsg& out){
    if(len<4) return false;
    uint32_t raw; memcpy(&raw,data,4);
    bool proto=(raw&PROTO_MASK)!=0; out.emsg=raw&~PROTO_MASK;
    if(proto){
        if(len<8) return false;
        uint32_t hl; memcpy(&hl,data+4,4);
        if(8+(size_t)hl>len) return false;
        PbReader hr(data+8,hl); uint32_t f,w;
        while(hr.next(f,w)){ if(f==1&&w==1)out.steamid=hr.fix64(); else if(f==2&&w==0)out.sessionid=(int32_t)hr.varint(); else if(f==13&&w==0)out.eresult=(int32_t)hr.varint(); else hr.skip(w);}
        out.body.assign(data+8+hl,data+len);
    } else {
        // non-proto ExtendedMsgHdr or MsgHdr; for our purposes body starts after header.
        // ChannelEncrypt* use MsgHdr (20 bytes: emsg+targetJob+sourceJob).
        if(len<20) return false;
        out.body.assign(data+20,data+len);
    }
    return true;
}
static bool gunzip(const Bytes& in,uint32_t out_size,Bytes& out){
    out.assign(out_size?out_size:in.size()*4,0);
    z_stream s; memset(&s,0,sizeof(s));
    if(inflateInit2(&s,15+32)!=Z_OK) return false;
    s.next_in=(Bytef*)in.data(); s.avail_in=(uInt)in.size(); s.next_out=out.data(); s.avail_out=(uInt)out.size();
    int r=inflate(&s,Z_FINISH); size_t produced=out.size()-s.avail_out; inflateEnd(&s);
    if(r!=Z_STREAM_END&&r!=Z_OK) return false; out.resize(produced); return true;
}
static void collect(const uint8_t* data,size_t len,std::vector<SteamMsg>& q){
    SteamMsg m; if(!parse_frame(data,len,m)) return;
    if(m.emsg==EMsg_Multi){
        PbReader r(m.body.data(),m.body.size()); uint32_t f,w,size_unzipped=0; Bytes payload;
        while(r.next(f,w)){ if(f==1&&w==0)size_unzipped=(uint32_t)r.varint(); else if(f==2&&w==2)payload=r.lendelim(); else r.skip(w);}
        Bytes flat;
        if(size_unzipped>0){ if(!gunzip(payload,size_unzipped,flat)){printf("  [Multi] gunzip FAILED\n"); return;} }
        else flat=payload;
        size_t off=0;
        while(off+4<=flat.size()){ uint32_t sz; memcpy(&sz,flat.data()+off,4); off+=4; if(off+sz>flat.size())break; collect(flat.data()+off,sz,q); off+=sz; }
    } else q.push_back(std::move(m));
}
static Bytes build_proto_frame(uint32_t emsg,const Bytes& hdr,const Bytes& body){
    Bytes f; uint32_t e=emsg|PROTO_MASK;
    f.insert(f.end(),(uint8_t*)&e,(uint8_t*)&e+4);
    uint32_t hl=(uint32_t)hdr.size(); f.insert(f.end(),(uint8_t*)&hl,(uint8_t*)&hl+4);
    f.insert(f.end(),hdr.begin(),hdr.end()); f.insert(f.end(),body.begin(),body.end());
    return f;
}
static Bytes make_header(uint64_t steamid,int32_t sessionid,uint64_t jobid_source){
    // Match ValvePython: emit steamid always; sessionid + jobid_source only when set.
    Bytes h; pb_fix64(h,1,steamid);
    if(sessionid) pb_u32(h,2,(uint32_t)sessionid);
    if(jobid_source!=0xFFFFFFFFFFFFFFFFull) pb_fix64(h,10,jobid_source);
    return h;
}
static Bytes make_um_header(uint64_t steamid,int32_t sessionid,uint64_t jobid,const std::string& method){
    Bytes h; pb_fix64(h,1,steamid); if(sessionid) pb_u32(h,2,(uint32_t)sessionid);
    pb_fix64(h,10,jobid); pb_str(h,12,method); return h;   // f12 = target_job_name
}
static Bytes make_nonproto_hdr(uint32_t emsg){ // MsgHdr: emsg(u32) + targetJobID(i64=-1) + sourceJobID(i64=-1)
    Bytes h; h.insert(h.end(),(uint8_t*)&emsg,(uint8_t*)&emsg+4);
    int64_t neg=-1; for(int i=0;i<2;i++) h.insert(h.end(),(uint8_t*)&neg,(uint8_t*)&neg+8); return h;
}

// ============================================================ TCP transport
static bool recv_all(SOCKET s,uint8_t* buf,size_t n){ size_t g=0; while(g<n){ int r=recv(s,(char*)buf+g,(int)(n-g),0); if(r<=0)return false; g+=r;} return true; }
static bool send_all(SOCKET s,const uint8_t* buf,size_t n){ size_t g=0; while(g<n){ int r=send(s,(const char*)buf+g,(int)(n-g),0); if(r<=0)return false; g+=r;} return true; }
static bool recv_message(SOCKET s,Bytes& out){ uint8_t hd[8]; if(!recv_all(s,hd,8))return false; uint32_t len; memcpy(&len,hd,4); if(memcmp(hd+4,"VT01",4)){printf("bad magic\n");return false;} out.resize(len); return len? recv_all(s,out.data(),len):true; }
static bool send_message(SOCKET s,const Bytes& msg){ uint8_t pre[8]; uint32_t len=(uint32_t)msg.size(); memcpy(pre,&len,4); memcpy(pre+4,"VT01",4); return send_all(s,pre,8)&&send_all(s,msg.data(),msg.size()); }

// ============================================================ CM discovery
// binary HTTPS GET to an arbitrary host (CDN or WebAPI)
static Bytes http_fetch(const std::wstring& host,const std::wstring& path){
    Bytes out;
    HINTERNET hS=WinHttpOpen(L"gh-cm/1.0",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!hS) return out;
    HINTERNET hC=WinHttpConnect(hS,host.c_str(),INTERNET_DEFAULT_HTTPS_PORT,0);
    if(hC){
        HINTERNET hR=WinHttpOpenRequest(hC,L"GET",path.c_str(),NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
        if(hR){
            DWORD sf=SECURITY_FLAG_IGNORE_UNKNOWN_CA|SECURITY_FLAG_IGNORE_CERT_CN_INVALID|SECURITY_FLAG_IGNORE_CERT_DATE_INVALID|SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(hR,WINHTTP_OPTION_SECURITY_FLAGS,&sf,sizeof(sf));
            if(WinHttpSendRequest(hR,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)&&WinHttpReceiveResponse(hR,NULL)){
                DWORD avail=0;
                do{ avail=0; WinHttpQueryDataAvailable(hR,&avail); if(avail){ Bytes c(avail); DWORD got=0; WinHttpReadData(hR,c.data(),avail,&got); out.insert(out.end(),c.begin(),c.begin()+got);} }while(avail>0);
            }
            WinHttpCloseHandle(hR);
        }
        WinHttpCloseHandle(hC);
    }
    WinHttpCloseHandle(hS);
    return out;
}
// raw DEFLATE inflate (windowBits=-15), for ZIP entries
static bool raw_inflate(const uint8_t* in,size_t inlen,size_t out_hint,Bytes& out){
    out.assign(out_hint?out_hint:inlen*8,0);
    z_stream s; memset(&s,0,sizeof(s));
    if(inflateInit2(&s,-15)!=Z_OK) return false;
    s.next_in=(Bytef*)in; s.avail_in=(uInt)inlen; s.next_out=out.data(); s.avail_out=(uInt)out.size();
    int r;
    do{ r=inflate(&s,Z_NO_FLUSH);
        if(s.avail_out==0 && r!=Z_STREAM_END){ size_t used=out.size(); out.resize(out.size()*2); s.next_out=out.data()+used; s.avail_out=(uInt)(out.size()-used); }
    }while(r==Z_OK);
    size_t produced=out.size()-s.avail_out; inflateEnd(&s);
    if(r!=Z_STREAM_END) return false; out.resize(produced); return true;
}
// unzip the single entry of a manifest ZIP (local file header at offset 0)
static bool unzip_first(const Bytes& zip,Bytes& out){
    if(zip.size()<30 || zip[0]!=0x50||zip[1]!=0x4b||zip[2]!=0x03||zip[3]!=0x04){ printf("not a ZIP (%zu bytes)\n",zip.size()); return false; }
    uint16_t method; memcpy(&method,&zip[8],2);
    uint32_t comp,uncomp; memcpy(&comp,&zip[18],4); memcpy(&uncomp,&zip[22],4);
    uint16_t fnl,exl; memcpy(&fnl,&zip[26],2); memcpy(&exl,&zip[28],2);
    size_t data=30+(size_t)fnl+exl;
    if(data+comp>zip.size()){ printf("zip truncated\n"); return false; }
    if(method==0){ out.assign(zip.begin()+data,zip.begin()+data+comp); return true; }
    if(method==8) return raw_inflate(&zip[data],comp,uncomp,out);
    printf("unknown zip method %u\n",method); return false;
}

// first entry of "serverlist":[ "ip:port", ... ]  (TCP CMs)
static std::string find_tcp_cm(const std::string& json){
    size_t k=json.find("\"serverlist\"");
    if(k==std::string::npos) return "";
    size_t br=json.find('[',k); if(br==std::string::npos) return "";
    size_t q=json.find('"',br); if(q==std::string::npos) return "";
    size_t e=json.find('"',q+1); if(e==std::string::npos) return "";
    return json.substr(q+1,e-q-1);
}

// ============================================================ main
static uint8_t g_key[32]; static uint8_t g_hmac[16]; static bool g_secured=false;
static Bytes g_depotkey; static std::string g_cdn; static uint64_t g_mcode=0;

static bool send_enc(SOCKET s,const Bytes& plain){
    if(!g_secured) return send_message(s,plain);
    Bytes enc; if(!sym_encrypt_hmac(g_key,g_hmac,plain,enc)) return false;
    return send_message(s,enc);
}
static bool recv_dec(SOCKET s,Bytes& plain){
    Bytes raw; if(!recv_message(s,raw)) return false;
    if(!g_secured){ plain=std::move(raw); return true; }
    return sym_decrypt_hmac(g_key,g_hmac,raw,plain);
}

int main(int argc,char** argv){
    setvbuf(stdout,NULL,_IONBF,0);
    uint32_t depot=argc>1?(uint32_t)strtoul(argv[1],0,10):232256;
    uint32_t app  =argc>2?(uint32_t)strtoul(argv[2],0,10):232250;

    WSADATA wsa; WSAStartup(MAKEWORD(2,2),&wsa);

    std::string cm;
    if(argc>3) cm=argv[3];
    else{
        printf("discovering CM (tcp)...\n");
            Bytes jb=http_fetch(L"api.steampowered.com",L"/ISteamDirectory/GetCMList/v1/?cellid=0&format=json");
        std::string j(jb.begin(),jb.end());
        cm=find_tcp_cm(j);
        if(cm.empty()){ printf("CM discovery failed (json %zu bytes)\n",j.size()); return 1; }
    }
    std::string host=cm.substr(0,cm.find(':')); std::string port=cm.substr(cm.find(':')+1);
    printf("CM = %s:%s\n",host.c_str(),port.c_str());

    struct addrinfo hints{}, *res=0; hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
    if(getaddrinfo(host.c_str(),port.c_str(),&hints,&res)!=0||!res){ printf("resolve failed\n"); return 1; }
    SOCKET s=socket(res->ai_family,res->ai_socktype,res->ai_protocol);
    if(connect(s,res->ai_addr,(int)res->ai_addrlen)!=0){ printf("connect failed err=%d\n",WSAGetLastError()); return 1; }
    freeaddrinfo(res);
    printf("tcp connected\n");

    uint64_t steamid=ANON_STEAMID; int32_t sessionid=0;

    // ---- ChannelEncrypt handshake ----
    {
        Bytes raw; if(!recv_dec(s,raw)){ printf("no ChannelEncryptRequest\n"); return 1; }
        SteamMsg m; parse_frame(raw.data(),raw.size(),m);
        if(m.emsg!=EMsg_ChannelEncryptRequest){ printf("expected EncryptRequest, got %u\n",m.emsg); return 1; }
        uint32_t protoVer=0,universe=0; memcpy(&protoVer,m.body.data(),4); memcpy(&universe,m.body.data()+4,4);
        Bytes challenge(m.body.begin()+8,m.body.end());
        printf("<- ChannelEncryptRequest proto=%u universe=%u challenge=%zu bytes\n",protoVer,universe,challenge.size());

        uint8_t sk[32]; if(!gen_random(sk,32)){ printf("rng fail\n"); return 1; }
        Bytes oaep_in(sk,sk+32); oaep_in.insert(oaep_in.end(),challenge.begin(),challenge.end());
        Bytes enc; if(!rsa_oaep_encrypt(oaep_in,enc)){ printf("RSA-OAEP failed\n"); return 1; }
        printf("   session key encrypted -> %zu bytes\n",enc.size());
        uint32_t crc=(uint32_t)crc32(0,enc.data(),(uInt)enc.size());

        // ChannelEncryptResponse body: <II128sII> = protoVer(1),keySize(128),key,crc,0
        Bytes body; uint32_t one=1,ks=128,zero=0;
        body.insert(body.end(),(uint8_t*)&one,(uint8_t*)&one+4);
        body.insert(body.end(),(uint8_t*)&ks,(uint8_t*)&ks+4);
        Bytes key128=enc; key128.resize(128,0);
        body.insert(body.end(),key128.begin(),key128.end());
        body.insert(body.end(),(uint8_t*)&crc,(uint8_t*)&crc+4);
        body.insert(body.end(),(uint8_t*)&zero,(uint8_t*)&zero+4);
        Bytes resp=make_nonproto_hdr(EMsg_ChannelEncryptResponse); resp.insert(resp.end(),body.begin(),body.end());
        if(!send_message(s,resp)){ printf("send resp failed\n"); return 1; }
        printf("-> ChannelEncryptResponse sent\n");

        Bytes r2; if(!recv_dec(s,r2)){ printf("no EncryptResult\n"); return 1; }
        SteamMsg m2; parse_frame(r2.data(),r2.size(),m2);
        if(m2.emsg!=EMsg_ChannelEncryptResult){ printf("expected EncryptResult, got %u\n",m2.emsg); return 1; }
        uint32_t er=0; memcpy(&er,m2.body.data(),4);
        printf("<- ChannelEncryptResult eresult=%u\n",er);
        if(er!=1){ printf("channel NOT secured\n"); return 1; }
        memcpy(g_key,sk,32); memcpy(g_hmac,sk,16); g_secured=true;
        printf("*** channel secured ***\n");
    }

    // ---- anonymous ClientLogon ----
    {
        // Match ValvePython anonymous_login() exactly: only protocol_version +
        // client_package_version; steamid=AnonUser/Public in the header.
        Bytes body;
        pb_u32(body,1,65580);          // protocol_version
        pb_u32(body,5,1561159470);     // client_package_version
        Bytes hdr=make_header(steamid,0,0xFFFFFFFFFFFFFFFFull);
        Bytes f=build_proto_frame(EMsg_ClientLogon,hdr,body);
        printf("-> ClientLogon (anon)\n");
        if(!send_enc(s,f)){ printf("send logon failed\n"); return 1; }
    }
    bool logged_in=false;
    for(int i=0;i<40&&!logged_in;i++){
        Bytes raw; if(!recv_dec(s,raw)) break;
        printf("   [rx %zu bytes] ",raw.size());
        for(size_t k=0;k<raw.size()&&k<48;k++) printf("%02x",raw[k]);
        printf("\n");
        std::vector<SteamMsg> q; collect(raw.data(),raw.size(),q);
        for(auto& m:q){
            if(m.emsg==EMsg_ClientLogOnResponse){
                printf("   [logon resp body %zu bytes] ",m.body.size());
                for(size_t k=0;k<m.body.size()&&k<48;k++) printf("%02x",m.body[k]);
                printf("\n");
                steamid=m.steamid; sessionid=m.sessionid;
                PbReader r(m.body.data(),m.body.size()); uint32_t f,w; int32_t erb=0;
                while(r.next(f,w)){ if(f==1&&w==0)erb=(int32_t)r.varint(); else r.skip(w);}
                printf("<- ClientLogOnResponse eresult=%d steamid=%llu session=%d\n",erb,(unsigned long long)steamid,sessionid);
                logged_in=(erb==1);
            } else printf("   (logon phase) emsg=%u\n",m.emsg);
        }
    }
    if(!logged_in){ printf("LOGON FAILED\n"); closesocket(s); return 1; }

    // ---- ClientGetDepotDecryptionKey ----
    {
        Bytes body; pb_u32(body,1,depot); pb_u32(body,2,app);
        Bytes hdr=make_header(steamid,sessionid,1);
        Bytes f=build_proto_frame(EMsg_ClientGetDepotDecryptionKey,hdr,body);
        printf("-> ClientGetDepotDecryptionKey depot=%u app=%u\n",depot,app);
        if(!send_enc(s,f)){ printf("send depotkey failed\n"); return 1; }
    }
    int rc=1;
    for(int i=0;i<40;i++){
        Bytes raw; if(!recv_dec(s,raw)) break;
        std::vector<SteamMsg> q; collect(raw.data(),raw.size(),q);
        bool got=false;
        for(auto& m:q){
            if(m.emsg==EMsg_ClientGetDepotDecryptionKeyResponse){
                PbReader r(m.body.data(),m.body.size()); uint32_t f,w; int32_t er=0; uint32_t did=0; Bytes key;
                while(r.next(f,w)){ if(f==1&&w==0)er=(int32_t)r.varint(); else if(f==2&&w==0)did=(uint32_t)r.varint(); else if(f==3&&w==2)key=r.lendelim(); else r.skip(w);}
                printf("*** DepotKeyResponse eresult=%d depot=%u keylen=%zu\n",er,did,key.size());
                if(er==1){ printf("*** KEY = "); for(uint8_t b:key) printf("%02x",b); printf("\n"); rc=0; g_depotkey=key; }
                got=true;
            } else printf("   (depotkey phase) emsg=%u\n",m.emsg);
        }
        if(got) break;
    }

    // ---- ServiceMethods over the CM (proves the engine is NOT needed for content RPCs) ----
    uint64_t jobid=2;
    {   // GetServersForSteamPipe
        Bytes body; pb_u32(body,1,0); pb_u32(body,2,20);  // cell_id, max_servers
        Bytes hdr=make_um_header(steamid,sessionid,jobid++,"ContentServerDirectory.GetServersForSteamPipe#1");
        printf("-> GetServersForSteamPipe\n");
        if(send_enc(s,build_proto_frame(EMsg_ServiceMethodCallFromClient,hdr,body)))
        for(int i=0;i<40;i++){ Bytes raw; if(!recv_dec(s,raw))break; std::vector<SteamMsg> q; collect(raw.data(),raw.size(),q); bool got=false;
            for(auto&m:q){ if(m.emsg==EMsg_ServiceMethodResponse){ got=true; int n=0;
                PbReader r(m.body.data(),m.body.size()); uint32_t fl,w;
                while(r.next(fl,w)){ if(fl==1&&w==2){ Bytes srv=r.lendelim();
                    PbReader sr(srv.data(),srv.size()); uint32_t sf,sw; std::string host,https;
                    while(sr.next(sf,sw)){ if(sf==8&&sw==2){Bytes b=sr.lendelim();host.assign(b.begin(),b.end());}
                        else if(sf==12&&sw==2){Bytes b=sr.lendelim();https.assign(b.begin(),b.end());} else sr.skip(sw);}
                    if(g_cdn.empty()&&!host.empty()) g_cdn=host;
                    if(n<4) printf("   server host=%s https=%s\n",host.c_str(),https.c_str()); n++;
                } else r.skip(w);}
                printf("*** content servers = %d\n",n);
            } else printf("   (servers phase) emsg=%u\n",m.emsg);}
            if(got)break; }
    }
    {   // GetManifestRequestCode (anon triple: app 232250 / depot 232256 / manifest 698669566371320345)
        Bytes body; pb_u32(body,1,232250); pb_u32(body,2,232256); pb_u64(body,3,698669566371320345ULL); pb_str(body,4,"public");
        Bytes hdr=make_um_header(steamid,sessionid,jobid++,"ContentServerDirectory.GetManifestRequestCode#1");
        printf("-> GetManifestRequestCode\n");
        if(send_enc(s,build_proto_frame(EMsg_ServiceMethodCallFromClient,hdr,body)))
        for(int i=0;i<40;i++){ Bytes raw; if(!recv_dec(s,raw))break; std::vector<SteamMsg> q; collect(raw.data(),raw.size(),q); bool got=false;
            for(auto&m:q){ if(m.emsg==EMsg_ServiceMethodResponse){ got=true; PbReader r(m.body.data(),m.body.size()); uint32_t fl,w; uint64_t code=0;
                while(r.next(fl,w)){ if(fl==1&&w==0)code=r.varint(); else r.skip(w);}
                g_mcode=code; printf("*** manifest_request_code = %llu\n",(unsigned long long)code);
            } else printf("   (mcode phase) emsg=%u\n",m.emsg);}
            if(got)break; }
    }

    // ---- Manifest + chunk fetch/decrypt: the full standalone CDN pipeline in C++ ----
    if(!g_depotkey.empty() && !g_cdn.empty() && g_mcode){
        const uint32_t depot=232256; const char* manifest="698669566371320345";
        wchar_t whost[256]; MultiByteToWideChar(CP_UTF8,0,g_cdn.c_str(),-1,whost,256);
        char path[256]; sprintf(path,"/depot/%u/manifest/%s/5/%llu",depot,manifest,(unsigned long long)g_mcode);
        wchar_t wpath[256]; MultiByteToWideChar(CP_UTF8,0,path,-1,wpath,256);
        printf("-> GET https://%s%s\n",g_cdn.c_str(),path);
        Bytes zip=http_fetch(whost,wpath);
        printf("   manifest zip = %zu bytes\n",zip.size());
        Bytes man;
        if(unzip_first(zip,man)){
            printf("   manifest unzipped = %zu bytes\n",man.size());
            Bytes firstsha; uint32_t cbcomp=0,cborig=0; int totalchunks=0; size_t off=0;
            while(off+4<=man.size()){
                uint32_t magic; memcpy(&magic,&man[off],4); off+=4;
                if(magic==0x32C415ABu) break;                 // EndOfManifest
                if(off+4>man.size()) break;
                uint32_t seclen; memcpy(&seclen,&man[off],4); off+=4;
                if(off+seclen>man.size()) break;
                if(magic==0x71F617D0u){                        // ContentManifestPayload
                    PbReader pr(&man[off],seclen); uint32_t f,w;
                    while(pr.next(f,w)){ if(f==1&&w==2){ Bytes mp=pr.lendelim();
                        PbReader mr(mp.data(),mp.size()); uint32_t mf,mw;
                        while(mr.next(mf,mw)){ if(mf==6&&mw==2){ Bytes ck=mr.lendelim(); totalchunks++;
                            if(firstsha.empty()){ PbReader cr(ck.data(),ck.size()); uint32_t cf,cw;
                                while(cr.next(cf,cw)){ if(cf==1&&cw==2)firstsha=cr.lendelim(); else if(cf==4&&cw==0)cborig=(uint32_t)cr.varint(); else if(cf==5&&cw==0)cbcomp=(uint32_t)cr.varint(); else cr.skip(cw);} }
                        } else mr.skip(mw);}
                    } else pr.skip(w);}
                }
                off+=seclen;
            }
            printf("   parsed manifest: %d chunks total\n",totalchunks);
            if(!firstsha.empty()){
                char shahex[64]={0}; for(size_t i=0;i<firstsha.size();i++) sprintf(shahex+i*2,"%02x",firstsha[i]);
                printf("   first chunk sha=%s cb_compressed=%u cb_original=%u\n",shahex,cbcomp,cborig);
                char cpath[128]; sprintf(cpath,"/depot/%u/chunk/%s",depot,shahex);
                wchar_t wcpath[128]; MultiByteToWideChar(CP_UTF8,0,cpath,-1,wcpath,128);
                printf("-> GET https://%s%s\n",g_cdn.c_str(),cpath);
                Bytes enc=http_fetch(whost,wcpath);
                printf("   chunk enc = %zu bytes\n",enc.size());
                if(enc.size()>=32){
                    Bytes iv;   aes256(g_depotkey.data(),0,false,NULL,Bytes(enc.begin(),enc.begin()+16),iv);
                    Bytes cont; aes256(g_depotkey.data(),1,false,iv.data(),Bytes(enc.begin()+16,enc.end()),cont);
                    pkcs7_unpad(cont);
                    printf("*** decrypted container = %zu bytes, magic=%.4s\n",cont.size(),cont.size()>=4?(char*)cont.data():"");
                    FILE* fp=fopen("chunk_container.bin","wb");
                    if(fp){ fwrite(cont.data(),1,cont.size(),fp); fclose(fp);
                        printf("*** wrote chunk_container.bin (decompress+sha1 should == %s)\n",shahex); }
                }
            }
        }
    }

    closesocket(s); WSACleanup();
    return rc;
}
