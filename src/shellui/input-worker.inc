/* Timed ShellUI key capture. SPDX-License-Identifier: GPL-3.0-or-later */
using EnumGetter=int(*)(MonoObject*);
static EnumGetter key_getter,type_getter,device_getter;
static MonoMethod *color_setter,*width_setter;
extern "C" __attribute__((naked,noinline,used,aligned(16384))) void input_trampoline() {
    __asm__ volatile(".rept 128\nnop\n.endr\nret\n");
}
static std::uint64_t circle_quiet_until=0; // Circle events are swallowed until then
static void release_input(std::uint32_t reason) {
    // No filesystem, managed invocation or dependency on Update in this path.
    if(reason==BackButton||reason==ToggleClose)
        __atomic_store_n(&circle_quiet_until,sceKernelGetProcessTime()+400000,__ATOMIC_RELEASE);
    __atomic_store_n(&state.input_active,0,__ATOMIC_RELEASE);
    std::uint32_t expected=NoRelease;
    if(__atomic_compare_exchange_n(&state.release_reason,&expected,reason,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
        __atomic_store_n(&state.input_released,sceKernelGetProcessTime(),__ATOMIC_RELEASE);
}
extern "C" std::int32_t input_next_key(MonoObject* dispatcher,MonoObject** event,MonoObject** widget,MonoObject** history) {
    auto original=reinterpret_cast<bool(*)(MonoObject*,MonoObject**,MonoObject**,MonoObject**)>(state.key_original);
    // The parent sets this before writing the hook. An invalid original never
    // starts capture; normally this method always forwards to it first.
    if(!original) { release_input(SetupError); return false; }
    // Canonical full-register 0/1 works with either AL or EAX boolean callers.
    const std::int32_t available=original(dispatcher,event,widget,history)?1:0;
    if(!available) return available;
    if(!__atomic_load_n(&state.input_active,__ATOMIC_ACQUIRE)) {
        // Just closed with Circle: its remaining events (the key-up) are not home's.
        if(sceKernelGetProcessTime()<__atomic_load_n(&circle_quiet_until,__ATOMIC_ACQUIRE)&&event&&*event&&
           key_getter&&key_getter(*event)==6) {
            __atomic_fetch_add(&state.events_consumed,1,__ATOMIC_RELAXED);
            return false;
        }
        return available;
    }
    auto now=sceKernelGetProcessTime();
    auto begin=__atomic_load_n(&state.input_started,__ATOMIC_ACQUIRE);
    auto end=__atomic_load_n(&state.input_deadline,__ATOMIC_ACQUIRE);
    auto heartbeat=__atomic_load_n(&state.guardian_heartbeat,__ATOMIC_ACQUIRE);
    if(!begin||end<=begin||end-begin>input_limit_us||now>=end) { release_input(Deadline); return available; }
    if(!heartbeat||now<heartbeat||now-heartbeat>500000) { release_input(GuardianLost); return available; }
    if(__atomic_load_n(&state.stop,__ATOMIC_ACQUIRE)) { release_input(StopRequest); return available; }
    if(!event||!*event) { release_input(SetupError); return available; }
    // These are compiled primitive, compiler-generated getters: no allocation,
    // boxed values, field-offset guesses, event delegates or focus changes.
    const int key=key_getter(*event),type=type_getter(*event),device=device_getter(*event);
    __atomic_fetch_add(&state.events_seen,1,__ATOMIC_RELAXED);
    __atomic_store_n(&state.last_key,(std::uint32_t)key,__ATOMIC_RELAXED);
    __atomic_store_n(&state.last_type,(std::uint32_t)type,__ATOMIC_RELAXED);
    __atomic_store_n(&state.last_device,(std::uint32_t)device,__ATOMIC_RELAXED);
    bool controller=device==1||device==3||device==6||device==9||device==10||device==12||device==13;
    if(!controller||type<0||type>3) return available;
#ifdef PROBE_TOGGLE
    // Circle: swallowed, and input is given back once it is up (toggle_poll), so neither
    // home nor the game takes it as a press of its own.
    if(key==6) {
#ifndef PROBE_PLAYER
        request_close_on_release(BackButton);
#endif
        // The control centre acts on the pad's own Circle (toggle_poll): back or close.
        __atomic_fetch_add(&state.events_consumed,1,__ATOMIC_RELAXED);
        return false;
    }
#else
    if(key==6) { release_input(BackButton); return available; }
#endif
#ifdef PROBE_PLAYER
    if(!((key>=1&&key<=5)||key==7||key==11||key==12)) return available;
#else
    if(key<1||key>5) return available; // PS/Options and other controls pass through.
#endif
    // Recheck after the getters; an expiry cannot be extended by reading input.
    if(!__atomic_load_n(&state.input_active,__ATOMIC_ACQUIRE)||sceKernelGetProcessTime()>=end) {
        release_input(Deadline); return available;
    }
#ifndef PROBE_TOGGLE
    if(type==1||type==3) { // Down or Repeat; Up is swallowed during this lease too.
        auto selected=__atomic_load_n(&state.selection,__ATOMIC_RELAXED)%3;
        if(key==1||key==2) selected=(selected+2)%3;
        if(key==3||key==4) selected=(selected+1)%3;
        __atomic_store_n(&state.selection,selected,__ATOMIC_RELAXED);
        if(key==5&&type==1) __atomic_fetch_add(&state.enter_count,1,__ATOMIC_RELAXED);
    }
#endif
    __atomic_fetch_add(&state.events_consumed,1,__ATOMIC_RELAXED);
    return false; // Original retrieval advanced the event queue; stop this dispatch.
}
static bool bind_input(MonoImage* image) {
    auto* dispatcher=mono_class_from_name(image,"Sce.PlayStation.PUI","EventDispatcher");
    auto* keys=mono_class_from_name(image,"Sce.PlayStation.PUI","KeyEventArgs");
    auto compile=[](MonoMethod* m)->void* { return m?mono_compile_method(m):nullptr; };
    state.key_method=(std::uint64_t)compile(method(dispatcher,"GetNextKeyEvent",3));
    state.key_hook=(std::uint64_t)&input_next_key;
    state.key_trampoline=(std::uint64_t)&input_trampoline;
    key_getter=reinterpret_cast<EnumGetter>(compile(property(keys,"KeyType",false)));
    type_getter=reinterpret_cast<EnumGetter>(compile(property(keys,"KeyEventType",false)));
    device_getter=reinterpret_cast<EnumGetter>(compile(property(keys,"DeviceType",false)));
    color_setter=property(panel_class,"BackgroundColor",true);
    width_setter=property(widget_class,"Width",true);
    bool ok=state.key_method&&key_getter&&type_getter&&device_getter&&color_setter&&width_setter;
    log("input binding=%d key_method=%p getters=%p,%p,%p",ok,(void*)state.key_method,
        (void*)key_getter,(void*)type_getter,(void*)device_getter);
    return ok;
}
[[maybe_unused]] static bool input_set(MonoMethod* setter,std::uint32_t handle,void* value) {
    if(!handle) return false;
    void* args[]={value}; MonoObject* exception=nullptr;
    mono_runtime_invoke(setter,mono_gchandle_get_target(handle),args,&exception);
    if(exception) { release_input(SetupError); describe_exception(setter,exception); return false; }
    return true;
}
static void update_input_ui() {
#ifdef PROBE_NATIVE
    native_ui();
#elif defined(PROBE_BOREALIS)
    borealis_ui();
#else
    auto now=sceKernelGetProcessTime();
#ifndef PROBE_TOGGLE
    if(!__atomic_load_n(&state.input_started,__ATOMIC_ACQUIRE)&&
       !__atomic_load_n(&state.release_reason,__ATOMIC_ACQUIRE)&&now-state.started>=input_delay_us) {
        auto* appended=get(scene_class,mono_gchandle_get_target(scene_handle),"IsAppended");
        auto heartbeat=__atomic_load_n(&state.guardian_heartbeat,__ATOMIC_ACQUIRE);
        if(!appended||!*static_cast<unsigned char*>(mono_object_unbox(appended))) release_input(SetupError);
        else if(!heartbeat||now<heartbeat||now-heartbeat>500000) release_input(GuardianLost);
        else {
            __atomic_store_n(&state.input_started,now,__ATOMIC_RELEASE);
            __atomic_store_n(&state.input_deadline,now+input_limit_us,__ATOMIC_RELEASE);
            __atomic_store_n(&state.input_active,1,__ATOMIC_RELEASE);
        }
    }
#endif
    if(__atomic_load_n(&state.input_active,__ATOMIC_ACQUIRE)&&now>=__atomic_load_n(&state.input_deadline,__ATOMIC_ACQUIRE))
        release_input(Deadline);
    static std::uint64_t painted=0;
    if(now-painted<100000) return; painted=now;
    auto active=__atomic_load_n(&state.input_active,__ATOMIC_ACQUIRE);
    auto started=__atomic_load_n(&state.input_started,__ATOMIC_ACQUIRE);
    auto reason=__atomic_load_n(&state.release_reason,__ATOMIC_ACQUIRE);
#ifdef PROBE_TOGGLE
    static int was_visible=-1;
    int visible=active?1:0;
    if(visible!=was_visible) {
        if(!input_set(property(widget_class,"Visible",true),visibility_handle,&visible)) {
            release_input(SetupError);
            __atomic_store_n(&state.phase,Removing,__ATOMIC_RELEASE);
            return;
        }
        was_visible=visible;
        log("TOGGLE panel visible=%d toggles=%llu",visible,(unsigned long long)__atomic_load_n(&state.toggles,__ATOMIC_RELAXED));
    }
    if(!visible) return;
#endif
    float color[]={active?1.0f:reason?0.0f:1.0f,active?0.0f:1.0f,0,1};
    (void)input_set(color_setter,status_handle,color);
    float fraction=1;
    if(started) {
        auto end=__atomic_load_n(&state.input_deadline,__ATOMIC_ACQUIRE);
        fraction=active&&now<end?(float)(end-now)/(float)input_limit_us:0;
    }
    float width=progress_width*fraction;
    (void)input_set(width_setter,progress_handle,&width);
    auto selected=__atomic_load_n(&state.selection,__ATOMIC_RELAXED)%3;
    auto enters=__atomic_load_n(&state.enter_count,__ATOMIC_RELAXED);
    for(unsigned i=0;i<3;++i) {
        float tile[]={i==0?1.0f:0.0f,i==1?1.0f:0.0f,i==2?1.0f:0.0f,i==selected?1.0f:0.25f};
        if(i==selected&&(enters&1)) tile[0]=tile[1]=tile[2]=1;
        (void)input_set(color_setter,tile_handles[i],tile);
    }
#endif
}
static void input_guardian_log() {
    static std::uint64_t seen=0,start_seen=0;
    static std::uint32_t reason_seen=0;
    auto begin=__atomic_load_n(&state.input_started,__ATOMIC_ACQUIRE);
    if(begin&&begin!=start_seen) {
        start_seen=begin;
        reason_seen=0;
        log("INPUT capture started max_ms=%llu deadline=%llu",(unsigned long long)(input_limit_us/1000),
            (unsigned long long)__atomic_load_n(&state.input_deadline,__ATOMIC_ACQUIRE));
    }
    auto events=__atomic_load_n(&state.events_seen,__ATOMIC_RELAXED);
    if(events!=seen) {
        seen=events;
        log("INPUT seen=%llu consumed=%llu key=%u type=%u device=%u selection=%u confirm=%u",
            (unsigned long long)events,(unsigned long long)__atomic_load_n(&state.events_consumed,__ATOMIC_RELAXED),
            __atomic_load_n(&state.last_key,__ATOMIC_RELAXED),__atomic_load_n(&state.last_type,__ATOMIC_RELAXED),
            __atomic_load_n(&state.last_device,__ATOMIC_RELAXED),__atomic_load_n(&state.selection,__ATOMIC_RELAXED),
            __atomic_load_n(&state.enter_count,__ATOMIC_RELAXED));
    }
    auto reason=__atomic_load_n(&state.release_reason,__ATOMIC_ACQUIRE);
    if(reason&&reason!=reason_seen) {
        reason_seen=reason;
        auto end=__atomic_load_n(&state.input_released,__ATOMIC_ACQUIRE);
        log("INPUT released reason=%u active=%u elapsed_ms=%llu",reason,
            __atomic_load_n(&state.input_active,__ATOMIC_ACQUIRE),(unsigned long long)(begin&&end>=begin?(end-begin)/1000:0));
    }
    if(!begin&&!reason) { seen=start_seen=0; reason_seen=0; }
}
