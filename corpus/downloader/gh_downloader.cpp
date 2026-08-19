// gh_downloader.cpp -- standalone CLI wrapper over the in-process downloader library
// (corpus/downloader/cm_downloader.h). Also the regression harness for that header.
//
// Build:  corpus/downloader/build_cm_test.sh corpus/downloader/gh_downloader.cpp <out.exe>
// Run:    gh_downloader.exe <app> <depot> <manifest> [outdir=gamedir] [branch=public]
//    or:  provide gh_download.txt = "app depot manifest name [branch]" and run with no args
//         (writes the game to game\<name>, progress to gh_download_status.txt).
// Owned games: set GH_STEAM_TOKEN + GH_STEAM_STEAMID to log on as the user.

#include "cm_downloader.h"
#include <cstdio>
#include <cstdlib>
#include <string>

static std::string g_status_path="gh_download_status.txt";
static std::string g_dl_name="game";

// progress callback -> overwrite gh_download_status.txt + echo to stdout
static void on_progress(void*,const char* phase,int percent,int fdone,int ftot,unsigned long long bytes){
    char buf[512]; int n=snprintf(buf,sizeof(buf),"phase=%s\npercent=%d\nfiles=%d/%d\nbytes=%llu\nname=%s\n",
        phase,percent,fdone,ftot,bytes,g_dl_name.c_str());
    HANDLE h=CreateFileW(ghdl::towide(g_status_path).c_str(),GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,0,NULL);
    if(h!=INVALID_HANDLE_VALUE){ DWORD w; WriteFile(h,buf,n,&w,NULL); CloseHandle(h); }
    printf("[%s] %d%% files=%d/%d bytes=%llu\n",phase,percent,fdone,ftot,bytes);
}
static bool read_request(const std::string& path,uint32_t& app,uint32_t& depot,uint64_t& manifest,std::string& name,std::string& branch){
    HANDLE h=CreateFileW(ghdl::towide(path).c_str(),GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    if(h==INVALID_HANDLE_VALUE) return false;
    DWORD sz=GetFileSize(h,NULL); std::string s(sz,0); DWORD r; ReadFile(h,&s[0],sz,&r,NULL); CloseHandle(h); s.resize(r);
    std::vector<std::string> tok; std::string cur;
    for(char c:s){ if(c==' '||c=='\t'||c=='\r'||c=='\n'){ if(!cur.empty()){tok.push_back(cur);cur.clear();} } else cur+=c; }
    if(!cur.empty())tok.push_back(cur); if(tok.size()<4) return false;
    app=(uint32_t)strtoul(tok[0].c_str(),0,10); depot=(uint32_t)strtoul(tok[1].c_str(),0,10);
    manifest=strtoull(tok[2].c_str(),0,10); name=tok[3]; branch=tok.size()>4?tok[4]:"public"; return true;
}

int main(int argc,char** argv){
    setvbuf(stdout,NULL,_IONBF,0);
    // App mode: resolve depots via PICS and download the whole app.
    //   gh_downloader.exe app <appid> [outdir] [branch]
    // Uses GH_STEAM_TOKEN/GH_STEAM_STEAMID if set (owned games), else anonymous (public apps).
    if(argc>=3 && std::string(argv[1])=="app"){
        uint32_t a=(uint32_t)strtoul(argv[2],0,10); std::string nm=argc>3?argv[3]:"gamedir"; std::string br=argc>4?argv[4]:"public";
        g_dl_name=nm; std::wstring od=ghdl::towide(nm);
        char tk[4096]={0}, sd[64]={0}; DWORD tl=GetEnvironmentVariableA("GH_STEAM_TOKEN",tk,sizeof(tk));
        GetEnvironmentVariableA("GH_STEAM_STEAMID",sd,sizeof(sd)); char ac[128]={0}; GetEnvironmentVariableA("GH_STEAM_ACCOUNT",ac,sizeof(ac));
        std::string tok=(tl>0)?tk:""; uint64_t sid=sd[0]?strtoull(sd,0,10):0;
        ghdl::g_logFn=[](const char* m){ printf("%s\n",m); }; ghdl::g_logBudget=200;
        int rc=ghdl::DownloadApp(a,od,br,tok,sid,ac,on_progress,nullptr);
        printf(rc==0?"\nSUCCESS\n":"\nFAILED rc=%d\n",rc); return rc?1:0;
    }
    uint32_t app,depot; uint64_t manifest; std::string branch,name; std::wstring outdir;
    if(argc>=4){
        app=(uint32_t)strtoul(argv[1],0,10); depot=(uint32_t)strtoul(argv[2],0,10); manifest=strtoull(argv[3],0,10);
        name=argc>4?argv[4]:"gamedir"; branch=argc>5?argv[5]:"public"; outdir=ghdl::towide(name);
    } else if(read_request("gh_download.txt",app,depot,manifest,name,branch)){
        outdir=ghdl::towide("game\\"+name);
    } else { printf("usage: gh_downloader.exe <app> <depot> <manifest> [outdir] [branch]\n   or provide gh_download.txt\n"); return 2; }
    g_dl_name=name;

    char tok[4096]={0}, sid[64]={0}; DWORD tl=GetEnvironmentVariableA("GH_STEAM_TOKEN",tok,sizeof(tok));
    GetEnvironmentVariableA("GH_STEAM_STEAMID",sid,sizeof(sid));
    std::string token=(tl>0)?tok:""; uint64_t token_sid=sid[0]?strtoull(sid,0,10):0;

    int rc=ghdl::DownloadDepot(app,depot,manifest,outdir,branch,token,token_sid,on_progress,nullptr);
    printf(rc==0?"\nSUCCESS\n":"\nFAILED rc=%d\n",rc);
    return rc?1:0;
}
