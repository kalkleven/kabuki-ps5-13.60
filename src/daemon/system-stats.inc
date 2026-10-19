/* The System tab's readings, gathered by the parent (an ordinary payload process) once a second
   while the panel is open and written to /data/custom-overlays/system.rec for the panel.
   CPU follows drakmor/ps5-hwinfo (each core's idle thread, found by name); the frame rate is the
   display controller's flip counter as Common FPS for PS5 / SimpleFPS read it.
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <sys/ioctl.h>
extern "C" {
int sceKernelGetCpuTemperature(int* temp);
int sceKernelGetSocSensorTemperature(int sensor,int* temp);
int get_page_table_stats(int vm,int type,int* total,int* free);
int sceKernelGetCpuUsage(void* threads,std::int32_t* count);
int sceKernelGetThreadName(std::uint32_t tid,char* name);
}
class SystemStats {
public:
    void tick(const State& s,std::uint32_t sdk,const std::string& librespot) {
        const auto now=sceKernelGetProcessTime();
        if(!s.input_active||now<next) return;
        next=now+1000000;
        sample(now);
        char firmware[16]; std::snprintf(firmware,sizeof firmware,"%x.%02x",sdk>>24,(sdk>>16)&0xffU);
        char librespot_text[61]{};
        std::size_t note_size=0;
        for(const unsigned char c:librespot) {
            if(note_size==sizeof librespot_text-1) break;
            librespot_text[note_size++]=(c<' '||c=='\\'||c==0x7f)?' ':static_cast<char>(c);
        }
        if(!note_size) std::snprintf(librespot_text,sizeof librespot_text,"Not running");
        timespec up{}; clock_gettime(CLOCK_MONOTONIC,&up);
        char text[768];
        std::snprintf(text,sizeof text,
            "kabuki\t1\nstat\tcpu\t%.1f\nstat\tcpu_temp\t%d\nstat\tsoc_temp\t%d\nstat\tmem_used_gb\t%.2f\nstat\tmem_total_gb\t%.2f\n"
            "stat\tfps\t%.1f\nstat\tfps_min\t%.1f\nstat\tuptime_s\t%lld\ninfo\tfirmware\t%s\ninfo\taddress\t%s\ninfo\tlibrespot\t%s\n",
            cpu,cpu_temp,soc_temp,mem_used/1024.0,mem_total/1024.0,fps_average(),fps_lowest(),(long long)up.tv_sec,firmware,
            address().c_str(),librespot_text);
        kabuki::write_file_atomic("/data/custom-overlays/system.rec",text);
        if(samples<=3) note("SYSTEM stats %.200s",text);
    }
private:
    // sceKernelGetCpuUsage's entry (ps5-hwinfo's proc_stats_t, 40 bytes).
    struct Thread { std::uint32_t pid,tid; std::int64_t user_s,user_us,system_s,system_us; };
    static_assert(sizeof(Thread)==40);
    static constexpr int kThreads=3072;
    // Twice the entries asked for: room in case the kernel writes more than it is told. On the heap:
    // as static data they made the payload too large to load (over 2 MiB).
    Thread* threads[2]={nullptr,nullptr};
    int counts[2]={0,0},bank=0;
    std::uint64_t taken[2]={0,0};
    std::uint32_t idle_tid[16]{}; int idle_found=0;
    std::uint64_t next=0;
    float cpu=-1; int cpu_temp=-1,soc_temp=-1,mem_used=-1,mem_total=-1;
    int dce=-1; std::uint64_t flips_at=0;
    std::uint64_t counters[12]{};int streak[12]{};float previous_rate[12]{};
    int selected_counter=-1;unsigned fps_failures=0;
    float fps_seen[5]={-1,-1,-1,-1,-1}; int fps_at=0,samples=0;

    void step(const char* name) { if(samples==0) note("SYSTEM step %s",name); }
    static double seconds(const Thread& t) { return t.user_s+t.user_us*1e-6+t.system_s+t.system_us*1e-6; }
    void sample(std::uint64_t now) {
        int t=0;
        step("temperature");
        cpu_temp=sceKernelGetCpuTemperature(&t)==0?t:-1;
        soc_temp=sceKernelGetSocSensorTemperature(0,&t)==0?t:-1;
        step("memory");
        memory();
        step("cpu");
        cpu_load(now);
        step("frame rate");
        fps_seen[fps_at++%5]=frame_rate(now);
        step("done");
        ++samples;
    }
    // System RAM: the kernel's page counters (free, inactive and cache pages are available), as
    // ps5-hwinfo's fallback; the page-table figure (constant 16384/10226 MB here) only without them.
    void memory() {
        unsigned page=0,pages=0,free=0,inactive=0,cache=0; std::size_t n=sizeof page;
        bool counted=!sysctlbyname("hw.pagesize",&page,&n,nullptr,0)&&page;
        if(counted) { n=sizeof pages; counted=!sysctlbyname("vm.stats.vm.v_page_count",&pages,&n,nullptr,0)&&pages; }
        if(counted) { n=sizeof free; counted=!sysctlbyname("vm.stats.vm.v_free_count",&free,&n,nullptr,0); }
        if(counted) {
            n=sizeof inactive; (void)sysctlbyname("vm.stats.vm.v_inactive_count",&inactive,&n,nullptr,0);
            n=sizeof cache; (void)sysctlbyname("vm.stats.vm.v_cache_count",&cache,&n,nullptr,0);
            const double mb=page/1048576.0;
            mem_total=int(pages*mb); mem_used=mem_total-int((double(free)+inactive+cache)*mb);
        }
        int total=0,free_mb=0;
        const bool table=get_page_table_stats(1,1,&total,&free_mb)==0&&total>0;
        if(!counted&&table) { mem_total=total; mem_used=total-free_mb; }
        if(samples<3) note("SYSTEM raw memory pages=%u page=%u free=%u inactive=%u cache=%u table_total=%d table_free=%d using=%s",
                           pages,page,free,inactive,cache,total,free_mb,counted?"counters":"table");
    }
    // CPU: 1 - idle share of each core's idle thread (SceIdleCpu0..15, SceIdleCpuRv for core 13),
    // averaged. Summing every thread counted the idle threads as busy: always 100%.
    void cpu_load(std::uint64_t now) {
        for(auto*& t:threads) if(!t) t=static_cast<Thread*>(std::calloc(kThreads*2,sizeof(Thread)));
        if(!threads[0]||!threads[1]) return;
        auto* list=threads[bank];
        std::int32_t count=kThreads;
        if(sceKernelGetCpuUsage(list,&count)!=0||count<=0||count>kThreads) {
            if(samples<3) note("SYSTEM raw cpu usage failed count=%d",count);
            return;
        }
        counts[bank]=count; taken[bank]=now;
        if(idle_found<8) {
            idle_found=0;
            for(int i=0;i<count;++i) {
                char name[64]{};
                if(sceKernelGetThreadName(list[i].tid,name)!=0) continue;
                int lane=-1;
                if(!std::strcmp(name,"SceIdleCpuRv")) lane=13;
                else if(std::sscanf(name,"SceIdleCpu%d",&lane)!=1) lane=-1;
                if(lane>=0&&lane<16) { idle_tid[lane]=list[i].tid; ++idle_found; }
            }
            if(samples<3) note("SYSTEM raw cpu threads=%d idle_threads=%d",count,idle_found);
        }
        const int other=bank^1;
        if(taken[other]&&now>taken[other]&&idle_found) {
            const double wall=(now-taken[other])/1e6;
            double busy=0; int lanes=0;
            for(int lane=0;lane<16;++lane) {
                if(!idle_tid[lane]) continue;
                const Thread *a=nullptr,*b=nullptr;
                for(int i=0;i<count;++i) if(list[i].tid==idle_tid[lane]) { a=&list[i]; break; }
                for(int i=0;i<counts[other];++i) if(threads[other][i].tid==idle_tid[lane]) { b=&threads[other][i]; break; }
                if(!a||!b) continue;
                const double idle=std::clamp((seconds(*a)-seconds(*b))/wall,0.0,1.0);
                busy+=1-idle; ++lanes;
            }
            if(lanes) cpu=float(busy/lanes*100);
        }
        bank=other;
    }
    // Pictures shown a second, from the display controller's flip counter (Common FPS for PS5's
    // fallback, SimpleFPS on 13.60). The home screen itself draws at 60; a game shows its own rate.
    float frame_rate(std::uint64_t now) {
        const int sample=open("/system_tmp/fps_sample",O_RDONLY);
        if(sample>=0) {
            unsigned char b[64]{}; const auto n=read(sample,b,sizeof b); close(sample);
            std::uint32_t magic=0; float value=0; std::memcpy(&magic,b,4); std::memcpy(&value,b+16,4);
            if(n>=20&&magic==0x4F465053u&&b[12]&&value>0&&value<=245) return value;
        }
        // DCE access can require the debugger credential already used by Kabuki's
        // loader. Keep it only around open/ioctl, restore and verify on every path.
        const auto auth=kernel_get_ucred_authid(getpid());
        bool privileged=false;
        const auto elevate=[&] {if(privileged)return true;if(!auth)return false;
            privileged=kernel_set_ucred_authid(getpid(),0x4800000000010003ULL)==0;return privileged;};
        if(dce<0) {
            dce=open("/dev/dce",O_RDWR);
            if(dce<0&&elevate()){dce=open("/dev/dce",O_RDWR);if(dce<0)dce=open("/dev/dce",O_RDONLY);}
        }
        struct { std::uint64_t selector,mask,output,reserved[3]; } arg{};
        alignas(16) std::uint64_t current[12]{};
        arg.selector=0x10000000AULL;arg.mask=0x8000000000ULL;arg.output=(std::uint64_t)current;
        const auto query=[&] {return dce>=0&&(ioctl(dce,0x80308217UL,&arg)==0||ioctl(dce,0xffffffff80308217UL,&arg)==0);};
        bool ok=query();if(!ok&&elevate())ok=query();const int error=errno;
        if(privileged&&(kernel_set_ucred_authid(getpid(),auth)||kernel_get_ucred_authid(getpid())!=auth)) {
            note("SYSTEM FPS auth restoration failed; sampling disabled");
            interrupted=SIGTERM;ok=false;
        }
        if(!ok) {
            if(dce>=0)close(dce);dce=-1;flips_at=0;selected_counter=-1;
            if(++fps_failures==1||fps_failures%15==0)note("SYSTEM FPS DCE unavailable errno=%d retry=%u",error,fps_failures);
            return -1;
        }
        if(!flips_at||now<=flips_at||now-flips_at>2500000) {
            std::memcpy(counters,current,sizeof counters);flips_at=now;
            std::fill(std::begin(streak),std::end(streak),0);
            std::fill(std::begin(fps_seen),std::end(fps_seen),-1);return -1;
        }
        float rates[12]{};
        for(int i=0;i<12;++i) {
            const double rate=current[i]>=counters[i]?double(current[i]-counters[i])*1e6/double(now-flips_at):0;
            rates[i]=rate>=1&&rate<=245?float(rate):-1;
            streak[i]=rates[i]>=0&&std::fabs(rates[i]-previous_rate[i])<=12?streak[i]+1:0;previous_rate[i]=rates[i];
        }
        if(rates[1]>=0)selected_counter=1;
        else if(selected_counter<0||rates[selected_counter]<0) {
            selected_counter=-1;
            for(int i=0;i<12;++i)if(streak[i]>=2){selected_counter=i;break;}
        }
        std::memcpy(counters,current,sizeof counters);flips_at=now;
        if(selected_counter<0)return -1;
        if(fps_failures||samples<3)note("SYSTEM FPS DCE counter=%d fps=%.1f restored=%u",selected_counter,rates[selected_counter],!privileged||kernel_get_ucred_authid(getpid())==auth);
        fps_failures=0;return rates[selected_counter];
    }

    float fps_average() const {
        float sum=0; int n=0;
        for(float f:fps_seen) if(f>=0) { sum+=f; ++n; }
        return n?sum/n:-1;
    }
    float fps_lowest() const {
        float low=-1;
        for(float f:fps_seen) if(f>=0&&(low<0||f<low)) low=f;
        return low;
    }
    // This console's address on the network: the source a UDP socket would use (nothing is sent).
    static std::string address() {
        const int fd=socket(AF_INET,SOCK_DGRAM,0); if(fd<0) return "";
        sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(53); a.sin_addr.s_addr=htonl(0x08080808);
        std::string text;
        sockaddr_in me{}; socklen_t size=sizeof me;
        if(!connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof a)&&!getsockname(fd,reinterpret_cast<sockaddr*>(&me),&size)) {
            char ip[32]; const auto v=ntohl(me.sin_addr.s_addr);
            std::snprintf(ip,sizeof ip,"%u.%u.%u.%u",v>>24,(v>>16)&255,(v>>8)&255,v&255); text=ip;
        }
        close(fd); return text;
    }
};
