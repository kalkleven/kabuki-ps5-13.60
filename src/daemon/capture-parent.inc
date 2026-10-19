/* Controller-focus lease; no code changes to games/Core. GPL-3.0-or-later. */
extern "C" {
int sceLncUtilInitialize();
int sceLncUtilSetControllerFocus(int app);
int sceKernelGetAppInfo(pid_t,void*);
}

struct FocusSnapshot { std::int32_t application,controller; };
struct FocusLease {
    Ready ready{};
    pid_t core=-1;
    std::uint64_t base=0,manager=0;
    std::int32_t shell_app=-1;
    std::uint32_t errors=0;
    unsigned acquired=0,restored=0,external=0;
    bool started=false,held=false,observed=false;
    FocusSnapshot saved{},current{};
    std::uint64_t attempted=0,deadline=0,next_log=0,next_snapshot=0;
    std::uint64_t remote_heartbeat=0,heartbeat_changed=0,clock_logged=0;
    std::uint64_t opening=0; // the opening the retakes count for
    unsigned retakes=0;
};
static std::uint64_t focus_fingerprint(const unsigned char* bytes,std::size_t count) {
    std::uint64_t h=0xcbf29ce484222325ULL;
    for(std::size_t i=0;i<count;++i) h=(h^bytes[i])*0x100000001b3ULL;
    return h;
}
static bool focus_code(FocusLease& lease,std::uint64_t offset,std::size_t count,std::uint64_t hash) {
    unsigned char bytes[64];
    if(count>sizeof bytes) return false;
    const int rc=mdbg_copyout(lease.core,(intptr_t)(lease.base+offset),bytes,count);
    const auto observed=rc?0:focus_fingerprint(bytes,count);
    note("FOCUS fingerprint offset=%llx read_rc=%d observed=%llx match=%u",
         (unsigned long long)offset,rc,(unsigned long long)observed,!rc&&observed==hash);
    if(!rc) {
        char hex[sizeof(bytes)*2+1]{};
        for(std::size_t i=0;i<count;++i)
            std::snprintf(hex+i*2,sizeof(hex)-i*2,"%02x",bytes[i]);
        note("FOCUS code bytes offset=%llx count=%zu hex=%s",
             (unsigned long long)offset,count,hex);
    }
    return !rc&&observed==hash;
}
static bool focus_snapshot(const FocusLease& lease,FocusSnapshot& result) {
    // The guarded firmware getter reads LncManager.controller (+12). The
    // ordinary AppFocusedAppStatus getter reads application (+8), a different
    // owner. This is observation only: no manager/registry/kernel-data writes.
    std::uint64_t manager=0,after=0; FocusSnapshot repeat{};
    if(mdbg_copyout(lease.core,(intptr_t)(lease.base+0x20d30f0),&manager,sizeof manager)||
       manager!=lease.manager||!user_address(manager)) return false;
    if(mdbg_copyout(lease.core,(intptr_t)(manager+8),&result,sizeof result)||
       mdbg_copyout(lease.core,(intptr_t)(manager+8),&repeat,sizeof repeat)||
       mdbg_copyout(lease.core,(intptr_t)(lease.base+0x20d30f0),&after,sizeof after)) return false;
    return manager==after&&!std::memcmp(&result,&repeat,sizeof result)&&
        result.application>=-1&&result.controller>=-1;
}
static bool prepare_focus(FocusLease& lease,const Ready& ready,std::uint32_t sdk) {
    lease.ready=ready; lease.core=process_pid("SceShellCore");
    note("FOCUS Core process pid=%d",lease.core);
    if(lease.core<=1) { note("FOCUS unavailable: Core process not found"); return false; }
    std::uint32_t handle=0;
    const int lookup=kernel_dynlib_handle(lease.core,"SceShellCore.elf",&handle);
    note("FOCUS Core module lookup rc=%d errno=%d handle=0x%08x",lookup,errno,handle);
    // The main executable can have handle zero. SDK success is its return
    // code, not a nonzero handle. If its loaded basename differs, try the
    // main handle; all three firmware fingerprints still have to match.
    if(lookup) {
        handle=0;
        note("FOCUS trying main module handle=0 with firmware fingerprint guards");
    }
    lease.base=(std::uint64_t)kernel_dynlib_mapbase_addr(lease.core,handle);
    note("FOCUS Core module base=%llx",(unsigned long long)lease.base);
    if(!user_address(lease.base)) {
        note("FOCUS unavailable: Core base is not a user address; no ownership change"); return false;
    }
    // 13.60 moved these code regions relative to the 13.20 function boundaries.
    // Their observed bytes still form a firmware fingerprint; the manager slot
    // and stable two-field snapshot below are separately validated before use.
    const bool fw1360=sdk==0x13600007U;
    note("FOCUS firmware SDK=%08x fingerprint_set=%s",sdk,fw1360?"13.60":"13.20");
    const bool code_a=focus_code(lease,0x6a4880,40,fw1360?0xd15dc735f907ea34ULL:0x505f765f05540c68ULL);
    const bool code_b=focus_code(lease,0x6eeb30,29,fw1360?0x1a394c083b0a9224ULL:0x1d9c912126922f03ULL);
    const bool code_c=focus_code(lease,0x69e334,16,fw1360?0x4bb0a4a243e6241eULL:0xb7ec89ce3a985c5eULL);
    if(!code_a||!code_b||!code_c) {
        // Read-only diagnostic for firmware porting. This never reaches the
        // focus API; it only checks whether the existing manager offset and
        // two-field snapshot are still plausible on the unrecognized build.
        const auto slot=lease.base+0x20d30f0;
        std::uint64_t candidate=0;
        const int manager_rc=mdbg_copyout(lease.core,(intptr_t)slot,&candidate,sizeof candidate);
        const bool candidate_valid=!manager_rc&&user_address(candidate);
        note("FOCUS candidate manager rc=%d slot=%llx pointer=%llx valid=%u",
             manager_rc,(unsigned long long)slot,(unsigned long long)candidate,candidate_valid);
        if(candidate_valid) {
            lease.manager=candidate;
            FocusSnapshot snapshot{};
            const bool stable=focus_snapshot(lease,snapshot);
            note("FOCUS candidate snapshot stable=%u application=%d controller=%d",
                 stable,snapshot.application,snapshot.controller);
        }
        note("FOCUS unavailable: firmware fingerprints did not match; no ownership change"); return false;
    }
    if(mdbg_copyout(lease.core,(intptr_t)(lease.base+0x20d30f0),&lease.manager,sizeof lease.manager)||
       !user_address(lease.manager)) {
        note("FOCUS unavailable: controller manager pointer invalid"); return false;
    }
    alignas(16) unsigned char info[256]{};
    int rc=sceKernelGetAppInfo(ready.state.pid,info);
    std::memcpy(&lease.shell_app,info,sizeof lease.shell_app);
    FocusSnapshot snapshot{};
    if(rc||lease.shell_app<=0||!focus_snapshot(lease,snapshot)) {
        note("FOCUS unavailable: identity rc=0x%08x shell_app=0x%08x",(unsigned)rc,(unsigned)lease.shell_app);
        return false;
    }
    rc=sceLncUtilInitialize();
    note("FOCUS prepared core=%d base=%llx shell_app=0x%08x app=0x%08x controller=0x%08x client_rc=0x%08x",
        lease.core,(unsigned long long)lease.base,(unsigned)lease.shell_app,
        (unsigned)snapshot.application,(unsigned)snapshot.controller,(unsigned)rc);
    return rc==0;
}
static bool restore_focus(FocusLease& lease,const FocusSnapshot& saved) {
    FocusSnapshot now{};
    if(!focus_snapshot(lease,now)) { ++lease.errors; return false; }
    if(now.controller==saved.controller) {
        ++lease.restored; note("FOCUS restored already controller=0x%08x",(unsigned)now.controller); return true;
    }
    if(now.controller!=lease.shell_app||now.application!=saved.application) {
        ++lease.external;
        note("FOCUS relinquished to system app=0x%08x controller=0x%08x; saved app=0x%08x owner=0x%08x",
             (unsigned)now.application,(unsigned)now.controller,(unsigned)saved.application,(unsigned)saved.controller);
        return true; // Do not overwrite a PS-button dialog or a newly launched app.
    }
    const int rc=sceLncUtilSetControllerFocus(saved.controller);
    const bool read=focus_snapshot(lease,now);
    const bool restored=read&&now.controller==saved.controller;
    note("FOCUS restore owner=0x%08x rc=0x%08x readback=%u observed=0x%08x",
         (unsigned)saved.controller,(unsigned)rc,restored,read?(unsigned)now.controller:0xffffffffU);
    if(restored) ++lease.restored; else ++lease.errors;
    return restored;
}
struct FocusGuard {
    FocusLease lease;
    bool start(const Ready& ready,std::uint32_t sdk) {
        if(!prepare_focus(lease,ready,sdk)) return false;
        lease.started=true;
        note("FOCUS guardian native_parent=1 SDK_access=serialized clocks=relative");
        return true;
    }
    void tick(const State& s,bool read,std::uint64_t request_time,bool allow_acquire=true) {
        auto& l=lease;
        const auto now=sceKernelGetProcessTime(); // AFTER reading remote State.
        if(read&&s.guardian_heartbeat&&s.guardian_heartbeat!=l.remote_heartbeat) {
            l.remote_heartbeat=s.guardian_heartbeat; l.heartbeat_changed=now;
        }
        // ShellUI timestamps must only be compared with other ShellUI stamps.
        // Observe heartbeat progress with our own clock, then translate the
        // remaining remote lease into a conservative local deadline once.
        const auto remote_now=s.guardian_heartbeat>s.ui_heartbeat?s.guardian_heartbeat:s.ui_heartbeat;
        const auto ui_age=remote_now>=s.ui_heartbeat?remote_now-s.ui_heartbeat:500001;
        // A slow ShellUI frame (a card loading its pictures) must not end the lease: allow 1.5 s.
        const bool fresh=l.heartbeat_changed&&now>=l.heartbeat_changed&&now-l.heartbeat_changed<=1500000;
        const bool active=read&&s.phase==Running&&s.input_active&&s.input_started&&
            s.input_deadline>s.input_started&&s.input_deadline-s.input_started<=input_limit_us&&
            remote_now>=s.input_started&&remote_now<s.input_deadline&&
            s.guardian_heartbeat&&s.ui_heartbeat&&ui_age<=1500000&&fresh;
        if(read&&s.input_started&&l.clock_logged!=s.input_started) {
            l.clock_logged=s.input_started;
            note("FOCUS clocks local=%llu remote_heartbeat=%llu remote_start=%llu remote_end=%llu",
                (unsigned long long)now,(unsigned long long)s.guardian_heartbeat,
                (unsigned long long)s.input_started,(unsigned long long)s.input_deadline);
        }
        if((active&&!l.held&&s.input_started!=l.attempted)||
           (l.held&&now>=l.next_snapshot)||now>=l.next_log) {
            l.observed=focus_snapshot(l,l.current); l.next_snapshot=now+100000;
        }
        if(l.held&&(!allow_acquire||stopping()||!active||now>=l.deadline||!l.observed||
                   l.current.application!=l.saved.application||l.current.controller!=l.shell_app)) {
            // Only a stall (the panel is still open, nothing else took the controller) may take
            // the lease again in the same opening, once ShellUI ticks again.
            const bool stall=read&&s.input_active&&s.phase==Running&&l.observed&&
                l.current.application==l.saved.application&&
                (l.current.controller==l.shell_app||l.current.controller==l.saved.controller)&&
                l.retakes<3;
            if(restore_focus(l,l.saved)) {
                l.held=false;
                if(stall) {
                    ++l.retakes; l.attempted=0;
                    note("FOCUS lease lost while the panel is open (retake %u); taking it again",l.retakes);
                }
            }
            else return; // Retry next native parent iteration.
        }
        if(active&&allow_acquire&&!stopping()&&!l.held&&s.input_started!=l.attempted) {
            if(s.input_started!=l.opening) { l.opening=s.input_started; l.retakes=0; }
            l.attempted=s.input_started;
            const auto remaining=s.input_deadline-remote_now;
            if(!l.observed||l.current.controller<0||remaining<=100000) {
                ++l.errors; note("FOCUS acquisition skipped: invalid owner or expired lease");
            } else if(l.current.controller==l.shell_app) {
                note("FOCUS home already owns input; ShellUI event gate handles this opening");
            } else {
                l.saved=l.current;
                l.deadline=request_time+remaining-100000; // Read/poll margin; never renew.
                if(sceKernelGetProcessTime()>=l.deadline) return;
                l.held=true; // Failed IPC may still have changed ownership.
                const int rc=sceLncUtilSetControllerFocus(l.shell_app);
                FocusSnapshot after{}; const bool check=focus_snapshot(l,after);
                const bool own=check&&after.controller==l.shell_app;
                l.current=after; l.observed=check; l.next_snapshot=0;
                note("FOCUS acquire saved=0x%08x target=0x%08x rc=0x%08x readback=%u observed=0x%08x remaining_ms=%llu",
                    (unsigned)l.saved.controller,(unsigned)l.shell_app,(unsigned)rc,own,
                    check?(unsigned)after.controller:0xffffffffU,(unsigned long long)(remaining/1000));
                if(rc==0&&own) ++l.acquired;
                else { ++l.errors; if(restore_focus(l,l.saved)) l.held=false; }
                State after_state{};
                if(l.held&&(sceKernelGetProcessTime()>=l.deadline||
                    !read_state(l.ready,after_state)||!after_state.input_active||
                    after_state.input_started!=l.attempted)) {
                    if(restore_focus(l,l.saved)) l.held=false;
                }
            }
        }
        if(now>=l.next_log) {
            l.next_log=now+1000000;
            note("FOCUS status panel=%u lease=%u app=0x%08x controller=0x%08x snapshot=%u heartbeat_fresh=%u",
                active,l.held,l.observed?(unsigned)l.current.application:0xffffffffU,
                l.observed?(unsigned)l.current.controller:0xffffffffU,l.observed,fresh);
        }
    }
    bool finish() {
        if(lease.started) {
            while(lease.held) {
                if(restore_focus(lease,lease.saved)) lease.held=false;
                else usleep(100000);
            }
            lease.started=false;
            note("FOCUS SUMMARY acquired=%u restored=%u system_changes=%u errors=%u",
                lease.acquired,lease.restored,lease.external,lease.errors);
        }
        return !lease.errors;
    }
    ~FocusGuard() { (void)finish(); }
};
