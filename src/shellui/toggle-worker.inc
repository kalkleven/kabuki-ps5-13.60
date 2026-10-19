/* Raw observation and bounded L3+R3 toggle. SPDX-License-Identifier: GPL-3.0-or-later */
extern "C" int scePadGetDataInternal(int port,void* data);

// A close asked for with Circle or L3+R3 hides the panel at once, but input is only given
// back once those buttons are up: given back while still held, the game (or home) took the
// held Circle as a press of its own.
static std::uint32_t close_pending=NoRelease; // atomic: also set from the key hook
static std::uint64_t close_asked_at=0;
static void request_close_on_release(std::uint32_t reason) {
    std::uint32_t none=NoRelease;
    if(__atomic_compare_exchange_n(&close_pending,&none,reason,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
        __atomic_store_n(&close_asked_at,sceKernelGetProcessTime(),__ATOMIC_RELEASE);
}
static bool closing() { return __atomic_load_n(&close_pending,__ATOMIC_ACQUIRE)!=NoRelease; }
// The left stick, as D-pad directions (up 0x10, right 0x20, down 0x40, left 0x80), with a
// dead zone and some hysteresis so a stick resting near the edge does not flicker.
static std::uint32_t stick_directions(const unsigned char* raw) {
    static std::uint32_t held=0;
    const int x=raw[4]-128,y=raw[5]-128;
    const auto axis=[](int v,std::uint32_t low_bit,std::uint32_t high_bit,std::uint32_t current) {
        const int on=72,off=48;
        std::uint32_t bits=0;
        if(v<=-on||(v<=-off&&(current&low_bit))) bits|=low_bit;
        if(v>=on||(v>=off&&(current&high_bit))) bits|=high_bit;
        return bits;
    };
    held=axis(x,0x80,0x20,held)|axis(y,0x10,0x40,held);
    return held;
}
static void toggle_poll() {
    static std::uint64_t stamp[4]{},fresh[4]{},hold=0,last_poll=0,next_repeat=0;
    static std::uint64_t logged=0,pad_gone_at=0;
    static std::uint32_t previous=0;
    static int selected_port=-1;
    static bool latched=true;
    const auto now=sceKernelGetProcessTime();
#ifdef PROBE_PLAYER
    if(!__atomic_load_n(&state.input_active,__ATOMIC_ACQUIRE)) overlay_console::input(0,false);
#endif
    if(__atomic_load_n(&state.phase,__ATOMIC_ACQUIRE)!=Running) {
        hold=last_poll=next_repeat=0; previous=0; selected_port=-1; latched=true;
        for(unsigned i=0;i<4;++i) stamp[i]=fresh[i]=0;
        return;
    }
    if(!last_poll||now<last_poll||now-last_poll>100000) hold=0;
    last_poll=now;
    const auto ui=__atomic_load_n(&state.ui_heartbeat,__ATOMIC_ACQUIRE);
    if(!ui||now<ui||now-ui>1500000) { // 1.5 s: a slow frame between cards is not a lost ShellUI
        release_input(GuardianLost); hold=0; latched=true; return;
    }
    if(__atomic_load_n(&state.stop,__ATOMIC_ACQUIRE)||access(worker_stop_path,F_OK)==0) {
        release_input(StopRequest); return;
    }
    if(__atomic_load_n(&state.input_active,__ATOMIC_ACQUIRE)&&
       now>=__atomic_load_n(&state.input_deadline,__ATOMIC_ACQUIRE)) release_input(Deadline);
    // Firmware 13.20: exported function takes a physical port index 0..3 and
    // writes 0x78 bytes. It returns buttons at 0, connected BYTE at 0x4c and
    // sample timestamp at 0x50. No PadOpen, focus setter or game handle is used.
    int port=-1,rc=0; std::uint32_t buttons=0; unsigned char stick[6]{128,128,128,128,128,128};
    for(int i=0;i<4;++i) {
        alignas(32) unsigned char raw[256]{};
        rc=scePadGetDataInternal(i,raw);
        __atomic_fetch_add(&state.pad_samples,1,__ATOMIC_RELAXED);
        std::uint64_t time=0; std::uint32_t bits=0;
        std::memcpy(&time,raw+0x50,sizeof time); std::memcpy(&bits,raw,sizeof bits);
        if(rc<0||!raw[0x4c]||!time||(bits&0x80000000U)) continue;
        if(time!=stamp[i]) { stamp[i]=time; fresh[i]=now; }
        if(!fresh[i]||now-fresh[i]>250000) continue;
        port=i; buttons=bits; std::memcpy(stick,raw,sizeof stick);
        break; // Never combine clicks from two controllers.
    }
    __atomic_store_n(&state.pad_rc,rc,__ATOMIC_RELAXED);
    __atomic_store_n(&state.pad_connected,port>=0?1U:0U,__ATOMIC_RELAXED);
    __atomic_store_n(&state.pad_buttons,buttons,__ATOMIC_RELAXED);
    __atomic_store_n(&state.pad_port,port>=0?(std::uint32_t)port:0xffffffffU,__ATOMIC_RELAXED);
    if(port!=selected_port) {
        log("PAD port change old=%d new=%d rc=0x%08x",selected_port,port,(unsigned)rc);
        selected_port=port; hold=next_repeat=0; previous=0; latched=true;
        pad_gone_at=port<0?now:0;
    }
    if(port<0&&pad_gone_at&&now-pad_gone_at>2000000&&__atomic_load_n(&state.input_active,__ATOMIC_ACQUIRE)) {
        pad_gone_at=0; release_input(PadLost);
    }
    if(now-logged>=30000000) {
        logged=now;
        log("PAD port=%d rc=0x%08x buttons=0x%08x samples=%llu active=%u selection=%u confirm=%u",port,(unsigned)rc,buttons,
            (unsigned long long)__atomic_load_n(&state.pad_samples,__ATOMIC_RELAXED),
            __atomic_load_n(&state.input_active,__ATOMIC_ACQUIRE),
            __atomic_load_n(&state.selection,__ATOMIC_RELAXED),__atomic_load_n(&state.enter_count,__ATOMIC_RELAXED));
    }
    if(port<0) { hold=0; previous=0; latched=true; return; }
#ifdef PROBE_PLAYER
    if(!__atomic_load_n(&state.player_ready,__ATOMIC_ACQUIRE)||
       __atomic_load_n(&state.player_failed,__ATOMIC_ACQUIRE)) {
        overlay_console::input(0,false); hold=0; previous=0; latched=true; return;
    }
#endif
#ifdef PROBE_NATIVE
    const std::uint32_t mask=overlay_console::chord_mask.load();
    const std::uint64_t hold_needed=overlay_console::chord_hold.load();
    if(overlay_console::recording.load()) { hold=0; latched=true; } // a shortcut being recorded is not pressed
#else
    const std::uint32_t mask=6U; // L3=2, R3=4.
    const std::uint64_t hold_needed=chord_hold_us;
#endif
    const auto chord=buttons&mask;
    if(!chord) { latched=false; hold=0; }
    else if(chord!=mask) hold=0;
    else if(!latched) {
        if(!hold) hold=now;
        else if(now-hold>=hold_needed) {
            const auto held_ms=(now-hold)/1000;
            latched=true; hold=0;
            if(__atomic_load_n(&state.input_active,__ATOMIC_ACQUIRE)) request_close_on_release(ToggleClose);
            else {
                __atomic_store_n(&close_pending,(std::uint32_t)NoRelease,__ATOMIC_RELEASE);
                __atomic_store_n(&state.release_reason,NoRelease,__ATOMIC_RELEASE);
                __atomic_store_n(&state.input_released,0,__ATOMIC_RELEASE);
                __atomic_store_n(&state.input_started,now,__ATOMIC_RELEASE);
                __atomic_store_n(&state.input_deadline,now+input_limit_us,__ATOMIC_RELEASE);
                __atomic_store_n(&state.input_active,1,__ATOMIC_RELEASE);
            }
            const auto count=__atomic_add_fetch(&state.toggles,1,__ATOMIC_RELAXED);
            log("TOGGLE chord hold_ms=%llu action=%s count=%llu",(unsigned long long)held_ms,
                closing()?"close":"open",(unsigned long long)count);
        }
    }
    if(__atomic_load_n(&state.input_active,__ATOMIC_ACQUIRE)) {
        // Circle: from the card back to the rail; on the rail it closes. Once per press.
        if((buttons&0x2000U)&&!(previous&0x2000U)
#ifdef PROBE_NATIVE
           &&!overlay_console::recording.load() // recording a shortcut: Circle is one of its buttons
#endif
          ) {
#ifdef PROBE_PLAYER
#ifdef PROBE_NATIVE
            if(overlay_console::nested()) overlay_console::back_to_rail(); else
#else
            if(overlay_console::card_focused()) overlay_console::back_to_rail(); else
#endif
#endif
            request_close_on_release(BackButton);
        }
        // Give input back once Circle, L3 and R3 are all up (at most 3 s after the close).
        if(closing()&&(!(buttons&(0x2000U|mask))||now-__atomic_load_n(&close_asked_at,__ATOMIC_ACQUIRE)>3000000)) {
            release_input(__atomic_load_n(&close_pending,__ATOMIC_ACQUIRE));
            __atomic_store_n(&close_pending,(std::uint32_t)NoRelease,__ATOMIC_RELEASE);
        }
#ifndef PROBE_PLAYER
        else {
            const auto directions=buttons&0xf0U;
            bool repeat=directions&&(directions!=(previous&0xf0U)||now>=next_repeat);
            if(repeat) {
                auto selection=__atomic_load_n(&state.selection,__ATOMIC_RELAXED)%3;
                if(directions&(0x10U|0x80U)) selection=(selection+2)%3;
                else if(directions&(0x20U|0x40U)) selection=(selection+1)%3;
                __atomic_store_n(&state.selection,selection,__ATOMIC_RELAXED);
                next_repeat=now+(directions!=(previous&0xf0U)?350000:120000);
            }
            if((buttons&0x4000U)&&!(previous&0x4000U))
                __atomic_fetch_add(&state.enter_count,1,__ATOMIC_RELAXED);
        }
#endif
    }
    if(!__atomic_load_n(&state.input_active,__ATOMIC_ACQUIRE))
        __atomic_store_n(&close_pending,(std::uint32_t)NoRelease,__ATOMIC_RELEASE);
#ifdef PROBE_PLAYER
    // The left stick moves like the D-pad. A closing panel takes no more input.
    const bool steering=__atomic_load_n(&state.input_active,__ATOMIC_ACQUIRE)&&!closing();
    overlay_console::input(steering?buttons|stick_directions(stick):0,steering);
#endif
    previous=buttons;
}
