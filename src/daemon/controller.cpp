/* Manual, reversible ShellUI panel experiment. SPDX-License-Identifier: GPL-3.0-or-later */
#include "protocol.hpp"
#include "broker.hpp"
#include "plugin_file.hpp"
#include "librespot_adapter.hpp"
#include "legacy_focus.hpp"
#include "prologue.hpp"
#include "stable_shellui_injector.hpp"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <cerrno>
#include <initializer_list>
#include <string>
#include <cctype>
#include <ctime>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/mman.h>
extern "C" {
#include <ps5/kernel.h>
#include <ps5/klog.h>
#include <ps5/mdbg.h>
#include <ps5/nid.h>
#include "pt.h"
extern const unsigned char home_bridge_blob[];
extern const unsigned int home_bridge_blob_size;
extern const unsigned char home_lookup_blob[];
extern const unsigned int home_lookup_blob_size;
std::uint64_t sceKernelGetProcessTime();
}
using namespace home_bridge;
static FILE* output;
static std::uint64_t start;
static volatile sig_atomic_t interrupted;
static bool own_stop_marker;
static dev_t stop_device;
static ino_t stop_inode;
static void signal_stop(int s) { interrupted=s; }
static void note(const char* fmt,...) {
    char text[768]; va_list ap; va_start(ap,fmt); std::vsnprintf(text,sizeof text,fmt,ap); va_end(ap);
    auto ms=(sceKernelGetProcessTime()-start)/1000;
    if(output) { std::fprintf(output,"[%llu ms] %s\n",(unsigned long long)ms,text); std::fflush(output); }
    klog_printf("[home-bridge +%llu ms] %s\n",(unsigned long long)ms,text);
}
static bool stopping() { return interrupted||access("/data/custom-overlays/stop.txt",F_OK)==0; }
static bool request_worker_stop() {
    // Direct memory writes can return EPERM. The native worker and UI callback
    // both already observe this file, so cleanup does not depend on that write.
    int fd=open("/data/custom-overlays/stop.txt",O_WRONLY|O_CREAT|O_EXCL,0666);
    if(fd<0) return errno==EEXIST;
    struct stat info{};
    const bool ok=fstat(fd,&info)==0&&write(fd,"parent cleanup\n",15)==15;
    close(fd);
    if(ok) { own_stop_marker=true; stop_device=info.st_dev; stop_inode=info.st_ino; }
    note("worker stop marker created=%u",ok);
    return ok;
}
static void clear_owned_stop_marker() {
    struct stat info{};
    if(own_stop_marker&&!stat("/data/custom-overlays/stop.txt",&info)&&
       info.st_dev==stop_device&&info.st_ino==stop_inode) {
        const int rc=unlink("/data/custom-overlays/stop.txt");
        note("owned cleanup stop marker removed=%u",rc==0);
    }
}
// The live process of that name: stopped, exiting or zombie ones are skipped, and of the rest
// the newest wins. `log` notes every match (a crashed ShellUI can linger, stopped and readable,
// beside the new one the system started: 2026-10-16).
static pid_t process_pid(const char* name,bool log=false) {
    int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_PROC,0}; std::size_t length=0;
    if(sysctl(mib,4,nullptr,&length,nullptr,0)||!length||length>16*1024*1024) return -1;
    auto* bytes=static_cast<char*>(std::malloc(length)); if(!bytes) return -1;
    pid_t result=-1; long long newest=-1;
    if(sysctl(mib,4,bytes,&length,nullptr,0)==0) {
        for(std::size_t i=0;i+sizeof(int)<=length;) {
            auto* p=reinterpret_cast<kinfo_proc*>(bytes+i);
            if(p->ki_structsize<(int)sizeof(kinfo_proc)||p->ki_structsize>(int)(length-i)) break;
            char elf[64]; std::snprintf(elf,sizeof elf,"%s.elf",name);
            if(std::strcmp(p->ki_comm,name)==0||std::strcmp(p->ki_comm,elf)==0) {
                const bool dead=p->ki_stat==SSTOP||p->ki_stat==SZOMB||(p->ki_flag&P_WEXIT);
                const long long start=(long long)p->ki_start.tv_sec*1000000+p->ki_start.tv_usec;
                if(log) note("process %s pid=%d stat=%d flag=0x%x start=%lld usable=%u",name,p->ki_pid,(int)p->ki_stat,
                             (unsigned)p->ki_flag,start,!dead);
                if(!dead&&start>newest) { newest=start; result=p->ki_pid; }
            }
            i+=(std::size_t)p->ki_structsize;
        }
    }
    std::free(bytes); return result;
}
static pid_t shell_pid() { return process_pid("SceShellUI",true); }
#ifdef PROBE_NATIVE
#include "system-stats.inc"
#include "chip-png.inc"
// The System tab's icon, beside the logs where ShellUI reads it (/user/data/custom-overlays).
static void stage_chip() {
    struct stat info{};
    const char* path="/data/custom-overlays/chip.png";
    if(!stat(path,&info)&&info.st_size==(off_t)sizeof chip_png) return;
    const int fd=open("/data/custom-overlays/chip.png.partial",O_WRONLY|O_CREAT|O_TRUNC,0644);
    const bool ok=fd>=0&&write(fd,chip_png,sizeof chip_png)==(ssize_t)sizeof chip_png;
    if(fd>=0) close(fd);
    note("chip icon staged=%u",ok&&rename("/data/custom-overlays/chip.png.partial",path)==0);
}
#endif
#ifdef PROBE_PLAYER
#ifndef PROBE_NATIVE
// ShellUI cannot see /preinst/common/font, so copy the faces Borealis needs
// to font_stage_dir before the worker starts. Done once; later runs skip it.
static bool copy_file(const char* from,const char* to) {
    struct stat a{},b{};
    if(stat(from,&a)) return false;
    if(!stat(to,&b)&&b.st_size==a.st_size) return true;
    char temp[256]; std::snprintf(temp,sizeof temp,"%s.partial",to);
    int in=open(from,O_RDONLY); if(in<0) return false;
    int out=open(temp,O_WRONLY|O_CREAT|O_TRUNC,0644); if(out<0) { close(in); return false; }
    char buffer[65536]; ssize_t n; bool ok=true; off_t total=0;
    while(ok&&(n=read(in,buffer,sizeof buffer))>0) { ok=write(out,buffer,(std::size_t)n)==n; total+=n; }
    ok=ok&&n==0&&total==a.st_size; close(in); ok=close(out)==0&&ok;
    if(ok) ok=rename(temp,to)==0;
    if(!ok) (void)unlink(temp);
    return ok;
}
static bool stage_fonts() {
    if(mkdir(font_stage_dir,0755)&&errno!=EEXIST) { note("font staging mkdir errno=%d",errno); return false; }
    bool ok=true;
    for(const char* face:{"SST-Light.otf","SST-Roman.otf","SST-Medium.otf","SST-Bold.otf"}) {
        char from[256],to[256];
        std::snprintf(from,sizeof from,"/preinst/common/font/%s",face);
        std::snprintf(to,sizeof to,"%s/%s",font_stage_dir,face);
        if(!copy_file(from,to)) { note("font staging failed %s errno=%d",face,errno); ok=false; }
    }
    note("font staging dir=%s ok=%u",font_stage_dir,ok);
    return ok;
}
#endif
// A provider's picture, after its path passed the broker's rules: no symlink below the provider's
// folder, a regular file of 2 MiB or less that starts like a PNG or JPEG. A file that isn't there
// yet (or isn't written yet) passes; the worker copes with missing covers.
static bool art_check(const std::string& dir,const std::string& path) {
    struct stat info{};
    for(std::size_t at=dir.size();at<path.size();) {
        const auto slash=path.find('/',at+1);
        if(lstat(path.substr(0,slash).c_str(),&info)) return errno==ENOENT;
        if(S_ISLNK(info.st_mode)) return false;
        if(slash==std::string::npos) break;
        at=slash;
    }
    if(!S_ISREG(info.st_mode)||info.st_size>2*1024*1024) return false;
    if(info.st_size<4) return true;
    const int fd=open(path.c_str(),O_RDONLY); if(fd<0) return true;
    unsigned char head[4]{}; const bool read_ok=read(fd,head,sizeof head)==(ssize_t)sizeof head; close(fd);
    return !read_ok||(head[0]==0x89&&head[1]=='P'&&head[2]=='N'&&head[3]=='G')||(head[0]==0xFF&&head[1]==0xD8&&head[2]==0xFF);
}
// The panel's link to librespot-ps5 (its control.rs): a loopback socket kept here, outside
// ShellUI. Each state line is left in player_file, which the worker reads as
// /user/data/...; commands come from the worker's State ring, which is only ever read.
// Kabuki's tiles (src/daemon/service.hpp) and, until librespot-ps5 registers its own, its link to
// librespot-ps5's control channel on 127.0.0.1:38574 (src/daemon/librespot_adapter.hpp). Both
// write the files the worker reads; the worker's commands come from its State ring.
class PlayerLink {
public:
    PlayerLink() : service("/data/custom-overlays",monotonic_ms),
                   adapter(service,[this](const std::string& line) { send_line(line); }),
                   broker(service,broker_hooks(),"/data/custom-overlays/art","0.2.0-rc.1") {
        service.load_settings();
        service.set_system_note("Not running");
    }
    ~PlayerLink() {
        disconnect(false);
        for(auto& peer:peers) close(peer.fd);
        if(listener>=0) close(listener);
    }
    void tick(const State& s) {
        const auto now=sceKernelGetProcessTime();
        if(fd<0&&now>=next_attempt) connect_now(now);
        if(fd>=0) receive();
        serve_providers(now);
        commands(s);
        adapter.tick();
        broker.tick();
        flush_providers();
        poll_librespot_stop(now);
    }
    const std::string& status() const { return service.system_note(); }
private:
    static constexpr std::uint16_t port=38574;
    kabuki::Service service;
    kabuki::LibrespotAdapter adapter;
    int fd=-1; std::uint64_t next_attempt=0; std::string input;
    bool seq_known=false; std::uint32_t seq=0; bool logged_absent=false;

    // The provider broker (src/daemon/broker.hpp, protocol/PROTOCOL.md): other programs' tiles
    // over loopback. Kept here with the librespot link, ticked from the same loop.
    static constexpr std::uint16_t provider_port=38590;
    struct Peer { int id; int fd; std::string out; };
    kabuki::Broker broker;
    std::vector<Peer> peers;
    int listener=-1; std::uint64_t next_listen=0; bool logged_listen=false;

    kabuki::Broker::Hooks broker_hooks() {
        kabuki::Broker::Hooks h;
        h.send=[this](int id,const std::string& line) {
            for(auto& peer:peers) if(peer.id==id) { peer.out+=line; peer.out+='\n'; return; }
        };
        h.log=[](const std::string& text) { note("%.200s",text.c_str()); };
        h.art_ok=art_check;
        h.native_provider=[this](const std::string& name){if(name=="librespot")adapter.native_takeover();};
        h.plugin_load=[](const std::string& path)->std::uintptr_t {
            struct stat st{};if(lstat(path.c_str(),&st)||!S_ISREG(st.st_mode))return 0;
            static std::map<std::string,std::uintptr_t> mapped;
            const auto key=path+":"+std::to_string(st.st_size)+":"+std::to_string(st.st_mtime);
            if(auto it=mapped.find(key);it!=mapped.end())return it->second;
            if(mapped.size()>=16)return 0;
            std::vector<std::uint8_t> bytes;std::uint64_t symbol=0;
            if(!kabuki::plugin_file(path,bytes,symbol))return 0;
            int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_PROC,0};std::size_t length=0;
            if(sysctl(mib,4,nullptr,&length,nullptr,0)||!length)return 0;
            std::vector<unsigned char> processes(length);
            if(sysctl(mib,4,processes.data(),&length,nullptr,0))return 0;
            pid_t target=-1;
            for(std::size_t at=0;at+sizeof(kinfo_proc)<=length;) {
                const auto* p=reinterpret_cast<const kinfo_proc*>(processes.data()+at);
                if(!std::strcmp(p->ki_comm,"SceShellUI")){target=p->ki_pid;break;}
                if(p->ki_structsize<0||std::size_t(p->ki_structsize)<sizeof(kinfo_proc)||std::size_t(p->ki_structsize)>length-at)break;at+=p->ki_structsize;
            }
            if(target<0)return 0;
            const auto entry=reinterpret_cast<const Elf64_Ehdr*>(bytes.data())->e_entry;
            const auto address=common_fps::ps5::map_shellui_plugin(target,bytes.data(),entry,symbol);
            if(address)mapped[key]=address;
            note("PLUGIN mapped=%u size=%zu",address!=0,bytes.size());return address;
        };
        return h;
    }
    void listen_now(std::uint64_t now) {
        next_listen=now+2000000;
        const int l=socket(AF_INET,SOCK_STREAM,0); if(l<0) return;
        int yes=1; setsockopt(l,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);
        sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(provider_port); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if(bind(l,reinterpret_cast<sockaddr*>(&a),sizeof a)||listen(l,8)) {
            if(!logged_listen) { note("BROKER cannot listen on 127.0.0.1:%u errno=%d; retrying every 2 s",provider_port,errno); logged_listen=true; }
            close(l); return;
        }
        fcntl(l,F_SETFL,fcntl(l,F_GETFL)|O_NONBLOCK);
        listener=l; logged_listen=false;
        note("BROKER listening on 127.0.0.1:%u",provider_port);
    }
    void close_peer(std::size_t index) {
        broker.closed(peers[index].id);
        close(peers[index].fd);
        peers.erase(peers.begin()+(std::ptrdiff_t)index);
    }
    void serve_providers(std::uint64_t now) {
        if(listener<0&&now>=next_listen) listen_now(now);
        for(;listener>=0;) {
            const int c=accept(listener,nullptr,nullptr);
            if(c<0) break;
            const int id=broker.open();
            if(id<0) { note("BROKER refused a ninth provider"); close(c); continue; }
            fcntl(c,F_SETFL,fcntl(c,F_GETFL)|O_NONBLOCK);
            int yes=1; setsockopt(c,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof yes);
            peers.push_back({id,c,{}});
        }
        for(std::size_t i=0;i<peers.size();) {
            char buffer[4096]; bool gone=false; std::size_t budget=256*1024;
            const int id=peers[i].id;
            while(budget&&broker.alive(id)) {
                const ssize_t n=recv(peers[i].fd,buffer,sizeof buffer,0);
                if(n>0) { broker.data(id,buffer,(std::size_t)n); budget-=std::min<std::size_t>(budget,(std::size_t)n); continue; }
                if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)) break;
                gone=true; break;
            }
            if(gone) { close_peer(i); continue; }
            ++i;
        }
    }
    // What the broker said goes out; a provider that can't keep up, or was dropped, is closed.
    void flush_providers() {
        for(std::size_t i=0;i<peers.size();) {
            auto& peer=peers[i];
            bool failed=false;
            while(!peer.out.empty()) {
                const ssize_t n=send(peer.fd,peer.out.data(),peer.out.size(),0);
                if(n>0) { peer.out.erase(0,(std::size_t)n); continue; }
                if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)) break;
                failed=true; break;
            }
            if(failed||!broker.alive(peer.id)||peer.out.size()>kabuki::kBrokerBuffer) close_peer(i); else ++i;
        }
    }
    static std::uint64_t monotonic_ms() {
        timespec t{}; clock_gettime(CLOCK_MONOTONIC,&t);
        return std::uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;
    }
    void connect_now(std::uint64_t now) {
        next_attempt=now+2000000;
        fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0) return;
        sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(port); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        // Loopback connects or is refused at once.
        if(connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof a)) {
            if(!logged_absent) { note("LINK librespot-ps5 not reachable on 127.0.0.1:%u errno=%d; retrying every 2 s",port,errno); logged_absent=true; }
            close(fd); fd=-1; return;
        }
        fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)|O_NONBLOCK);
        int yes=1; setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof yes);
        note("LINK connected to librespot-ps5"); logged_absent=false; input.clear();
        adapter.connected();
    }
    void disconnect(bool tell) {
        if(fd<0) return;
        close(fd); fd=-1; input.clear();
        if(tell) note("LINK librespot-ps5 disconnected");
        adapter.disconnected();
    }
    void receive() {
        char buffer[4096];
        for(;;) {
            const ssize_t n=recv(fd,buffer,sizeof buffer,0);
            if(n>0) { input.append(buffer,(std::size_t)n); if(input.size()>262144) { disconnect(true); return; } continue; }
            if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)) break;
            disconnect(true); return;
        }
        // Library and track lists each matter; of the state lines only the newest.
        const auto end=input.rfind('\n'); if(end==std::string::npos) return;
        std::string newest_state;
        for(std::size_t from=0;from<=end;) {
            const auto stop=input.find('\n',from);
            std::string line=input.substr(from,stop-from);
            from=stop+1;
            if(line.empty()||line.front()!='{') continue;
            if(!line.compare(0,10,"{\"library\"")||!line.compare(0,9,"{\"tracks\"")) adapter.line(line);
            else newest_state=std::move(line);
        }
        input.erase(0,end+1);
        if(!newest_state.empty()) adapter.line(newest_state);
    }
    void send_line(const std::string& line) {
        if(fd<0) { note("LINK command dropped: librespot-ps5 not connected"); return; }
        const std::string out=line+"\n";
        if(send(fd,out.data(),out.size(),0)!=(ssize_t)out.size()) { disconnect(true); return; }
        note("LINK sent %.120s",line.c_str());
    }
    void commands(const State& s) {
        if(!seq_known) { seq=s.command_seq; seq_known=true; return; } // Not earlier sessions' presses.
        if(s.command_seq-seq>8) seq=s.command_seq-8;
        for(;seq!=s.command_seq;++seq) {
            char text[129]{}; std::memcpy(text,s.command_text[seq%8],128);
            const int kind=s.command_kind[seq%8],value=s.command_value[seq%8];
            if(kind==kabuki::kTerminateLibrespot) { terminate_librespot(); continue; }
            service.command(kind,value,text);
            if(kind==kabuki::kSaveSettings||kind==kabuki::kSaveSlots) note("SETTINGS kind=%d %.100s",kind,text);
        }
    }
    void terminate_librespot() {
        const pid_t pid=process_pid("librespot-ps5");
        if(pid<=1||pid==getpid()) {
            service.set_system_note("Not running");
            note("LIBRESPOT stop skipped: target process not found");
            return;
        }
        if(kill(pid,SIGTERM)==0) {
            stopping_pid=pid;
            stop_deadline=sceKernelGetProcessTime()+5000000;
            next_stop_check=0;
            service.set_system_note("Stopping...");
            note("LIBRESPOT SIGTERM sent pid=%d",pid);
        } else {
            const int error=errno;
            service.set_system_note(error==ESRCH?"Not running":"Stop failed");
            note("LIBRESPOT SIGTERM failed pid=%d errno=%d",pid,error);
        }
    }
    void poll_librespot_stop(std::uint64_t now) {
        if(stopping_pid<=1||now<next_stop_check) return;
        next_stop_check=now+200000;
        const pid_t current=process_pid("librespot-ps5");
        if(current!=stopping_pid) {
            stopping_pid=-1;
            service.set_system_note("Not running");
        } else if(now>=stop_deadline) {
            stopping_pid=-1;
            service.set_system_note("Still running");
            note("LIBRESPOT did not exit after SIGTERM pid=%d",current);
        } else service.set_system_note("Stopping...");
    }
    pid_t stopping_pid=-1;
    std::uint64_t stop_deadline=0,next_stop_check=0;
};
#endif
static bool ready(Ready& r,pid_t pid) {
    FILE* f=std::fopen(ready_path,"rb"); if(!f) return false;
    bool ok=std::fread(&r,1,sizeof r,f)==sizeof r&&std::fgetc(f)==EOF; std::fclose(f);
    bool valid=ok&&r.state.signature==magic&&(r.state.version==protocol_version||(!input_mode&&r.state.version==2))&&r.state.pid==pid&&
        user_address(r.address)&&user_address(r.state.update)&&user_address(r.state.hook)&&user_address(r.state.trampoline);
#ifdef PROBE_INPUT
    valid=valid&&user_address(r.state.key_method)&&user_address(r.state.key_hook)&&user_address(r.state.key_trampoline)&&
        r.state.key_method!=r.state.update&&r.state.key_trampoline!=r.state.trampoline;
#endif
    return valid;
}
static bool read_state(const Ready& r,State& s) {
    bool valid=mdbg_copyout(r.state.pid,(intptr_t)r.address,&s,sizeof s)==0&&
        s.signature==magic&&s.version==r.state.version&&s.pid==r.state.pid&&s.update==r.state.update&&
        s.hook==r.state.hook&&s.trampoline==r.state.trampoline;
#ifdef PROBE_INPUT
    valid=valid&&s.key_method==r.state.key_method&&s.key_hook==r.state.key_hook&&s.key_trampoline==r.state.key_trampoline;
#endif
    return valid;
}
#ifndef PROBE_BUILD_ID
#define PROBE_BUILD_ID 0ULL
#endif
// Older markers hold only the pid; build then reads as 0 (unknown).
struct Attempt { pid_t pid=0; std::uint32_t version=0; std::uint64_t build=0; };
static Attempt read_attempt() {
    Attempt a{}; FILE* f=std::fopen(attempt_path,"rb"); if(!f) return a;
    if(std::fread(&a,1,sizeof a,f)<sizeof a.pid) a=Attempt{};
    std::fclose(f); return a;
}
static bool already_attempted(pid_t pid) { return read_attempt().pid==pid; }
static bool mark_attempt(pid_t pid) {
    FILE* f=std::fopen(attempt_path,"wb"); if(!f) return false;
    const Attempt a{pid,protocol_version,PROBE_BUILD_ID};
    bool ok=std::fwrite(&a,1,sizeof a,f)==sizeof a;
    return std::fclose(f)==0&&ok;
}
#ifdef PROBE_DEV_REPLACE
#if !defined(PROBE_INPUT)
#error "PROBE_DEV_REPLACE reads the input hook fields of the worker state"
#endif
// DEVELOPMENT ONLY (OVERLAY_DEV_REPLACE=1 build.sh borealis). Lets a new worker
// build load into a ShellUI that still holds an older, idle one, instead of
// requiring a reboot. The old worker's code and threads stay resident (~4 MB
// each) until ShellUI restarts. Never ship a build with this enabled.
//
// Old workers may use another State layout, so only the prefix through
// key_hook is read. That prefix has been unchanged since v7; keep it stable.
static_assert(offsetof(State,key_method)==80&&offsetof(State,key_hook)==88);
struct ResidentPrefix { std::uint64_t address,update,hook,key_method,key_hook; std::uint32_t version; };
static bool resident_prefix(pid_t pid,ResidentPrefix& p) {
    unsigned char raw[sizeof(std::uint64_t)+offsetof(State,key_hook)+sizeof(std::uint64_t)];
    FILE* f=std::fopen(ready_path,"rb"); if(!f) return false;
    bool ok=std::fread(raw,1,sizeof raw,f)==sizeof raw; std::fclose(f);
    if(!ok) return false;
    const unsigned char* s=raw+sizeof(std::uint64_t);
    std::uint64_t signature; std::int32_t owner;
    std::memcpy(&p.address,raw,8); std::memcpy(&signature,s+offsetof(State,signature),8);
    std::memcpy(&p.version,s+offsetof(State,version),4); std::memcpy(&owner,s+offsetof(State,pid),4);
    std::memcpy(&p.update,s+offsetof(State,update),8); std::memcpy(&p.hook,s+offsetof(State,hook),8);
    std::memcpy(&p.key_method,s+offsetof(State,key_method),8); std::memcpy(&p.key_hook,s+offsetof(State,key_hook),8);
    return signature==magic&&owner==pid&&user_address(p.address)&&user_address(p.update)&&
        user_address(p.hook)&&user_address(p.key_method)&&user_address(p.key_hook);
}
// A resident worker from another build: true when this build should replace it.
static bool resident_from_other_build(pid_t pid) {
    ResidentPrefix p; if(!resident_prefix(pid,p)) return false;
    const auto a=read_attempt();
    return p.version!=protocol_version||a.pid!=pid||a.build!=PROBE_BUILD_ID;
}
static bool hooked(pid_t pid,std::uint64_t method) {
    static const unsigned char absolute_jump[]={0xff,0x25,0,0,0,0};
    unsigned char bytes[sizeof absolute_jump];
    return mdbg_copyout(pid,(intptr_t)method,bytes,sizeof bytes)||!std::memcmp(bytes,absolute_jump,sizeof bytes);
}
static bool resident_idle(pid_t pid) {
    ResidentPrefix p; if(!resident_prefix(pid,p)) return false;
    unsigned char live[offsetof(State,removed)+sizeof(std::uint32_t)];
    if(mdbg_copyout(pid,(intptr_t)p.address,live,sizeof live)) return false;
    std::uint64_t signature,update,hook; std::uint32_t phase,removed; std::int32_t owner;
    std::memcpy(&signature,live+offsetof(State,signature),8); std::memcpy(&owner,live+offsetof(State,pid),4);
    std::memcpy(&update,live+offsetof(State,update),8); std::memcpy(&hook,live+offsetof(State,hook),8);
    std::memcpy(&phase,live+offsetof(State,phase),4); std::memcpy(&removed,live+offsetof(State,removed),4);
    const bool consistent=signature==magic&&owner==pid&&update==p.update&&hook==p.hook;
    const bool parked=phase==Prepared||((phase==Done||phase==Failed)&&removed);
    const bool unhooked=!hooked(pid,p.update)&&!hooked(pid,p.key_method);
    note("DEV resident worker version=%u phase=%u removed=%u consistent=%u unhooked=%u",
         p.version,phase,removed,consistent,unhooked);
    return consistent&&parked&&unhooked;
}
#endif
static void jump(unsigned char* bytes,std::uint64_t to) {
    const unsigned char prefix[]={0xff,0x25,0,0,0,0}; std::memcpy(bytes,prefix,6); std::memcpy(bytes+6,&to,8);
}
static bool migrate_focus_property(const Ready& r) {
    if(r.state.version!=2) return true;
    // Only our exact known v9 worker is eligible. ShellUI is stopped and its
    // original Update bytes have matched before this is called. Nothing in the
    // managed firmware is modified: this changes our own property-name data.
    const auto base=r.state.trampoline;
    if(r.state.hook!=base+legacy_hook_offset||r.address!=base+legacy_state_offset) {
        note("worker migration refused: layout mismatch; fresh ShellUI required"); return false;
    }
    unsigned char code[sizeof legacy_hook_code];
    if(mdbg_copyout(r.state.pid,(intptr_t)r.state.hook,code,sizeof code)||
       std::memcmp(code,legacy_hook_code,sizeof code)) {
        note("worker migration refused: code fingerprint mismatch; fresh ShellUI required"); return false;
    }
    State s;
    if(!read_state(r,s)||(s.phase!=Prepared&&((s.phase!=Failed&&s.phase!=Done)||!s.removed))) {
        note("worker migration refused: worker not idle"); return false;
    }
    constexpr char old_name[]="NodeFocusable";
    constexpr char fixed_name[sizeof old_name]="Focusable";
    char actual[sizeof old_name];
    const auto address=base+legacy_focus_offset;
    if(mdbg_copyout(r.state.pid,(intptr_t)address,actual,sizeof actual)) return false;
    if(!std::memcmp(actual,fixed_name,sizeof actual)) {
        note("worker Focusable migration already applied"); return true;
    }
    if(std::memcmp(actual,old_name,sizeof actual)) {
        note("worker migration refused: property data mismatch"); return false;
    }
    int rc=pt_copyin(r.state.pid,fixed_name,(intptr_t)address,sizeof fixed_name);
    bool verified=!rc&&!mdbg_copyout(r.state.pid,(intptr_t)address,actual,sizeof actual)&&
        !std::memcmp(actual,fixed_name,sizeof actual);
    note("worker property migration NodeFocusable -> Focusable ptrace_rc=%d verified=%u",rc,verified);
    if(!verified) {
        rc=pt_copyin(r.state.pid,old_name,(intptr_t)address,sizeof old_name);
        bool rolled_back=!rc&&!mdbg_copyout(r.state.pid,(intptr_t)address,actual,sizeof actual)&&
            !std::memcmp(actual,old_name,sizeof actual);
        note("worker property migration rollback=%u",rolled_back);
    }
    return verified;
}
static bool write_code(pid_t pid,std::uint64_t addr,const void* data,std::size_t size);
static std::uint64_t mono_export(pid_t pid,const char* name) {
    const char* libraries[]={"libmonosgen-2.0.sprx","libmonosgen-2.0.0.sprx"};
    for(const char* library:libraries) {
        std::uint32_t handle=0;
        if(kernel_dynlib_handle(pid,library,&handle)<0||!handle) continue;
        auto address=(std::uint64_t)kernel_dynlib_dlsym(pid,handle,name);
        if(user_address(address)) return address;
        char nid[12]{};
        if(nid_encode(name,nid)) {
            address=(std::uint64_t)kernel_dynlib_resolve(pid,handle,nid);
            if(user_address(address)) return address;
        }
    }
    return 0;
}
static bool migrate_inherited_lookup(const Ready& r) {
    if(r.state.version!=2) return true;
    // Called only after migrate_focus_property verifies the exact idle v9
    // worker. Rebind its own import slot; no global Mono binding is changed.
    if(home_lookup_blob_size!=77||legacy_lookup_shim_offset+home_lookup_blob_size>128) return false;
    const auto base=r.state.trampoline;
    const auto pid=r.state.pid;
    const auto lookup=mono_export(pid,"mono_class_get_method_from_name");
    const auto parent=mono_export(pid,"mono_class_get_parent");
    std::uint64_t current=0,parent_slot=0;
    if(!lookup||!parent||
       mdbg_copyout(pid,(intptr_t)(base+legacy_lookup_slot),&current,sizeof current)||
       mdbg_copyout(pid,(intptr_t)(base+legacy_parent_slot),&parent_slot,sizeof parent_slot)||
       parent_slot!=parent) {
        note("worker lookup migration refused: runtime binding mismatch"); return false;
    }
    unsigned char code[77],observed[77];
    std::memcpy(code,home_lookup_blob,sizeof code);
    std::memcpy(code+sizeof code-16,&lookup,8);
    std::memcpy(code+sizeof code-8,&parent,8);
    const auto address=base+legacy_lookup_shim_offset;
    if(current==address) {
        bool verified=!mdbg_copyout(pid,(intptr_t)address,observed,sizeof observed)&&!std::memcmp(code,observed,sizeof code);
        note("worker inherited lookup already installed verified=%u",verified); return verified;
    }
    if(current!=lookup) { note("worker lookup migration refused: unexpected import value"); return false; }
    if(!write_code(pid,address,code,sizeof code)) return false;
    int rc=pt_copyin(pid,&address,(intptr_t)(base+legacy_lookup_slot),sizeof address);
    std::uint64_t check=0;
    bool verified=!rc&&!mdbg_copyout(pid,(intptr_t)(base+legacy_lookup_slot),&check,sizeof check)&&check==address;
    note("worker inherited lookup import ptrace_rc=%d verified=%u",rc,verified);
    if(!verified) {
        rc=pt_copyin(pid,&lookup,(intptr_t)(base+legacy_lookup_slot),sizeof lookup);
        bool rollback=!rc&&!mdbg_copyout(pid,(intptr_t)(base+legacy_lookup_slot),&check,sizeof check)&&check==lookup;
        note("worker inherited lookup import rollback=%u",rollback);
    }
    return verified;
}
static bool write_code(pid_t pid,std::uint64_t addr,const void* data,std::size_t size) {
    if(!user_address(addr)||!size||size>128||!user_address(addr+size-1)) return false;
    unsigned char check[128];
    // The embedded trampoline is already writable/executable. Try the direct
    // write first, as the upstream high-firmware hook controller does, avoiding
    // a needless protection change on an already writable mapping.
    int rc=mdbg_copyin(pid,data,(intptr_t)addr,size);
    note("code write addr=%llx size=%zu direct_rc=%d errno=%d",(unsigned long long)addr,size,rc,errno);
    bool protection=true;
    if(rc) {
    const std::uint64_t page=addr&~0x3fffULL,end=(addr+size+0x3fffULL)&~0x3fffULL;
    int protect_rc=kernel_mprotect(pid,(intptr_t)page,end-page,PROT_READ|PROT_WRITE|PROT_EXEC);
    note("code protection addr=%llx rwx_rc=%d errno=%d",(unsigned long long)page,protect_rc,errno);
    if(protect_rc) return false;
    rc=mdbg_copyin(pid,data,(intptr_t)addr,size);
    note("code protected write mdbg_rc=%d errno=%d",rc,errno);
    if(rc) {
        rc=pt_copyin(pid,data,(intptr_t)addr,size);
        note("code protected write ptrace_rc=%d errno=%d",rc,errno);
    }
    protect_rc=kernel_mprotect(pid,(intptr_t)page,end-page,PROT_READ|PROT_EXEC);
    protection=protect_rc==0;
    note("code protection rx_rc=%d errno=%d",protect_rc,errno);
    }
    int read_rc=mdbg_copyout(pid,(intptr_t)addr,check,size);
    if(read_rc) read_rc=pt_copyout(pid,(intptr_t)addr,check,size);
    bool equal=!read_rc&&std::memcmp(check,data,size)==0;
    note("code readback read_rc=%d equal=%u protection_ok=%u",read_rc,equal,protection);
    return !rc&&protection&&equal;
}
struct InputPatch {
    std::uint64_t method=0,hook=0,trampoline_address=0;
    std::size_t length=0;
    unsigned char original[16]{},desired[16]{},trampoline[128]{};
    bool installed=false,restored=false;
};
static bool disarm_input(const Ready& r) {
#ifdef PROBE_INPUT
    std::uint32_t zero=0,check=1;
    int rc=pt_copyin(r.state.pid,&zero,(intptr_t)(r.address+offsetof(State,input_active)),sizeof zero);
    bool verified=!rc&&!mdbg_copyout(r.state.pid,(intptr_t)(r.address+offsetof(State,input_active)),&check,sizeof check)&&!check;
    note("input gate disarmed verified=%u",verified); return verified;
#else
    (void)r; return true;
#endif
}
static bool patch_input(const Ready& r,InputPatch& p,bool install) {
    if(!disarm_input(r)) return false;
    unsigned char current[16];
    if(mdbg_copyout(r.state.pid,(intptr_t)p.method,current,sizeof current)) return false;
    if(!install&&!std::memcmp(current,p.original,sizeof current)) {
        p.installed=false; p.restored=true; note("input hook already original"); return true;
    }
    const auto* expected=install?p.original:p.desired;
    const auto* desired=install?p.desired:p.original;
    if(std::memcmp(current,expected,sizeof current)) { note("input patch refused: target bytes changed"); return false; }
    bool ok=write_code(r.state.pid,p.method,desired,16);
    if(install) {
        p.installed=ok;
        if(!ok) {
            p.restored=write_code(r.state.pid,p.method,p.original,16);
            note("input patch failed rollback_verified=%u",p.restored);
        }
    } else { p.restored=ok; if(ok) p.installed=false; }
    note("input hook install=%u verified=%u",install,ok); return ok;
}
static bool transaction(const Ready& r,const unsigned char* expected,const unsigned char* desired,
                        const unsigned char* trampoline,std::size_t length,bool arm,InputPatch* input=nullptr) {
    const pid_t pid=r.state.pid,self=getpid(); const auto auth=kernel_get_ucred_authid(self);
    if(!auth||kernel_set_ucred_authid(self,0x4800000000010003ULL)) return false;
    bool attached=pt_attach(pid)==0,ok=false;
    if(attached) {
        bool input_ok=!input||arm||patch_input(r,*input,false);
        unsigned char current[16];
        if(mdbg_copyout(pid,(intptr_t)r.state.update,current,16)==0&&std::memcmp(current,expected,16)==0) {
            bool prepared=true;
            if(arm) {
                prepared=migrate_focus_property(r);
                if(prepared) prepared=migrate_inherited_lookup(r);
                if(prepared) prepared=write_code(pid,r.state.trampoline,trampoline,length+14);
                if(prepared&&input) prepared=write_code(pid,input->trampoline_address,input->trampoline,input->length+14);
                note("prepare trampoline verified=%u",prepared);
                State s;
                if(prepared) {
                    prepared=read_state(r,s);
                    note("prepare state read=%u",prepared);
                }
                if(prepared) {
                    s.original=r.state.trampoline; s.phase=Armed; s.stop=0; s.seconds=session_seconds;
                    s.removed=0; s.started=0; s.frames=0;
#ifdef PROBE_INPUT
                    s.key_original=r.state.key_trampoline;
                    s.input_started=s.input_deadline=s.input_released=0;
                    s.input_active=0; s.release_reason=NoRelease;
                    s.events_seen=s.events_consumed=0;
                    s.last_key=s.last_type=s.last_device=s.selection=s.enter_count=0;
#ifdef PROBE_TOGGLE
                    s.pad_samples=s.toggles=s.ui_heartbeat=0;
                    s.pad_buttons=s.pad_port=s.pad_connected=0; s.pad_rc=0;
#ifdef PROBE_PLAYER
                    s.surface_frames=s.surface_uploads=s.render_max_us=s.upload_max_us=0;
#endif
#endif
#endif
                    int rc=mdbg_copyin(pid,&s,(intptr_t)r.address,sizeof s);
                    note("prepare state write mdbg_rc=%d errno=%d",rc,errno);
                    if(rc) {
                        rc=pt_copyin(pid,&s,(intptr_t)r.address,sizeof s);
                        note("prepare state write ptrace_rc=%d errno=%d",rc,errno);
                    }
                    State check;
                    prepared=rc==0&&read_state(r,check)&&std::memcmp(&check,&s,sizeof s)==0;
                    note("prepare state verified=%u",prepared);
                }
            }
            if(prepared&&arm&&input) prepared=patch_input(r,*input,true);
            if(prepared) {
                ok=write_code(pid,r.state.update,desired,16);
                if(!ok) {
                    bool rollback=write_code(pid,r.state.update,expected,16);
                    note("patch failed rollback_verified=%u",rollback);
                    if(arm&&rollback) {
                        State s;
                        if(read_state(r,s)) {
                            s.phase=Prepared; s.original=0; s.removed=1;
                            (void)pt_copyin(pid,&s,(intptr_t)r.address,sizeof s);
                        }
                    }
                }
            }
        } else note("patch refused: target bytes changed");
        if(arm&&!ok&&input) input_ok=patch_input(r,*input,false);
        if(arm&&!ok&&input_ok) {
            unsigned char check[16]; State s;
            if(!mdbg_copyout(pid,(intptr_t)r.state.update,check,sizeof check)&&
               !std::memcmp(check,expected,sizeof check)&&read_state(r,s)) {
                s.phase=Prepared; s.removed=1; s.original=0;
                (void)pt_copyin(pid,&s,(intptr_t)r.address,sizeof s);
            }
        }
        ok=ok&&input_ok;
        bool detached=pt_detach(pid,0)==0; (void)kill(pid,SIGCONT);
        note("transaction arm=%u patched=%u detached=%u",arm,ok,detached); ok=ok&&detached;
    }
    bool restored=kernel_set_ucred_authid(self,auth)==0&&kernel_get_ucred_authid(self)==auth;
    note("transaction authority_restored=%u",restored); return ok&&restored;
}
#ifdef PROBE_DEV_REPLACE
// DEVELOPMENT ONLY. A worker whose parent died (2026-10-16: the System sampler crashed it) keeps
// running and hooked, and refused every later build until a reboot. With the lock ours, no
// parent owns it: ask it to stop (its stop file), wait for its scene to go, then put back the
// hooked methods' first bytes. Each trampoline holds them, followed by a jump back to the method
// plus their length; the 16 bytes written at each method keep the rest of the originals.
static bool unhook_from_trampoline(pid_t pid,std::uint64_t method,std::uint64_t trampoline,const char* what) {
    if(!user_address(method)||!user_address(trampoline)) { note("DEV orphan %s: no hook recorded",what); return true; }
    if(!hooked(pid,method)) { note("DEV orphan %s already original",what); return true; }
    unsigned char t[64],current[16];
    if(mdbg_copyout(pid,(intptr_t)trampoline,t,sizeof t)||mdbg_copyout(pid,(intptr_t)method,current,sizeof current)) return false;
    std::size_t length=0;
    for(std::size_t n=14;n<=16&&!length;++n) {
        static const unsigned char jump_prefix[]={0xff,0x25,0,0,0,0};
        std::uint64_t to=0; std::memcpy(&to,t+n+6,8);
        if(!std::memcmp(t+n,jump_prefix,sizeof jump_prefix)&&to==method+n) length=n;
    }
    if(!length) { note("DEV orphan %s: trampoline not recognised",what); return false; }
    unsigned char restored[16]; std::memcpy(restored,current,16); std::memcpy(restored,t,length);
    const bool ok=write_code(pid,method,restored,16)&&!hooked(pid,method);
    note("DEV orphan %s unhooked=%u length=%zu",what,ok,length);
    return ok;
}
static bool retire_orphan(pid_t pid) {
    Ready r{}; State s{};
    if(!ready(r,pid)||!read_state(r,s)) { note("DEV orphan unreadable"); return false; }
    const auto parked=[&s] { return (s.phase==Done||s.phase==Failed)&&s.removed; };
    note("DEV orphan worker phase=%u removed=%u: retiring it (its parent is gone)",s.phase,s.removed);
    if(!parked()) {
        const int fd=open("/data/custom-overlays/stop.txt",O_WRONLY|O_CREAT|O_EXCL,0666);
        const bool ours=fd>=0;
        if(fd>=0) { (void)!write(fd,"orphan\n",7); close(fd); }
        const auto until=sceKernelGetProcessTime()+10000000;
        while(sceKernelGetProcessTime()<until&&read_state(r,s)&&!parked()) usleep(100000);
        if(ours) unlink("/data/custom-overlays/stop.txt");
        if(!parked()) { note("DEV orphan did not stop phase=%u removed=%u",s.phase,s.removed); return false; }
    }
    const pid_t self=getpid(); const auto auth=kernel_get_ucred_authid(self);
    if(!auth||kernel_set_ucred_authid(self,0x4800000000010003ULL)) return false;
    bool ok=false;
    if(pt_attach(pid)==0) {
        ok=unhook_from_trampoline(pid,s.update,s.trampoline,"update");
        ok=unhook_from_trampoline(pid,s.key_method,s.key_trampoline,"input")&&ok;
        const bool detached=pt_detach(pid,0)==0; (void)kill(pid,SIGCONT);
        ok=ok&&detached;
    }
    const bool restored=kernel_set_ucred_authid(self,auth)==0;
    note("DEV orphan retired=%u authority_restored=%u",ok,restored);
    return ok&&restored;
}
#endif
#ifdef PROBE_INPUT
static bool prepare_input_patch(const Ready& r,InputPatch& p) {
    p.method=r.state.key_method; p.hook=r.state.key_hook; p.trampoline_address=r.state.key_trampoline;
    if(mdbg_copyout(r.state.pid,(intptr_t)p.method,p.original,sizeof p.original)) {
        note("cannot read key retrieval prologue"); return false;
    }
    p.length=native_patch_length(p.original);
    note("input hook method=%llx hook=%llx trampoline=%llx displaced=%zu first=%02x%02x%02x%02x%02x%02x%02x%02x",
        (unsigned long long)p.method,(unsigned long long)p.hook,(unsigned long long)p.trampoline_address,p.length,
        p.original[0],p.original[1],p.original[2],p.original[3],p.original[4],p.original[5],p.original[6],p.original[7]);
    if(!p.length) { note("input prologue unsupported or already hooked; no capture started"); return false; }
    std::memcpy(p.trampoline,p.original,p.length); jump(p.trampoline+p.length,p.method+p.length);
    std::memcpy(p.desired,p.original,sizeof p.desired); jump(p.desired,p.hook);
    for(std::size_t i=14;i<p.length;++i) p.desired[i]=0x90;
    return true;
}
#endif
#ifdef PROBE_CAPTURE
#include "capture-parent.inc"
#endif
int main() {
    start=sceKernelGetProcessTime();
    if(mkdir("/data/custom-overlays",0777)&&errno!=EEXIST) return 1;
    int lock=open("/data/custom-overlays/probe.lock",O_CREAT|O_RDWR,0666);
    if(lock<0) return 1;
#ifdef PROBE_DEV_REPLACE
    // DEVELOPMENT ONLY: a probe that is still running (an earlier build) used to make a new
    // launch exit at once, leaving the old panel up (2026-10-15). Ask it to stop with its own
    // stop file, wait for it to let go of the lock, then take over. A stop file left with no
    // probe running is stale and would stop this one at once, so it goes too.
    bool took_over=false,stale_stop=false;
    int killed=0;
    if(flock(lock,LOCK_EX|LOCK_NB)) {
        const int fd=open("/data/custom-overlays/stop.txt",O_WRONLY|O_CREAT|O_EXCL,0666);
        const bool ours=fd>=0;
        if(fd>=0) { (void)!write(fd,"dev takeover\n",13); close(fd); }
        const auto until=sceKernelGetProcessTime()+30000000,grace=sceKernelGetProcessTime()+8000000;
        pid_t stuck=-1;
        while(!(took_over=flock(lock,LOCK_EX|LOCK_NB)==0)&&sceKernelGetProcessTime()<until) {
            // Still there after the grace period: its ShellUI cannot tick (a crashed one never
            // removes the scene), so it never lets go. Its pid is in its log's last BEGIN line.
            if(stuck<0&&sceKernelGetProcessTime()>=grace) {
                stuck=0;
                if(FILE* f=std::fopen("/data/custom-overlays/probe.log","rb")) {
                    char line[512]; int pid=0;
                    while(std::fgets(line,sizeof line,f))
                        if(const char* at=std::strstr(line,"] BEGIN version=")) if(const char* p=std::strstr(at," pid=")) pid=std::atoi(p+5);
                    std::fclose(f);
                    if(pid>1&&pid!=getpid()&&kill(pid,SIGKILL)==0) stuck=pid;
                }
            }
            usleep(200000);
        }
        killed=stuck>0?stuck:0;
        if(ours||took_over) unlink("/data/custom-overlays/stop.txt");
        if(!took_over) { close(lock); return 1; }
    } else stale_stop=unlink("/data/custom-overlays/stop.txt")==0;
#else
    if(flock(lock,LOCK_EX|LOCK_NB)) { close(lock); return 1; }
#endif
    output=std::fopen("/data/custom-overlays/probe.log","a"); if(!output) { close(lock); return 1; }
#ifdef PROBE_DEV_REPLACE
    if(took_over||stale_stop) note("DEV took_over_running_probe=%u killed_stuck_probe_pid=%d removed_stale_stop=%u",took_over,killed,stale_stop);
#endif
    std::signal(SIGINT,signal_stop); std::signal(SIGTERM,signal_stop);
    note("BEGIN version=%u variant=%s pid=%d duration_s=%u",
#ifdef PROBE_PLAYER
         21U,
#elif defined(PROBE_CAPTURE)
         16U,
#elif defined(PROBE_TOGGLE)
         13U,
#else
         input_mode?12U:11U,
#endif
#ifdef PROBE_PLAYER
         variant,
#elif defined(PROBE_CAPTURE)
         "capture-probe",
#else
         variant,
#endif
         getpid(),session_seconds);
#ifdef PROBE_INPUT
#ifdef PROBE_TOGGLE
#ifdef PROBE_CAPTURE
    note("toggle L3+R3 hold_ms=%llu limit_ms=%llu session_s=%u game_focus_lease=1 guardian=native_parent clocks=relative",
         (unsigned long long)(chord_hold_us/1000),(unsigned long long)(input_limit_us/1000),session_seconds);
#else
    note("toggle L3+R3 hold_ms=2000 limit_ms=10000 session_s=120 raw_observe=1 game_exclusive=0");
#endif
#else
    note("input delay_ms=5000 limit_ms=10000 guardian_ms=500 release=deadline+Back+stop");
#endif
#endif
#ifdef PROBE_DEV_REPLACE
    note("DEV replace mode ON build=%016llx; development only",(unsigned long long)PROBE_BUILD_ID);
#endif
    int failed=1;
    do {
        std::uint32_t sdk=0; std::size_t len=sizeof sdk;
        // Firmware is diagnostic only. Resolve and validate the live methods and
        // hook prologues below instead of gating experiments by a version list.
        if(sysctlbyname("kern.sdk_version",&sdk,&len,nullptr,0)||len!=sizeof sdk) {
            note("firmware SDK query unavailable; continuing with runtime checks");sdk=0;
        }
        pid_t pid=shell_pid(); note("target SceShellUI pid=%d sdk=0x%08x",pid,sdk);
        if(pid<=1||stopping()) break;
        Ready r{}; State s{};
        bool resident=ready(r,pid),replacing=false;
#ifdef PROBE_DEV_REPLACE
        if(resident_from_other_build(pid)) {
            if(!resident_idle(pid)&&!(retire_orphan(pid)&&resident_idle(pid))) {
                note("DEV replace refused: resident worker is not idle; reboot required"); break;
            }
            note("DEV replacing idle resident worker with build=%016llx; old code stays until ShellUI restarts",
                 (unsigned long long)PROBE_BUILD_ID);
            resident=false; replacing=true;
        }
#endif
        if(resident) {
            if(!read_state(r,s)||(s.phase!=Prepared&&((s.phase!=Done&&s.phase!=Failed)||!s.removed))) {
                note("existing bridge is not cleanly idle; no second worker started"); break;
            }
            note("reusing resident bridge worker phase=%u frames=%llu",s.phase,(unsigned long long)s.frames);
        } else {
            if(!replacing&&r.state.signature==magic&&r.state.pid==pid&&r.state.version!=protocol_version) {
                note("resident worker version=%u required=%u; restart ShellUI or reboot before this revision",
                     r.state.version,protocol_version); break;
            }
#if defined(PROBE_PLAYER) && !defined(PROBE_NATIVE)
            if(!stage_fonts()) break;
#endif
#ifdef PROBE_NATIVE
            stage_chip();
#endif
#ifdef PROBE_DEV_REPLACE
            // A worker that never published readiness was never armed (hooks are installed
            // only from its ready file), so it is inert. Another build may load beside it.
            if(!replacing&&already_attempted(pid)&&read_attempt().build!=PROBE_BUILD_ID) {
                note("DEV earlier build=%016llx never became ready and was never armed; loading this build beside it",
                     (unsigned long long)read_attempt().build);
                replacing=true;
            }
#endif
            if(!replacing&&already_attempted(pid)) {
                note("an earlier loader attempt lacks valid readiness; restart ShellUI or reboot before retrying"); break;
            }
            if(!mark_attempt(pid)) { note("cannot record loader attempt; aborting"); break; }
            (void)unlink(ready_path);
            auto result=common_fps::ps5::inject_shellui_stable(pid,home_bridge_blob,home_bridge_blob_size);
            note("loader mapped=%u thread=%u detached=%u auth_restored=%u unresolved=%u first=%s",
                 result.elf_loaded,result.pthread_create_ok,result.detached,result.auth_restored,
                 result.imports_unresolved,result.first_unresolved?result.first_unresolved:"");
            if(!result.elf_loaded||!result.pthread_create_ok||!result.detached||!result.auth_restored||result.imports_unresolved) break;
            auto deadline=sceKernelGetProcessTime()+15000000;
            while(!ready(r,pid)&&sceKernelGetProcessTime()<deadline&&!stopping()) usleep(100000);
            if(!ready(r,pid)||!read_state(r,s)||s.phase!=Prepared) { note("bridge readiness timeout; inspect shell log"); break; }
        }
#ifdef PROBE_PLAYER
        // Complete native initialization before installing either UI hook.
        const auto initialization_deadline=sceKernelGetProcessTime()+20000000;
        bool initialized=false;
        while(sceKernelGetProcessTime()<initialization_deadline&&!stopping()) {
            if(!read_state(r,s)||s.player_failed) break;
            if(s.player_ready) { initialized=true; break; }
            usleep(100000);
        }
          if(!initialized) { note("Player overlay initialization unavailable; overlay not armed"); break; }
#endif
        unsigned char original[16],desired[16],trampoline[128]{};
        if(mdbg_copyout(pid,(intptr_t)r.state.update,original,16)) { note("cannot read update prologue"); break; }
        auto displaced=native_patch_length(original);
        note("hook addresses update=%llx hook=%llx trampoline=%llx displaced=%zu",
             (unsigned long long)r.state.update,(unsigned long long)r.state.hook,
             (unsigned long long)r.state.trampoline,displaced);
        if(!displaced) {
            note("update prologue unsupported or already hooked: %02x%02x%02x%02x%02x%02x%02x%02x",original[0],original[1],original[2],original[3],original[4],original[5],original[6],original[7]); break;
        }
        std::memcpy(trampoline,original,displaced); jump(trampoline+displaced,r.state.update+displaced);
        std::memcpy(desired,original,16); jump(desired,r.state.hook);
        for(std::size_t i=14;i<displaced;i++) desired[i]=0x90;
        InputPatch* input=nullptr;
#ifdef PROBE_INPUT
        InputPatch key_patch;
        if(!prepare_input_patch(r,key_patch)) break;
        input=&key_patch;
#endif
#ifdef PROBE_CAPTURE
        FocusGuard focus;
        if(!focus.start(r,sdk)) { note("focus guardian unavailable; overlay not armed"); break; }
#endif
        if(!transaction(r,original,desired,trampoline,displaced,true,input)) {
            note("hook install failed; checking for a surviving own hook");
            unsigned char observed[16];
            int rc=mdbg_copyout(pid,(intptr_t)r.state.update,observed,16);
            bool unchanged=!rc&&!std::memcmp(observed,original,16);
            bool own_hook=!rc&&!std::memcmp(observed,desired,16);
            note("failed install readback rc=%d original_unchanged=%u own_hook=%u",rc,unchanged,own_hook);
            if(own_hook) (void)transaction(r,desired,original,nullptr,0,false,input);
            else if(input) (void)transaction(r,original,original,nullptr,0,false,input);
            break;
        }
        note("hook installed; %s scene experiment running",input_mode?"timed controller":"passive global");
        auto deadline=sceKernelGetProcessTime()+(std::uint64_t)(session_seconds+15)*1000000;
        std::uint32_t last=0;
#ifdef PROBE_PLAYER
        PlayerLink link;
#endif
#ifdef PROBE_NATIVE
        static SystemStats system; // two thread snapshots: too large for the stack
#endif
        while(sceKernelGetProcessTime()<deadline) {
#ifdef PROBE_CAPTURE
            const auto requested=sceKernelGetProcessTime();
#endif
            const bool observed=read_state(r,s);
#ifdef PROBE_CAPTURE
            focus.tick(s,observed,requested);
#endif
            if(!observed) { note("state read failed"); break; }
            if(s.phase!=last) { note("bridge phase=%u removed=%u frames=%llu",s.phase,s.removed,(unsigned long long)s.frames); last=s.phase; }
            {
                static std::uint32_t was_open=0;
                if(s.input_active!=was_open) {
                    was_open=s.input_active;
                    static const char* reasons[]={"none","deadline","Circle","stop","setup error","ShellUI stalled","shortcut","pad lost","system menu"};
                    if(s.input_active) note("PANEL open");
                    else note("PANEL closed reason=%u (%s) open_ms=%llu",s.release_reason,
                              s.release_reason<9?reasons[s.release_reason]:"?",
                              s.input_released>s.input_started?(unsigned long long)((s.input_released-s.input_started)/1000):0ULL);
                }
            }
            if((s.phase==Done||s.phase==Failed)&&s.removed) break;
            if(stopping()) (void)request_worker_stop();
#ifdef PROBE_PLAYER
            link.tick(s);
#endif
#ifdef PROBE_NATIVE
            system.tick(s,sdk,link.status());
#endif
#ifdef PROBE_CAPTURE
            usleep(s.input_active?20000:100000);
#else
            usleep(100000);
#endif
        }
        bool clean=(s.phase==Done||s.phase==Failed)&&s.removed;
        if(!clean) {
            note("cleanup timeout; stopping bridge before hook restoration");
            (void)request_worker_stop();
            auto grace=sceKernelGetProcessTime()+5000000;
            while(sceKernelGetProcessTime()<grace&&read_state(r,s)) {
#ifdef PROBE_CAPTURE
                focus.tick(s,true,sceKernelGetProcessTime(),false);
#endif
                clean=(s.phase==Done||s.phase==Failed)&&s.removed;
                if(clean) break; usleep(100000);
            }
        }
#ifdef PROBE_CAPTURE
        bool focus_clean=focus.finish();
#endif
        bool hook_restored=transaction(r,desired,original,nullptr,0,false,input);
        note("SUMMARY scene_removed=%u hook_restored=%u phase=%u frames=%llu resident_worker=1",
             clean,hook_restored,s.phase,(unsigned long long)s.frames);
#ifdef PROBE_PLAYER
        note("BOREALIS SUMMARY ready=%u failed=%u rendered=%llu uploaded=%llu render_max_us=%llu upload_max_us=%llu",
             s.player_ready,s.player_failed,(unsigned long long)s.surface_frames,
             (unsigned long long)s.surface_uploads,(unsigned long long)s.render_max_us,
             (unsigned long long)s.upload_max_us);
#endif
        failed=!(clean&&hook_restored&&s.phase==Done);
#ifdef PROBE_PLAYER
        failed=failed||s.player_failed;
#endif
        if(clean&&hook_restored) clear_owned_stop_marker();
#ifdef PROBE_CAPTURE
        failed=failed||!focus_clean;
#endif
#ifdef PROBE_INPUT
        bool refreshed=read_state(r,s);
        auto elapsed=s.input_started&&s.input_released>=s.input_started?(s.input_released-s.input_started)/1000:0;
        note("INPUT SUMMARY active=%u reason=%u elapsed_ms=%llu seen=%llu consumed=%llu key_hook_restored=%u snapshot=%u",
            s.input_active,s.release_reason,(unsigned long long)elapsed,
            (unsigned long long)s.events_seen,(unsigned long long)s.events_consumed,key_patch.restored,refreshed);
        failed=failed||!refreshed||s.input_active||!key_patch.restored||
#ifdef PROBE_TOGGLE
            s.release_reason>PadLost;
        note("TOGGLE SUMMARY count=%llu samples=%llu port=%u connected=%u buttons=0x%08x rc=0x%08x game_focus_lease=%u",
            (unsigned long long)s.toggles,(unsigned long long)s.pad_samples,s.pad_port,s.pad_connected,s.pad_buttons,(unsigned)s.pad_rc,
#ifdef PROBE_CAPTURE
            1U
#else
            0U
#endif
        );
#else
            (s.release_reason!=Deadline&&s.release_reason!=BackButton&&s.release_reason!=StopRequest);
#endif
#endif
    } while(false);
    note("END result=%s shell_log=%s",failed?"error":"complete",shell_log);
    std::fclose(output); close(lock); return failed;
}
