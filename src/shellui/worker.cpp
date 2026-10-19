/* Temporary passive home-scene panel. SPDX-License-Identifier: GPL-3.0-or-later */
#include "protocol.hpp"
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <unistd.h>

extern "C" {
struct MonoDomain; struct MonoAssembly; struct MonoImage; struct MonoClass;
struct MonoObject; struct MonoMethod; struct MonoProperty; struct MonoString;
MonoDomain* mono_get_root_domain();
void* mono_thread_attach(MonoDomain*);
MonoAssembly* mono_domain_assembly_open(MonoDomain*,const char*);
MonoImage* mono_assembly_get_image(MonoAssembly*);
MonoClass* mono_class_from_name(MonoImage*,const char*,const char*);
MonoClass* mono_object_get_class(MonoObject*);
MonoClass* mono_class_get_parent(MonoClass*);
const char* mono_class_get_name(MonoClass*);
const char* mono_class_get_namespace(MonoClass*);
const char* mono_method_get_name(MonoMethod*);
MonoClass* mono_method_get_class(MonoMethod*);
MonoMethod* mono_class_get_method_from_name(MonoClass*,const char*,int);
MonoProperty* mono_class_get_property_from_name(MonoClass*,const char*);
MonoMethod* mono_property_get_get_method(MonoProperty*);
MonoMethod* mono_property_get_set_method(MonoProperty*);
MonoObject* mono_runtime_invoke(MonoMethod*,void*,void**,MonoObject**);
MonoObject* mono_object_new(MonoDomain*,MonoClass*);
char* mono_string_to_utf8(MonoString*);
void mono_free(void*);
void* mono_object_unbox(MonoObject*);
void* mono_compile_method(MonoMethod*);
std::uint32_t mono_gchandle_new(MonoObject*,int);
MonoObject* mono_gchandle_get_target(std::uint32_t);
void mono_gchandle_free(std::uint32_t);
std::uint64_t sceKernelGetProcessTime();
}
using namespace home_bridge;
static State state;
static MonoDomain* domain;
static MonoClass *application_class,*scene_class,*container_class,*widget_class,*panel_class,*ui_class;
static std::uint32_t scene_handle,root_handle;
static bool removal_requested,failed;
#ifdef PROBE_INPUT
static std::uint32_t tile_handles[3],status_handle,progress_handle;
[[maybe_unused]] static float progress_width;
#ifdef PROBE_TOGGLE
static std::uint32_t visibility_handle;
static void toggle_poll();
static void request_close_on_release(std::uint32_t reason);
static bool closing();
#endif
static void release_input(std::uint32_t reason);
static bool bind_input(MonoImage* image);
static void update_input_ui();
#endif
extern "C" __attribute__((naked,noinline,used,aligned(16384))) void bridge_trampoline() {
    __asm__ volatile(".rept 128\nnop\n.endr\nret\n");
}
static void log(const char* fmt,...) {
    // /system_tmp holds only about 256 KB and the system needs it too: a filled one left
    // the worker unable to publish readiness (2026-10-15). Start the log again past 48 KB.
    FILE* f=std::fopen(shell_log,"a"); if(!f) return;
    if(std::fseek(f,0,SEEK_END)==0&&std::ftell(f)>48*1024) {
        std::fclose(f); f=std::fopen(shell_log,"w"); if(!f) return;
        std::fputs("[log started again at 48 KB]\n",f);
    }
    std::fprintf(f,"[pid=%d t=%llu] ",getpid(),(unsigned long long)sceKernelGetProcessTime());
    va_list ap; va_start(ap,fmt); std::vfprintf(f,fmt,ap); va_end(ap);
    std::fputc('\n',f); std::fclose(f);
}
static MonoMethod* property(MonoClass* c,const char* name,bool setter);
static MonoMethod* method(MonoClass* c,const char* name,int count) {
    for(;c;c=mono_class_get_parent(c)) {
        auto* m=mono_class_get_method_from_name(c,name,count);
        if(m) return m;
    }
    log("missing inherited method %s argc=%d",name,count); return nullptr;
}
static void describe_exception(MonoMethod* method,MonoObject* exception) {
    auto pin=mono_gchandle_new(exception,1);
    if(!pin) { log("exception diagnostic pin failed"); return; }
    exception=mono_gchandle_get_target(pin);
    auto* type=mono_object_get_class(exception);
    auto* owner=mono_method_get_class(method);
    log("managed exception method=%s.%s::%s type=%s.%s",
        mono_class_get_namespace(owner),mono_class_get_name(owner),mono_method_get_name(method),
        mono_class_get_namespace(type),mono_class_get_name(type));
    auto* getter=property(type,"Message",false);
    if(getter) {
        MonoObject* secondary=nullptr;
        auto* message=mono_runtime_invoke(getter,exception,nullptr,&secondary);
        if(message&&!secondary) {
            auto* utf8=mono_string_to_utf8(reinterpret_cast<MonoString*>(message));
            if(utf8) { log("exception message=%.1200s",utf8); mono_free(utf8); }
        } else log("exception Message getter failed");
    }
    mono_gchandle_free(pin);
}
static bool native_plugin_active=false,native_plugin_failed=false;
static bool native_try(MonoMethod*,void*,void**,MonoObject**);
static bool invoke(MonoMethod* m,MonoObject* obj,void** args=nullptr,MonoObject** output=nullptr) {
    if(native_plugin_active)return native_try(m,obj,args,output);
    if(!m) { log("missing method"); return false; }
    auto* owner=mono_method_get_class(m);
    log("UI call %s.%s::%s",mono_class_get_namespace(owner),mono_class_get_name(owner),mono_method_get_name(m));
    MonoObject* exception=nullptr;
    MonoObject* value=mono_runtime_invoke(m,obj,args,&exception);
    if(exception) { describe_exception(m,exception); return false; }
    if(output) *output=value;
    return true;
}
static MonoMethod* property(MonoClass* c,const char* name,bool setter) {
    for(;c;c=mono_class_get_parent(c)) {
        auto* p=mono_class_get_property_from_name(c,name);
        if(p) return setter?mono_property_get_set_method(p):mono_property_get_get_method(p);
    }
    return nullptr;
}
static bool set(MonoClass* c,MonoObject* obj,const char* name,void* value) {
    void* args[]={value}; auto* m=property(c,name,true);
    if(!m) { log("missing setter %s",name); return false; }
    return invoke(m,obj,args);
}
static MonoObject* get(MonoClass* c,MonoObject* obj,const char* name) {
    MonoObject* result=nullptr;
    return invoke(property(c,name,false),obj,nullptr,&result)?result:nullptr;
}
static std::uint32_t object(MonoClass* c) {
    auto* o=mono_object_new(domain,c); if(!o) return 0;
    auto h=mono_gchandle_new(o,1); if(!h) return 0;
    if(!invoke(mono_class_get_method_from_name(c,".ctor",0),mono_gchandle_get_target(h))) {
        mono_gchandle_free(h); return 0;
    }
    return h;
}
[[maybe_unused]] static bool panel(MonoObject* parent,float x,float y,float w,float h,float r,float g,float b,float a,std::uint32_t* retained=nullptr) {
    auto pin=object(panel_class); if(!pin) return false;
    auto* obj=mono_gchandle_get_target(pin);
    int no=0,yes=1; float color[]={r,g,b,a};
    bool ok=set(widget_class,obj,"X",&x)&&set(widget_class,obj,"Y",&y)&&
        set(widget_class,obj,"Width",&w)&&set(widget_class,obj,"Height",&h)&&
        set(widget_class,obj,"Focusable",&no)&&set(widget_class,obj,"Visible",&yes)&&
        set(panel_class,obj,"BackgroundColor",color);
    if(ok) { void* args[]={obj}; ok=invoke(mono_class_get_method_from_name(widget_class,"AppendChild",1),parent,args); }
    if(ok&&retained) *retained=pin;
    else mono_gchandle_free(pin);
    return ok;
}
#ifdef PROBE_NATIVE
#include "native-worker.inc"
#elif defined(PROBE_BOREALIS)
#include "borealis-worker.inc"
#endif
static bool create(MonoObject* app) {
    log("UI stage=root scene application=%p",(void*)app);
    auto* root=get(application_class,app,"RootScene"); if(!root) return false;
    root_handle=mono_gchandle_new(root,1); if(!root_handle) return false;
    log("UI stage=scene constructor");
    scene_handle=object(scene_class); if(!scene_handle) return false;
    auto* scene=mono_gchandle_get_target(scene_handle);
    log("UI stage=scene properties");
    int yes=1,no=0;
    if(!set(scene_class,scene,"Focusable",&no)||
       !set(scene_class,scene,"ScreenFixedTransform",&yes)||!set(scene_class,scene,"ScreenRenderOverlay",&yes)) return false;
    log("UI stage=root widget");
    auto* widget=get(scene_class,scene,"RootWidget"); if(!widget) return false;
    auto widget_pin=mono_gchandle_new(widget,1); if(!widget_pin) return false;
#ifdef PROBE_TOGGLE
    visibility_handle=mono_gchandle_new(widget,1);
    if(!visibility_handle||!set(widget_class,widget,"Visible",&no)) {
        mono_gchandle_free(widget_pin); return false;
    }
#endif
    float width=1920,height=1080;
    auto* sw=get(ui_class,nullptr,"ScreenWidth");
    if(sw) width=(float)*static_cast<int*>(mono_object_unbox(sw));
    auto* sh=get(ui_class,nullptr,"ScreenHeight");
    if(sh) height=(float)*static_cast<int*>(mono_object_unbox(sh));
    bool ok=width>=640&&width<=8192&&height>=480&&height<=8192;
    [[maybe_unused]] const float scale=width/1920.0f;
    [[maybe_unused]] float x=width-480*scale,y=120*scale,w=432*scale,h=720*scale;
    log("UI stage=panel widgets screen=%.0fx%.0f",width,height);
#ifdef PROBE_NATIVE
    if(ok) ok=create_native(widget,width,height);
#elif defined(PROBE_BOREALIS)
    if(ok) ok=create_borealis(widget,width,height);
#else
    if(ok) ok=panel(widget,x,y,w,h,0.8f,0,0.8f,1)&&
              panel(widget,x+8*scale,y+8*scale,w-16*scale,h-16*scale,0.1f,0.1f,0.1f,0.8f)&&
              panel(widget,x+16*scale,y+16*scale,128*scale,100*scale,1,0,0,1
#ifdef PROBE_INPUT
                    ,&tile_handles[0]
#endif
              )&&panel(widget,x+144*scale,y+16*scale,128*scale,100*scale,0,1,0,1
#ifdef PROBE_INPUT
                    ,&tile_handles[1]
#endif
              )&&panel(widget,x+272*scale,y+16*scale,128*scale,100*scale,0,0,1,1
#ifdef PROBE_INPUT
                    ,&tile_handles[2]
#endif
              );
#ifdef PROBE_INPUT
    progress_width=w-32*scale;
    if(ok) ok=panel(widget,x+16*scale,y+h-80*scale,progress_width,24*scale,1,1,0,1,&status_handle)&&
              panel(widget,x+16*scale,y+h-40*scale,progress_width,16*scale,1,1,1,1,&progress_handle);
#endif
#endif
    mono_gchandle_free(widget_pin);
    if(!ok) return false;
    log("UI stage=add scene");
    void* args[]={scene};
    auto* root_class=mono_object_get_class(root);
    log("root class=%s.%s",mono_class_get_namespace(root_class),mono_class_get_name(root_class));
    if(!invoke(method(root_class,"AddScene",1),root,args)) return false;
    log("scene add requested root=%p scene=%p screen=%.0fx%.0f passive=1 overlay=1",(void*)root,(void*)scene,width,height);
    return true;
}
static bool remove() {
#ifdef PROBE_INPUT
    release_input(StopRequest);
#endif
#ifdef PROBE_NATIVE
    if(!native_detach()) return false; // the glass focus scene goes first
#endif
    auto* scene=scene_handle?mono_gchandle_get_target(scene_handle):nullptr;
    bool attached=false;
    if(scene) {
        auto* appended=get(scene_class,scene,"IsAppended");
        if(!appended) return false;
        attached=*static_cast<unsigned char*>(mono_object_unbox(appended))!=0;
    }
    if(attached&&!removal_requested) {
        if(!invoke(mono_class_get_method_from_name(scene_class,"RemoveFromSuperScene",0),scene)) return false;
        removal_requested=true; log("scene removal requested");
        return false; // Allow the ordinary UI update to process queued removal.
    }
    if(attached) return false;
#ifdef PROBE_NATIVE
    dispose_native();
#elif defined(PROBE_BOREALIS)
    dispose_borealis();
#endif
    if(scene_handle) mono_gchandle_free(scene_handle);
    if(root_handle) mono_gchandle_free(root_handle);
#ifdef PROBE_INPUT
    for(auto& handle:tile_handles) { if(handle) mono_gchandle_free(handle); handle=0; }
    if(status_handle) mono_gchandle_free(status_handle);
    if(progress_handle) mono_gchandle_free(progress_handle);
    status_handle=progress_handle=0;
#ifdef PROBE_TOGGLE
    if(visibility_handle) mono_gchandle_free(visibility_handle);
    visibility_handle=0;
#endif
#endif
    scene_handle=root_handle=0;
    state.removed=1; log("scene detached; handles released"); return true;
}
#ifdef PROBE_INPUT
#include "input-worker.inc"
#ifdef PROBE_TOGGLE
#include "toggle-worker.inc"
#endif
#endif
extern "C" void bridge_update(MonoObject* app) {
    auto original=reinterpret_cast<void(*)(MonoObject*)>(state.original);
    auto phase=__atomic_load_n(&state.phase,__ATOMIC_ACQUIRE);
    if(phase==Armed) {
        state.started=sceKernelGetProcessTime(); state.frames=0; state.removed=0;
        removal_requested=false; failed=!create(app);
#ifdef PROBE_INPUT
        if(failed) release_input(SetupError);
#endif
        __atomic_store_n(&state.phase,failed?Removing:Running,__ATOMIC_RELEASE);
        if(failed) log("scene setup failed; attempting cleanup");
    }
    phase=__atomic_load_n(&state.phase,__ATOMIC_ACQUIRE);
    if(phase==Running) {
        ++state.frames;
#ifdef PROBE_TOGGLE
        __atomic_store_n(&state.ui_heartbeat,sceKernelGetProcessTime(),__ATOMIC_RELEASE);
#endif
#ifdef PROBE_INPUT
        update_input_ui();
#endif
        if(state.frames==2) {
            auto* appended=get(scene_class,mono_gchandle_get_target(scene_handle),"IsAppended");
            log("scene IsAppended=%d",appended?(int)*static_cast<unsigned char*>(mono_object_unbox(appended)):-1);
        }
        if(__atomic_load_n(&state.stop,__ATOMIC_ACQUIRE)||
           (state.frames%15==0&&access(worker_stop_path,F_OK)==0)||
           sceKernelGetProcessTime()-state.started >= (std::uint64_t)state.seconds*1000000)
            __atomic_store_n(&state.phase,Removing,__ATOMIC_RELEASE);
    }
    if(__atomic_load_n(&state.phase,__ATOMIC_ACQUIRE)==Removing&&remove())
        __atomic_store_n(&state.phase,failed?Failed:Done,__ATOMIC_RELEASE);
    if(original) original(app);
}
extern "C" __attribute__((noreturn,visibility("default"))) void* elf_main(void*) {
#ifdef PROBE_PLAYER
    // The manual loader supplies relocations. The linker folds Prospero's
    // legacy .ctors into this range, alongside modern .init_array entries.
    const auto constructors=overlay_init_end-overlay_init_begin;
    log("OVERLAY constructors begin count=%llu",(unsigned long long)constructors);
    if(constructors<=0||constructors>4096) {
        log("OVERLAY invalid constructor range; worker inert");
        for(;;) usleep(1000000);
    }
    for(auto* ctor=overlay_init_begin;ctor!=overlay_init_end;++ctor) {
        const auto entry=reinterpret_cast<std::uintptr_t>(*ctor);
        if(!user_address(entry)) {
            log("OVERLAY invalid constructor index=%llu; worker inert",(unsigned long long)(ctor-overlay_init_begin));
            for(;;) usleep(1000000);
        }
        log("OVERLAY constructor index=%llu address=%p",(unsigned long long)(ctor-overlay_init_begin),(void*)entry);
        (*ctor)();
    }
    log("OVERLAY constructors complete count=%llu",(unsigned long long)constructors);
#endif
    log("BEGIN bridge worker version=%u",protocol_version);
    state.pid=getpid(); state.hook=(std::uint64_t)&bridge_update;
    state.trampoline=(std::uint64_t)&bridge_trampoline;
    domain=mono_get_root_domain();
    if(domain&&mono_thread_attach(domain)) {
        auto* a=mono_domain_assembly_open(domain,"/system_ex/common_ex/lib/Sce.PlayStation.PUI.dll");
        auto* image=a?mono_assembly_get_image(a):nullptr;
        if(image) {
            application_class=mono_class_from_name(image,"Sce.PlayStation.PUI","Application");
            ui_class=mono_class_from_name(image,"Sce.PlayStation.PUI","UISystem");
            scene_class=mono_class_from_name(image,"Sce.PlayStation.PUI.UI2","Scene");
            container_class=mono_class_from_name(image,"Sce.PlayStation.PUI.UI2","ContainerScene");
            widget_class=mono_class_from_name(image,"Sce.PlayStation.PUI.UI2","Widget");
            panel_class=mono_class_from_name(image,"Sce.PlayStation.PUI.UI2","Panel");
            auto* method=application_class?mono_class_get_method_from_name(application_class,"Update",0):nullptr;
            state.update=method?(std::uint64_t)mono_compile_method(method):0;
#ifdef PROBE_INPUT
            if(!bind_input(image)) state.update=0;
#endif
#ifdef PROBE_NATIVE
            if(!bind_native(image)) { log("SHELLUI native binding failed"); state.update=0; }
#elif defined(PROBE_BOREALIS)
            if(!bind_borealis(image)) { log("BOREALIS texture binding failed"); state.update=0; }
#endif
        }
    }
    if(!state.update||!scene_class||!container_class||!widget_class||!panel_class||!ui_class) {
        state.phase=Failed; log("runtime binding failed; worker inert");
    } else {
#ifdef PROBE_PLAYER
        pthread_t drawing;
#ifdef PROBE_NATIVE
        const int rc=pthread_create(&drawing,nullptr,native_data_thread,nullptr);
        log("SHELLUI data thread create rc=%d",rc);
#else
        const int rc=pthread_create(&drawing,nullptr,borealis_draw_thread,nullptr);
        log("BOREALIS drawing thread create rc=%d",rc);
#endif
        if(rc) { state.player_failed=1; state.phase=Failed; }
#endif
        Ready ready{(std::uint64_t)&state,state};
        FILE* f=std::fopen(temp_path,"wb");
        bool ok=f&&std::fwrite(&ready,1,sizeof ready,f)==sizeof ready;
        if(f) ok=std::fclose(f)==0&&ok;
        if(ok) ok=std::rename(temp_path,ready_path)==0;
        log("READY published=%d update=%p state=%p",ok,(void*)state.update,(void*)&state);
    }
    // The upstream loader has observed instability when injected threads return.
    // Keep this small worker inert; the parent restores the hook after the panel.
    for(;;) {
#ifdef PROBE_INPUT
        __atomic_store_n(&state.guardian_heartbeat,sceKernelGetProcessTime(),__ATOMIC_RELEASE);
#ifdef PROBE_TOGGLE
        toggle_poll();
#endif
        if(__atomic_load_n(&state.input_active,__ATOMIC_ACQUIRE)) {
            auto now=sceKernelGetProcessTime();
            if(now>=__atomic_load_n(&state.input_deadline,__ATOMIC_ACQUIRE)) release_input(Deadline);
            else if(__atomic_load_n(&state.stop,__ATOMIC_ACQUIRE)||access(worker_stop_path,F_OK)==0)
                release_input(StopRequest);
        }
        input_guardian_log();
#if defined(PROBE_PLAYER)
        // Poll the pad at 125 Hz while the hook runs; Borealis then draws at 60 fps.
        usleep(__atomic_load_n(&state.phase,__ATOMIC_ACQUIRE)==Running?8000:1000000);
#elif defined(PROBE_TOGGLE)
        usleep(__atomic_load_n(&state.phase,__ATOMIC_ACQUIRE)==Running?20000:1000000);
#else
        usleep(__atomic_load_n(&state.input_active,__ATOMIC_ACQUIRE)?20000:100000);
#endif
#else
        usleep(1000000);
#endif
    }
}
