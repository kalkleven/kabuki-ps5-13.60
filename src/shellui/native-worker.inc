/* Native ShellUI/PUI control centre: native_layout.hpp over PUI widgets. No Borealis, NanoVG or card uploads. */
#include "native_layout.hpp"
#include <pthread.h>
#include <atomic>
#include <cmath>
#include <ctime>
#include <initializer_list>
#include <new>
#include <stdexcept>
#include <vector>
extern "C" {
struct MonoArray; struct MonoMethodSignature; struct MonoType;
MonoMethod* mono_class_get_methods(MonoClass*,void**);
MonoMethodSignature* mono_method_signature(MonoMethod*);
std::uint32_t mono_signature_get_param_count(MonoMethodSignature*);
MonoType* mono_signature_get_params(MonoMethodSignature*,void**);
char* mono_type_get_name(MonoType*);
MonoString* mono_string_new(MonoDomain*,const char*);
MonoClass* mono_get_byte_class();
MonoArray* mono_array_new(MonoDomain*,MonoClass*,std::uintptr_t);
char* mono_array_addr_with_size(MonoArray*,int,std::uintptr_t);
extern void (*overlay_init_begin[])();
extern void (*overlay_init_end[])();
}

namespace overlay_console {
static std::atomic<std::uint32_t> native_held{0},native_edges{0};
static std::atomic<bool> native_back{false},native_focused{false},native_nested{false};
// From Kabuki's settings (the data thread keeps them current): the shortcut's buttons and hold time,
// whether one is being recorded (no toggling meanwhile), and whether the interface plays sounds.
std::atomic<std::uint32_t> chord_mask{6};
std::atomic<std::uint64_t> chord_hold{500000};
std::atomic<bool> recording{false},sounds_on{true};
static void publish_settings(const overlay::NativeView& v) {
    if(__builtin_popcount(v.settings.chord)>=2) chord_mask.store(v.settings.chord);
    chord_hold.store(std::uint64_t(v.settings.hold_ms)*1000);
    recording.store(v.recording); sounds_on.store(v.settings.sounds);
}
bool card_focused() { return native_focused.load(); }
// In a menu inside the card (a popup, or an open playlist's tracks): Circle steps back out of it.
bool nested() { return native_nested.load(); }
void back_to_rail() { native_back.store(true); }
void input(std::uint32_t buttons,bool active) {
    if(!active) { native_held.store(0); native_edges.store(0); return; }
    const auto previous=native_held.exchange(buttons);
    native_edges.fetch_or(buttons&~previous);
}
void send_command(int kind,int value,const char* text=nullptr) {
    const auto seq=__atomic_load_n(&state.command_seq,__ATOMIC_RELAXED);
    state.command_kind[seq%8]=kind; state.command_value[seq%8]=value;
    std::snprintf(state.command_text[seq%8],sizeof state.command_text[0],"%s",text?text:"");
    __atomic_store_n(&state.command_seq,seq+1,__ATOMIC_RELEASE);
    log("SHELLUI command kind=%d value=%d seq=%u",kind,value,seq+1);
}
}
static pthread_mutex_t native_snapshot_lock=PTHREAD_MUTEX_INITIALIZER;
static overlay::NativeView native_snapshot;
static std::uint64_t native_snapshot_seq=0; // changes with each new snapshot (under the lock)
static std::vector<int> native_sounds; // console UI sounds to play, in order (under the lock)
static void* native_data_thread(void*) {
    try {
        overlay::NativeModel model(overlay_console::send_command);
        overlay_console::publish_settings(model.view()); // the saved shortcut, before any opening
        __atomic_store_n(&state.player_ready,1,__ATOMIC_RELEASE);
        for(;;) {
            if(__atomic_load_n(&state.phase,__ATOMIC_ACQUIRE)!=Running ||
               !__atomic_load_n(&state.input_active,__ATOMIC_ACQUIRE)) { usleep(20000); continue; }
            timespec t{}; clock_gettime(CLOCK_MONOTONIC,&t);
            const auto now=std::uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;
            model.poll_files(now);
            model.tick(now,__atomic_load_n(&state.input_started,__ATOMIC_ACQUIRE),
                       overlay_console::native_held.load(),overlay_console::native_edges.exchange(0),
                       overlay_console::native_back.exchange(false));
            auto view=model.view();
            overlay_console::native_focused.store(view.focused);
            overlay_console::native_nested.store(view.nested);
            overlay_console::publish_settings(view);
            pthread_mutex_lock(&native_snapshot_lock);
            native_snapshot=std::move(view); __atomic_fetch_add(&native_snapshot_seq,1,__ATOMIC_RELAXED);
            for(int sound:model.take_sounds()) if(native_sounds.size()<32) native_sounds.push_back(sound);
            pthread_mutex_unlock(&native_snapshot_lock);
            usleep(16000);
        }
    } catch(const std::exception& e) { log("SHELLUI data worker failed: %s",e.what()); }
      catch(...) { log("SHELLUI data worker failed"); }
    __atomic_store_n(&state.player_failed,1,__ATOMIC_RELEASE); release_input(SetupError);
    for(;;) usleep(1000000);
}

// One PUI widget and what was last set on it (a value is only set again when it changes).
struct NativeWidget {
    std::uint32_t handle=0,asset=0,texture=0,font=0,transform=0,parent=0;
    std::string text,uri,measured_text;
    std::string missing; std::uint64_t missing_at=0; // a picture file not there (yet): looked for again in 1 s
    float measured=0;
    float x=-9999,y=-9999,w=-1,h=-1,alpha=-1;
    float red=-1,green=-1,blue=-1,tint_alpha=-1;
    int visible=-1;
    // The card's pop: its Transform's scale, and the unscaled box it is scaled from.
    float scale=-1,base_x=0,base_y=0,base_w=0,base_h=0;
    bool pivot_centre=false;
};
static MonoClass *native_label_class,*native_font_class,*native_image_class,*native_asset_class,*native_texture_class,*native_transform_class;
static MonoMethod *native_font_ctor,*native_asset_uri_ctor,*native_asset_texture_ctor,*native_texture_ctor,*native_pixels;
static MonoMethod *native_x,*native_y,*native_width,*native_height,*native_opacity,*native_visible;
static MonoMethod *native_text,*native_font,*native_color,*native_image,*native_filter,*native_text_width;
static MonoMethod *native_register_resources,*native_unregister_resources;
static MonoMethod *native_get_transform,*native_reset_transform,*native_transform_ctor,*native_set_scale,*native_pivot_centre,*native_notify;
static bool native_resources_registered=false,native_scaling=true;
static bool native_ok=true;
static MonoMethod *native_focus_manager,*native_focus_scene; // FocusManager.Instance, FocusActiveScene
static float native_unit=1;
static NativeWidget native_root;
static overlay::NativeView native_view;
static std::uint64_t native_view_seq=0;

static MonoMethod* native_exact(MonoClass* type,const char* name,std::initializer_list<const char*> parameters) {
    for(auto* c=type;c;c=mono_class_get_parent(c)) {
        void* iter=nullptr;
        while(auto* m=mono_class_get_methods(c,&iter)) {
            if(std::strcmp(mono_method_get_name(m),name)) continue;
            auto* sig=mono_method_signature(m);
            if(mono_signature_get_param_count(sig)!=parameters.size()) continue;
            void* pi=nullptr; bool match=true;
            for(auto* expected:parameters) {
                char* actual=mono_type_get_name(mono_signature_get_params(sig,&pi));
                match=match&&actual&&!std::strcmp(actual,expected);
                if(actual) mono_free(actual);
            }
            if(match) return m;
        }
    }
    log("SHELLUI missing exact method %s.%s",mono_class_get_name(type),name); return nullptr;
}
static bool native_invoke(MonoMethod* m,void* object,void** args=nullptr) {
    if(native_plugin_active)return native_try(m,object,args,nullptr);
    if(!m) { native_ok=false; return false; }
    MonoObject* exception=nullptr;
    mono_runtime_invoke(m,object,args,&exception);
    if(exception) { describe_exception(m,exception); native_ok=false; return false; }
    return true;
}
// For optional features (the pop's scale, measuring text): a failure is logged and the
// panel carries on without it.
static bool native_try(MonoMethod* m,void* object,void** args,MonoObject** out=nullptr) {
    if(!m) {if(native_plugin_active)native_plugin_failed=true;return false;}
    MonoObject* exception=nullptr;
    auto* value=mono_runtime_invoke(m,object,args,&exception);
    if(exception) { describe_exception(m,exception); if(native_plugin_active)native_plugin_failed=true; return false; }
    if(out) *out=value;
    return true;
}
static void native_set(MonoMethod* setter,NativeWidget& widget,void* value) {
    if(!widget.handle) { native_ok=false; return; }
    void* args[]={value}; native_invoke(setter,mono_gchandle_get_target(widget.handle),args);
}
static void native_geometry(NativeWidget& box,float x,float y,float w,float h) {
    x*=native_unit; y*=native_unit; w*=native_unit; h*=native_unit;
    if(x!=box.x) { native_set(native_x,box,&x); box.x=x; }
    if(y!=box.y) {
        // Label overrides Y to maintain its baseline; use its own setter.
        auto* c=mono_object_get_class(mono_gchandle_get_target(box.handle));
        native_set(property(c,"Y",true),box,&y); box.y=y;
    }
    if(w!=box.w) { native_set(native_width,box,&w); box.w=w; }
    if(h!=box.h) { native_set(native_height,box,&h); box.h=h; }
}
static void native_show(NativeWidget& box,bool show) {
    int visible=show;
    if(box.visible!=visible) { native_set(native_visible,box,&visible); box.visible=visible; }
}
static void native_alpha(NativeWidget& box,float alpha) {
    if(std::fabs(box.alpha-alpha)>0.001f) { native_set(native_opacity,box,&alpha); box.alpha=alpha; }
}
static void native_words(NativeWidget& label,const std::string& text) {
    if(label.text==text) return;
    auto* str=mono_string_new(domain,text.c_str());
    const auto pin=str?mono_gchandle_new(reinterpret_cast<MonoObject*>(str),1):0;
    if(!pin) { native_ok=false; return; }
    native_set(native_text,label,mono_gchandle_get_target(pin));
    mono_gchandle_free(pin); label.text=text;
}
static void native_tint(NativeWidget& image,float r,float g,float b,float a=1) {
    if(std::fabs(image.red-r)<0.002f&&std::fabs(image.green-g)<0.002f&&std::fabs(image.blue-b)<0.002f&&
       std::fabs(image.tint_alpha-a)<0.002f) return;
    float color[]={r,g,b,a}; native_set(native_filter,image,color);
    image.red=r; image.green=g; image.blue=b; image.tint_alpha=a;
}
static bool native_append(NativeWidget& box,MonoClass* type,MonoObject* parent,float x,float y,float w,float h) {
    box.parent=mono_gchandle_new(parent,0);
    box.handle=object(type);
    if(!box.handle) return false;
    auto* obj=mono_gchandle_get_target(box.handle);
    int no=0;
    if(!set(widget_class,obj,"Focusable",&no)) return false;
    native_geometry(box,x,y,w,h); native_show(box,true);
    void* args[]={obj};
    return invoke(method(widget_class,"AppendChild",1),parent,args)&&native_ok;
}
static void native_release_asset(NativeWidget& box) {
    for(auto* h:{&box.asset,&box.texture}) if(*h) {
        auto* obj=mono_gchandle_get_target(*h);
        auto* dispose=method(mono_object_get_class(obj),"Dispose",0);
        if(dispose) native_invoke(dispose,obj);
        mono_gchandle_free(*h); *h=0;
    }
}
static std::uint32_t native_construct(MonoClass* type,MonoMethod* ctor,void** args) {
    auto* obj=mono_object_new(domain,type); if(!obj) return 0;
    const auto pin=mono_gchandle_new(obj,1);
    if(!pin||!native_invoke(ctor,mono_gchandle_get_target(pin),args)) {
        if(pin) mono_gchandle_free(pin); return 0;
    }
    return pin;
}
static void native_picture(NativeWidget& box,const std::string& uri) {
    if(box.uri==uri) return;
    if(uri.empty()) {
        native_set(native_image,box,nullptr); native_release_asset(box); box.uri.clear(); return;
    }
    // A cover file librespot-ps5 has not written yet, or has already pruned, is not an error: the
    // picture stays empty and is looked for again a second later. (ImageAsset threw
    // FileNotFoundException for a pruned queue cover and the whole panel closed, 2026-10-16.)
    const auto now=sceKernelGetProcessTime();
    if(box.missing==uri&&now-box.missing_at<1000000) return;
    const auto absent=[&] {
        if(box.missing!=uri) log("SHELLUI picture missing %s",uri.c_str());
        box.missing=uri; box.missing_at=now;
        if(box.asset) { native_set(native_image,box,nullptr); native_release_asset(box); }
        box.uri.clear();
    };
    if(uri.front()=='/'&&access(uri.c_str(),R_OK)!=0) { absent(); return; }
    const std::string address=uri.front()=='/'?"file://"+uri:uri;
    auto* text=mono_string_new(domain,address.c_str());
    const auto pin=text?mono_gchandle_new(reinterpret_cast<MonoObject*>(text),1):0;
    if(!pin) { native_ok=false; return; }
    auto* obj=mono_object_new(domain,native_asset_class);
    const auto asset=obj?mono_gchandle_new(obj,1):0;
    void* args[]={mono_gchandle_get_target(pin)};
    const bool made=asset&&native_try(native_asset_uri_ctor,mono_gchandle_get_target(asset),args);
    mono_gchandle_free(pin);
    if(!made) { if(asset) mono_gchandle_free(asset); absent(); return; }
    box.missing.clear();
    native_set(native_image,box,mono_gchandle_get_target(asset));
    native_release_asset(box); box.asset=asset; box.uri=uri;
}
static bool native_image_box(NativeWidget& box,MonoObject* parent,float x,float y,float w,float h,const std::string& uri="") {
    if(!native_append(box,native_image_class,parent,x,y,w,h)) return false;
    int no=0,inside=2;
    if(!set(native_image_class,mono_gchandle_get_target(box.handle),"DisposeImageOutOfScreen",&no)||
       !set(native_image_class,mono_gchandle_get_target(box.handle),"ImageScaleType",&inside)) return false;
    if(!uri.empty()) native_picture(box,uri);
    return native_ok;
}
// Skins (card, discs, rings, veil, the buffering light) are pictured once by native_layout.hpp
// at the screen's resolution and uploaded once. Text, covers and icons are native widgets.
static bool native_skin(NativeWidget& box,MonoObject* parent,const overlay::native::Box& b,int kind,float radius) {
    if(!native_image_box(box,parent,b.x,b.y,b.w,b.h)) return false;
    int pw=1,ph=1;
    overlay::native::skin_size(kind,b.w,b.h,native_unit,pw,ph);
    const auto pixels=overlay::native::skin_pixels(kind,b.w,b.h,radius,pw,ph);
    int no=0,format=1;
    void* texargs[]={&pw,&ph,&no,&format};
    box.texture=native_construct(native_texture_class,native_texture_ctor,texargs);
    if(!box.texture) return false;
    void* assetargs[]={mono_gchandle_get_target(box.texture)};
    box.asset=native_construct(native_asset_class,native_asset_texture_ctor,assetargs);
    auto* array=mono_array_new(domain,mono_get_byte_class(),pixels.size());
    const auto pin=array?mono_gchandle_new(reinterpret_cast<MonoObject*>(array),1):0;
    if(!pin||!box.asset) { if(pin) mono_gchandle_free(pin); return false; }
    auto* data=mono_array_addr_with_size(reinterpret_cast<MonoArray*>(mono_gchandle_get_target(pin)),1,0);
    std::memcpy(data,pixels.data(),pixels.size());
    int level=0; void* upload[]={&level,mono_gchandle_get_target(pin)};
    native_invoke(native_pixels,mono_gchandle_get_target(box.texture),upload); mono_gchandle_free(pin);
    native_set(native_image,box,mono_gchandle_get_target(box.asset));
    int stretch=1; set(native_image_class,mono_gchandle_get_target(box.handle),"ImageScaleType",&stretch);
    return native_ok;
}
static bool native_label(NativeWidget& label,MonoObject* parent,const overlay::native::Box& b,float size,int weight,
                         overlay::native::Rgb colour,int alignment,int trimming,const std::string& text) {
    if(!native_append(label,native_label_class,parent,b.x,b.y,b.w,b.h)) return false;
    auto* boxed=mono_object_new(domain,native_font_class);
    label.font=boxed?mono_gchandle_new(boxed,1):0;
    if(!label.font) return false;
    int font_size=int(std::lround(size*native_unit)),style=0;
    void* fontargs[]={&font_size,&style,&weight};
    if(!native_invoke(native_font_ctor,mono_object_unbox(mono_gchandle_get_target(label.font)),fontargs)) return false;
    native_set(native_font,label,mono_object_unbox(mono_gchandle_get_target(label.font)));
    float color[]={colour.r,colour.g,colour.b,1};
    native_set(native_color,label,color);
    int no=0,one=1;
    auto* obj=mono_gchandle_get_target(label.handle);
    if(!set(native_label_class,obj,"EnableThemedColor",&no)||
       !set(native_label_class,obj,"EnableStringifiedJsonWithId",&no)||
       !set(native_label_class,obj,"MaxLineCount",&one)||
       !set(native_label_class,obj,"HorizontalAlignment",&alignment)||
       !set(native_label_class,obj,"TextTrimming",&trimming)) return false;
    native_words(label,text); return native_ok;
}
// A label's text width in 1080p units, from its own font (remembered for the last text).
static float native_measure(NativeWidget& label,const std::string& text) {
    if(label.measured_text==text&&label.measured>0) return label.measured;
    float width=float(text.size())*11;
    if(label.font&&native_text_width) {
        auto* str=mono_string_new(domain,text.c_str());
        const auto pin=str?mono_gchandle_new(reinterpret_cast<MonoObject*>(str),1):0;
        MonoObject* out=nullptr;
        if(pin) {
            void* args[]={mono_gchandle_get_target(pin)};
            if(native_try(native_text_width,mono_object_unbox(mono_gchandle_get_target(label.font)),args,&out)&&out)
                width=*static_cast<float*>(mono_object_unbox(out))/native_unit;
            mono_gchandle_free(pin);
        }
    }
    label.measured_text=text; label.measured=width;
    return width;
}
// The console's pop: the card grows to full size as it fades in. A UI2 Panel's Transform
// setter "accepts only null" (13.20, 2026-10-15), so the card's own Transform is asked for
// (or given one through ResetTransform) and scaled in place. Without one, it only fades.
static void native_scale(NativeWidget& w,float s) {
    if(!native_scaling||!w.handle||std::fabs(s-w.scale)<0.0005f) return;
    auto* obj=mono_gchandle_get_target(w.handle);
    if(!w.transform) {
        MonoObject* t=nullptr;
        native_try(native_get_transform,obj,nullptr,&t);
        if(!t&&native_transform_class&&native_transform_ctor&&native_reset_transform) {
            auto* fresh=mono_object_new(domain,native_transform_class);
            const auto pin=fresh?mono_gchandle_new(fresh,1):0;
            if(pin&&native_try(native_transform_ctor,mono_gchandle_get_target(pin),nullptr)) {
                void* args[]={mono_gchandle_get_target(pin)};
                if(native_try(native_reset_transform,obj,args)) native_try(native_get_transform,obj,nullptr,&t);
            }
            if(pin) mono_gchandle_free(pin);
        }
        if(!t) { native_scaling=false; log("SHELLUI card scale unavailable; the pop only fades"); return; }
        w.transform=mono_gchandle_new(t,1);
        MonoObject* centre=nullptr;
        native_try(native_pivot_centre,mono_gchandle_get_target(w.transform),nullptr,&centre);
        w.pivot_centre=centre&&*static_cast<unsigned char*>(mono_object_unbox(centre));
        w.base_x=w.x; w.base_y=w.y; w.base_w=w.w; w.base_h=w.h;
        log("SHELLUI card scale through its Transform pivot_centre=%d",w.pivot_centre);
    }
    float v[3]={s,s,1}; void* args[]={v};
    if(!native_try(native_set_scale,mono_gchandle_get_target(w.transform),args)) {
        native_scaling=false; log("SHELLUI card scale failed; the pop only fades"); return;
    }
    if(native_notify) native_try(native_notify,obj,nullptr);
    w.scale=s;
    if(!w.pivot_centre) { // scaled about its top left: keep its centre in place
        float x=w.base_x+(1-s)*w.base_w/2,y=w.base_y+(1-s)*w.base_h/2;
        if(x!=w.x) { native_set(native_x,w,&x); w.x=x; }
        if(y!=w.y) { native_set(native_y,w,&y); w.y=y; }
    }
}
static std::string native_icon(const char* name) { return std::string("cxml://custom_overlay/")+name; }
// The console's "no cover" picture, from the React Native common assets (registered privately).
static bool native_common_registered=false;
static std::string native_fallback_art() {
    return native_common_registered?"cxml://custom_common/system_assets_Music_Fallback_01-500x500":native_icon("iconid_music");
}


// The console's own glass focus (PUI UI3): Sony's FocusRenderWidget (the glass, the shimmering
// ring, its show/hide, glide and press animations) in a UI3 scene of ours above the card, bound
// to invisible UI3 anchor widgets placed where the focused thing is. UI2 widgets (ours) only get
// the old UI2 focus, so the anchors stand in for them.
// /data/custom-overlays/focus-mode.txt chooses, without a rebuild (read when the panel is made):
//   system (default): PUI's focus manager focuses the anchors (the UI3 scene is focusable only
//   while open) and runs the glass itself; self: we drive a FocusRenderWidget (its per-frame
//   steps called from here froze ShellUI on 2026-10-16, so it is opt-in); off: our own rings.
static MonoClass *ui3_scene_class,*ui3_widget_class,*ui3_focus_class;
static MonoMethod *ui3_load,*ui3_root_widget,*ui3_append,*ui3_focus_ctor,*ui3_bind,*ui3_show,*ui3_hide,*ui3_warp,*ui3_press,*ui3_sync;
static MonoMethod *ui3_x,*ui3_y,*ui3_w,*ui3_h,*ui3_radius,*ui3_style,*ui3_focusable,*ui3_set_focus,*ui3_release,*ui3_scene_focusable;
static MonoMethod *ui3_scene_appended,*ui3_scene_remove,*ui3_visible,*ui3_sync_opacity,*ui3_sync_transform,*ui3_sync_render;
static std::uint32_t ui3_scene=0,ui3_root=0,ui3_focus=0;
struct Ui3Anchor { std::uint32_t handle=0; float x=-1,y=-1,w=-1,h=-1,r=-1; };
constexpr int ui3_anchor_count=64; // Focus ids 0..63
static Ui3Anchor ui3_anchors[ui3_anchor_count];
static int ui3_mode=2,ui3_bound=-1,ui3_presses=-1;
static bool ui3_attached=false,ui3_failed=false,ui3_shown=false,ui3_removing=false;
static float ui3_unit=1;
static std::uint64_t ui3_hidden_at=0;
static int ui3_root_visible=-1;

static void ui3_set(MonoMethod* setter,std::uint32_t handle,void* value) {
    void* args[]={value};
    if(!native_try(setter,mono_gchandle_get_target(handle),args)) ui3_failed=true;
}
static bool ui3_call(MonoMethod* m,std::uint32_t handle,void** args=nullptr) {
    if(!native_try(m,mono_gchandle_get_target(handle),args)) { ui3_failed=true; return false; }
    return true;
}
static void ui3_read_mode() {
    ui3_mode=2;
    if(FILE* f=std::fopen("/user/data/custom-overlays/focus-mode.txt","rb")) {
        char text[16]{}; (void)!std::fread(text,1,sizeof text-1,f); std::fclose(f);
        if(!std::strncmp(text,"off",3)) ui3_mode=0;
        else if(!std::strncmp(text,"self",4)) ui3_mode=1;
    }
}
// The UI3 scene goes in above ours once ours is up (scenes added later draw on top).
static bool ui3_attach() {
    ui3_attached=true;
    if(!ui3_scene_class||!ui3_widget_class||!ui3_root_widget||!ui3_append||
       (ui3_mode==1&&(!ui3_focus_class||!ui3_focus_ctor||!ui3_bind||!ui3_show||!ui3_hide))) {
        log("SHELLUI glass focus unavailable (bindings); rings stay"); return false;
    }
    ui3_scene=object(ui3_scene_class);
    if(!ui3_scene) { log("SHELLUI glass focus: UI3 scene failed"); return false; }
    auto* scene=mono_gchandle_get_target(ui3_scene);
    if(ui3_load&&!native_try(ui3_load,scene,nullptr)) log("SHELLUI glass focus: LoadSync threw (continuing)");
    int yes=1,no=0;
    set(ui3_scene_class,scene,"ScreenFixedTransform",&yes); set(ui3_scene_class,scene,"ScreenRenderOverlay",&yes);
    set(ui3_scene_class,scene,"Focusable",&no);
    MonoObject* root=nullptr;
    if(!native_try(ui3_root_widget,scene,nullptr,&root)||!root) { log("SHELLUI glass focus: no UI3 root widget"); return false; }
    ui3_root=mono_gchandle_new(root,1);
    if(auto* design=get(ui3_scene_class,scene,"DesignWidth")) {
        const float w=*static_cast<float*>(mono_object_unbox(design));
        ui3_unit=w>=640&&w<=8192?w/1920:native_unit;
        log("SHELLUI glass focus: UI3 DesignWidth=%.0f unit=%.3f (UI2 unit %.3f)",w,ui3_unit,native_unit);
    } else ui3_unit=native_unit;
    if(ui3_mode==1) {
        void* args[]={scene};
        ui3_focus=native_construct(ui3_focus_class,ui3_focus_ctor,args);
        native_ok=true; // a failed construction here only turns the glass off
        if(!ui3_focus) { log("SHELLUI glass focus: FocusRenderWidget constructor failed"); return false; }
        void* child[]={mono_gchandle_get_target(ui3_focus)};
        if(!native_try(ui3_append,root,child)) log("SHELLUI glass focus: FocusRenderWidget not appended (renders through the scene?)");
    }
    auto* ours=mono_gchandle_get_target(root_handle);
    void* add[]={scene};
    if(!native_try(method(mono_object_get_class(ours),"AddScene",1),ours,add)) { log("SHELLUI glass focus: AddScene failed"); return false; }
    log("SHELLUI glass focus attached mode=%s",ui3_mode==1?"self":"system");
    return !ui3_failed;
}
static std::uint32_t ui3_anchor(const overlay::native::Focus& f) {
    auto& a=ui3_anchors[f.id];
    if(!a.handle) {
        a.handle=object(ui3_widget_class);
        if(!a.handle) { ui3_failed=true; return 0; }
        int style=6,focusable=ui3_mode==2; // FocusStyle.RoundedRectangle
        if(ui3_style) ui3_set(ui3_style,a.handle,&style);
        if(ui3_focusable) ui3_set(ui3_focusable,a.handle,&focusable);
        void* child[]={mono_gchandle_get_target(a.handle)};
        if(!native_try(ui3_append,mono_gchandle_get_target(ui3_root),child)) { ui3_failed=true; return 0; }
    }
    float x=f.x*ui3_unit,y=f.y*ui3_unit,w=f.w*ui3_unit,h=f.h*ui3_unit,r=f.radius*ui3_unit;
    if(x!=a.x) { ui3_set(ui3_x,a.handle,&x); a.x=x; }
    if(y!=a.y) { ui3_set(ui3_y,a.handle,&y); a.y=y; }
    if(w!=a.w) { ui3_set(ui3_w,a.handle,&w); a.w=w; }
    if(h!=a.h) { ui3_set(ui3_h,a.handle,&h); a.h=h; }
    if(r!=a.r&&ui3_radius) { ui3_set(ui3_radius,a.handle,&r); a.r=r; }
    return a.handle;
}
// Without Sony's focus manager nothing runs the widget's per-frame steps: its show/hide, glide
// and press animations change values that only these apply (console, 2026-10-15: the ring
// stayed frozen on the scrub thumb). One that throws is dropped alone.
static void ui3_tick() {
    for(auto** m:{&ui3_sync,&ui3_sync_opacity,&ui3_sync_transform,&ui3_sync_render})
        if(*m&&!native_try(*m,mono_gchandle_get_target(ui3_focus),nullptr)) {
            log("SHELLUI glass focus: %s dropped",mono_method_get_name(*m)); *m=nullptr;
        }
}
// The whole glass layer is hidden once nothing has had the focus for a while, so a focus that
// did not hide can never stay on screen.
static void ui3_layer(bool visible) {
    if(!ui3_visible||ui3_root_visible==int(visible)) return;
    int value=visible;
    void* args[]={&value};
    if(native_try(ui3_visible,mono_gchandle_get_target(ui3_root),args)) ui3_root_visible=visible;
}
// Returns true while the console draws the focus (the layout then hides its own rings).
static bool native_focus(const overlay::native::Focus& f,int presses) {
    if(!ui3_mode||ui3_failed||ui3_removing) return false;
    if(!ui3_attached&&!ui3_attach()) { ui3_failed=true; return false; }
    const auto now=sceKernelGetProcessTime();
    if(ui3_mode==2&&f.id<0&&ui3_shown&&__atomic_load_n(&state.input_active,__ATOMIC_ACQUIRE)&&!closing())
        return !ui3_failed; // between cards: keep the focus (and ShellUI's) until the panel closes
    if(f.id<0||f.id>=ui3_anchor_count) {
        if(ui3_shown) ui3_hidden_at=now;
        if(ui3_mode==1&&ui3_focus&&now-ui3_hidden_at<600000) ui3_tick(); // its fade out
        else if(now-ui3_hidden_at>=600000) ui3_layer(false);
        if(ui3_shown) {
            if(ui3_mode==1) ui3_call(ui3_hide,ui3_focus);
            else if(ui3_bound>=0&&ui3_release) ui3_call(ui3_release,ui3_anchors[ui3_bound].handle);
            if(ui3_mode==2&&ui3_scene_focusable) { int no=0; ui3_set(ui3_scene_focusable,ui3_scene,&no); }
            ui3_shown=false; ui3_bound=-1;
        }
        return !ui3_failed;
    }
    const auto anchor=ui3_anchor(f);
    if(!anchor) return false;
    if(ui3_mode==1) {
        void* bind[]={mono_gchandle_get_target(anchor),mono_gchandle_get_target(anchor)};
        ui3_layer(true);
        if(!ui3_shown) {
            ui3_call(ui3_bind,ui3_focus,bind); ui3_call(ui3_show,ui3_focus);
            log("SHELLUI glass focus show id=%d",f.id);
        } else if(f.id!=ui3_bound) {
            ui3_call(ui3_bind,ui3_focus,bind);
            // The glide, the following and the press are extras: one that throws is dropped alone.
            if(ui3_warp&&!native_try(ui3_warp,mono_gchandle_get_target(ui3_focus),nullptr)) ui3_warp=nullptr;
        }
        if(ui3_shown&&presses!=ui3_presses&&ui3_press&&!native_try(ui3_press,mono_gchandle_get_target(ui3_focus),nullptr))
            ui3_press=nullptr;
        ui3_tick();
    } else {
        ui3_layer(true);
        if(!ui3_shown&&ui3_scene_focusable) { int yes=1; ui3_set(ui3_scene_focusable,ui3_scene,&yes); }
        if(f.id!=ui3_bound&&ui3_set_focus) {
            ui3_call(ui3_set_focus,anchor);
            if(!ui3_shown) log("SHELLUI glass focus (system) id=%d",f.id);
        }
    }
    ui3_presses=presses; ui3_shown=true; ui3_bound=f.id;
    if(ui3_failed) log("SHELLUI glass focus failed; rings return");
    return !ui3_failed;
}
// Takes the UI3 scene out before ours goes; true once it is gone (or never came).
static bool native_detach() {
    if(!ui3_scene) return true;
    auto* scene=mono_gchandle_get_target(ui3_scene);
    MonoObject* appended=nullptr;
    if(!native_try(ui3_scene_appended,scene,nullptr,&appended)||!appended) return true;
    if(!*static_cast<unsigned char*>(mono_object_unbox(appended))) return true;
    if(!ui3_removing) {
        ui3_removing=true;
        if(ui3_mode==2&&ui3_scene_focusable) { int no=0; void* a[]={&no}; native_try(ui3_scene_focusable,scene,a); }
        if(!native_try(ui3_scene_remove,scene,nullptr)) return true;
        log("SHELLUI glass focus scene removal requested");
    }
    return false;
}
static void ui3_dispose() {
    for(auto& a:ui3_anchors) { if(a.handle) mono_gchandle_free(a.handle); a=Ui3Anchor{}; }
    for(auto* h:{&ui3_focus,&ui3_root,&ui3_scene}) if(*h) { mono_gchandle_free(*h); *h=0; }
    ui3_attached=ui3_failed=ui3_shown=ui3_removing=false; ui3_bound=ui3_presses=ui3_root_visible=-1; ui3_hidden_at=0;
}
static void bind_ui3(MonoImage* pui) {
    ui3_scene_class=mono_class_from_name(pui,"Sce.PlayStation.PUI.UI3","Scene");
    ui3_widget_class=mono_class_from_name(pui,"Sce.PlayStation.PUI.UI3","Widget");
    ui3_focus_class=mono_class_from_name(pui,"Sce.PlayStation.PUI.UI3","FocusRenderWidget");
    if(!ui3_scene_class||!ui3_widget_class) return;
    ui3_load=native_exact(ui3_scene_class,"LoadSync",{});
    ui3_root_widget=native_exact(ui3_scene_class,"get_RootWidget",{});
    ui3_scene_focusable=native_exact(ui3_scene_class,"set_Focusable",{"System.Boolean"});
    ui3_scene_appended=native_exact(ui3_scene_class,"get_IsAppended",{});
    ui3_scene_remove=native_exact(ui3_scene_class,"RemoveFromSuperScene",{});
    ui3_append=native_exact(ui3_widget_class,"AppendChild",{"Sce.PlayStation.PUI.WidgetBase"});
    ui3_visible=property(ui3_widget_class,"Visible",true);
    ui3_x=property(ui3_widget_class,"X",true); ui3_y=property(ui3_widget_class,"Y",true);
    ui3_w=property(ui3_widget_class,"Width",true); ui3_h=property(ui3_widget_class,"Height",true);
    ui3_radius=native_exact(ui3_widget_class,"set_BorderRadius",{"System.Single"});
    ui3_style=native_exact(ui3_widget_class,"set_FocusStyle",{"Sce.PlayStation.PUI.UI3.FocusStyle"});
    ui3_focusable=native_exact(ui3_widget_class,"set_Focusable",{"System.Boolean"});
    ui3_set_focus=native_exact(ui3_widget_class,"SetFocus",{});
    ui3_release=native_exact(ui3_widget_class,"ReleaseFocus",{});
    if(ui3_focus_class) {
        ui3_focus_ctor=native_exact(ui3_focus_class,".ctor",{"Sce.PlayStation.PUI.UI3.Scene"});
        ui3_bind=native_exact(ui3_focus_class,"Bind",{"Sce.PlayStation.PUI.UI3.Widget","Sce.PlayStation.PUI.UI3.Widget"});
        ui3_show=native_exact(ui3_focus_class,"FocusShow",{});
        ui3_hide=native_exact(ui3_focus_class,"FocusHide",{});
        ui3_warp=native_exact(ui3_focus_class,"StartWarp",{});
        ui3_press=native_exact(ui3_focus_class,"StartPressEffect",{});
        ui3_sync=native_exact(ui3_focus_class,"SyncTarget",{});
        ui3_sync_opacity=native_exact(ui3_focus_class,"SyncOpacity",{});
        ui3_sync_transform=native_exact(ui3_focus_class,"SyncTransform",{});
        ui3_sync_render=native_exact(ui3_focus_class,"SyncRenderParam",{});
    }
    log("SHELLUI glass focus bindings scene=%d focus_widget=%d bind=%d show=%d warp=%d press=%d set_focus=%d",
        ui3_root_widget!=nullptr,ui3_focus_ctor!=nullptr,ui3_bind!=nullptr,ui3_show!=nullptr,ui3_warp!=nullptr,
        ui3_press!=nullptr,ui3_set_focus!=nullptr);
}

// The console's UI sounds. SystemSound's filter (OrbisPlayFilter) drops most sounds asked for
// from here: by type only the edge bump and "decide" were heard, by script name nothing
// (2026-10-16). So each sound is the console's own sound file, cxml://ui3/psfx_..., as a PUI
// SoundAsset played directly; those two keep the system type that already works.
static MonoClass* native_sound_asset_class;
static MonoMethod *native_play_sound,*native_sound_asset_ctor,*native_sound_asset_play;
static std::uint32_t native_system_sound=0;
static std::uint32_t native_sound_assets[128];
static bool native_sound_tried[128];
static bool native_sound_failed=false;
// The sound's data id (snd_*); its script (psfx_*) is the same name with the other prefix.
static const char* native_sound_file(int type) {
    using namespace overlay::sound;
    switch(type) {
    case focus_move: case list_move: return "snd_focus_move";
    case back: return "snd_cancel";
    case list_decide: return "snd_enter";
    case menu_open: return "snd_open_control_center";
    case menu_close: return "snd_close_control_center";
    case popup_open: return "snd_open_option_menu";
    case option_close: return "snd_close_option_menu";
    case slider: return "snd_slider_level_meter";
    case switch_on: return "snd_switch_on";
    case switch_off: return "snd_switch_off";
    case tab_enter: case play_pause: return "snd_enter"; // the console's select
    case skip: return "snd_change_panel";
    default: return nullptr; // decide and the edge bump: the system type plays them
    }
}
static MonoMethod* native_play_script; // SystemSound.Play(script, force): the fallback for a file
static bool native_script_tried[128];
// The asset for a sound type, made once (and kept while the panel exists).
static std::uint32_t native_sound_asset(int type) {
    if(type<0||type>=128) return 0;
    if(native_sound_tried[type]) return native_sound_assets[type];
    native_sound_tried[type]=true;
    const char* file=native_sound_file(type);
    if(!file||!native_sound_asset_class||!native_sound_asset_ctor||!native_sound_asset_play) return 0;
    // Our own registration of the UI3 RCO (the icons' prefix) first: under ShellUI's "ui3" prefix
    // the sound ids were not found (2026-10-16).
    for(const char* prefix:{"cxml://custom_overlay/","cxml://ui3/"}) {
        const std::string uri=std::string(prefix)+file;
        auto* text=mono_string_new(domain,uri.c_str());
        const auto pin=text?mono_gchandle_new(reinterpret_cast<MonoObject*>(text),1):0;
        if(!pin) return 0;
        auto* obj=mono_object_new(domain,native_sound_asset_class);
        const auto asset=obj?mono_gchandle_new(obj,1):0;
        void* args[]={mono_gchandle_get_target(pin)};
        const bool ok=asset&&native_try(native_sound_asset_ctor,mono_gchandle_get_target(asset),args);
        mono_gchandle_free(pin);
        if(!ok) { if(asset) mono_gchandle_free(asset); log("SHELLUI sound asset %s failed",uri.c_str()); continue; }
        native_sound_assets[type]=asset;
        log("SHELLUI sound asset %s ready",uri.c_str());
        return asset;
    }
    return 0;
}
static void native_sound_preload() {
    using namespace overlay::sound;
    for(int type:{focus_move,back,list_decide,menu_open,menu_close,popup_open,option_close,slider,switch_on,switch_off,tab_enter,
                  play_pause,skip})
        native_sound_asset(type);
}
static void native_sound(int type) {
    if(native_sound_failed) return;
    if(const auto asset=native_sound_asset(type)) {
        if(native_try(native_sound_asset_play,mono_gchandle_get_target(asset),nullptr)) return;
        log("SHELLUI sound asset play failed type=%d; using the system type",type);
        native_sound_assets[type]=0; mono_gchandle_free(asset);
    }
    if(!native_play_sound&&!native_play_script) return;
    if(!native_system_sound) {
        auto* ss=get(ui_class,nullptr,"SystemSound");
        if(!ss) { native_sound_failed=true; log("SHELLUI sounds unavailable: no SystemSound"); return; }
        native_system_sound=mono_gchandle_new(ss,1);
    }
    // No file: the console's script for it (psfx_*), forced past the system filter. Open and close
    // have no system type the console plays, so they rely on this.
    if(const char* file=native_sound_file(type); file&&native_play_script) {
        const std::string script=std::string("psfx_")+(file+4);
        auto* text=mono_string_new(domain,script.c_str());
        const auto pin=text?mono_gchandle_new(reinterpret_cast<MonoObject*>(text),1):0;
        if(pin) {
            int force=1; void* args[]={mono_gchandle_get_target(pin),&force};
            const bool ok=native_try(native_play_script,mono_gchandle_get_target(native_system_sound),args);
            mono_gchandle_free(pin);
            if(type>=0&&type<128&&!native_script_tried[type]) { native_script_tried[type]=true; log("SHELLUI sound script %s played=%d",script.c_str(),ok); }
            if(ok&&(type==overlay::sound::menu_open||type==overlay::sound::menu_close)) return;
        }
    }
    if(!native_play_sound) return;
    if(type==overlay::sound::menu_close) type=overlay::sound::back;
    if(type==overlay::sound::focus_move) type=overlay::sound::list_move; // FocusMoveByDirectionalKey is filtered
    if(type==overlay::sound::play_pause||type==overlay::sound::skip) type=overlay::sound::decide;
    void* args[]={&type,nullptr};
    if(!native_try(native_play_sound,mono_gchandle_get_target(native_system_sound),args)) {
        native_sound_failed=true; log("SHELLUI sounds off: PlaySoundType failed");
    }
}
static void native_sound_release() {
    for(int i=0;i<128;++i) {
        if(native_sound_assets[i]) mono_gchandle_free(native_sound_assets[i]);
        native_sound_assets[i]=0; native_sound_tried[i]=false; native_script_tried[i]=false;
    }
    if(native_system_sound) { mono_gchandle_free(native_system_sound); native_system_sound=0; }
}
static void bind_sounds(MonoImage* pui) {
    if(auto* type=mono_class_from_name(pui,"Sce.PlayStation.PUI","SystemSound")) {
        void* iter=nullptr;
        while(auto* m=mono_class_get_methods(type,&iter)) {
            if(std::strcmp(mono_method_get_name(m),"PlaySoundType")) continue;
            auto* sig=mono_method_signature(m);
            if(mono_signature_get_param_count(sig)!=2) continue;
            void* pi=nullptr;
            char* first=mono_type_get_name(mono_signature_get_params(sig,&pi));
            const bool match=first&&!std::strcmp(first,"Sce.PlayStation.PUI.SystemSoundType");
            if(first) mono_free(first);
            if(match) { native_play_sound=m; break; }
        }
    }
    native_sound_asset_class=mono_class_from_name(pui,"Sce.PlayStation.PUI","SoundAsset");
    if(native_sound_asset_class) {
        native_sound_asset_ctor=native_exact(native_sound_asset_class,".ctor",{"System.String"});
        native_sound_asset_play=native_exact(native_sound_asset_class,"Play",{});
    }
    if(auto* type=mono_class_from_name(pui,"Sce.PlayStation.PUI","SystemSound"))
        native_play_script=native_exact(type,"Play",{"System.String","System.Boolean"});
    log("SHELLUI sounds binding type=%d asset=%d",native_play_sound!=nullptr,native_sound_asset_play!=nullptr);
}

// native_layout.hpp's backend: PUI Label, ImageBox and Panel widgets.
struct NativeBackend {
    using Widget=NativeWidget;
    template<class F> bool plugin_scope(F fn) {
        const bool saved=native_ok, active=native_plugin_active, failed=native_plugin_failed;
        native_plugin_active=true;native_plugin_failed=false;
        const bool result=fn();
        const bool local_failed=native_plugin_failed||!native_ok;
        native_plugin_active=active;native_plugin_failed=failed||local_failed;native_ok=saved;
        return result&&!local_failed;
    }
    void plugin_remove(Widget& w) {
        if(w.handle) {auto* obj=of(w);if(w.parent) {void* args[]={obj};native_try(method(widget_class,"RemoveChild",1),mono_gchandle_get_target(w.parent),args);}
            native_try(method(mono_object_get_class(obj),"Dispose",0),obj,nullptr);native_release_asset(w);}
        for(auto h:{w.font,w.transform,w.handle,w.parent})if(h)mono_gchandle_free(h);w=Widget{};
    }
    using Box=overlay::native::Box;
    using Rgb=overlay::native::Rgb;
    static MonoObject* of(Widget& w) { return mono_gchandle_get_target(w.handle); }
    Widget& screen() { return native_root; }
    bool panel(Widget& w,Widget& parent,Box b,bool clip) {
        if(!native_append(w,panel_class,of(parent),b.x,b.y,b.w,b.h)) return false;
        float clear[]={0,0,0,0}; set(panel_class,of(w),"BackgroundColor",clear);
        int yes=1;
        if(clip&&!set(widget_class,of(w),"ClipChildren",&yes)) log("SHELLUI ClipChildren unavailable");
        return native_ok;
    }
    bool skin(Widget& w,Widget& parent,Box b,int kind,float radius) { return native_skin(w,of(parent),b,kind,radius); }
    bool image(Widget& w,Widget& parent,Box b,const std::string& uri) { return native_image_box(w,of(parent),b.x,b.y,b.w,b.h,uri); }
    bool label(Widget& w,Widget& parent,Box b,float size,int weight,Rgb colour,int align,int trim,const std::string& text) {
        return native_label(w,of(parent),b,size,weight,colour,align,trim,text);
    }
    void geometry(Widget& w,Box b) { native_geometry(w,b.x,b.y,b.w,b.h); }
    void show(Widget& w,bool on) { native_show(w,on); }
    void alpha(Widget& w,float a) { native_alpha(w,a); }
    void words(Widget& w,const std::string& text) { native_words(w,text); }
    void tint(Widget& w,Rgb c,float a) { native_tint(w,c.r,c.g,c.b,a); }
    void picture(Widget& w,const std::string& uri) { native_picture(w,uri); }
    void scale(Widget& w,float s) { native_scale(w,s); }
    float text_width(Widget& w,const std::string& text) { return native_measure(w,text); }
    std::string icon(const char* name) { return native_icon(name); }
    std::string fallback_art() { return native_fallback_art(); }
    bool focus(const overlay::native::Focus& f,int presses) { return native_focus(f,presses); }
};
#include "plugin_host.hpp"
static NativeBackend native_backend;
static kabuki::PluginHost<NativeBackend>* native_plugins=nullptr;
static overlay::native::Layout<NativeBackend>* native_layout=nullptr;

static bool bind_native(MonoImage* pui) {
    native_label_class=mono_class_from_name(pui,"Sce.PlayStation.PUI.UI2","Label");
    native_font_class=mono_class_from_name(pui,"Sce.PlayStation.PUI.UI2","UIFont");
    native_image_class=mono_class_from_name(pui,"Sce.PlayStation.PUI.UI2","ImageBox");
    native_asset_class=mono_class_from_name(pui,"Sce.PlayStation.PUI","ImageAsset");
    native_transform_class=mono_class_from_name(pui,"Sce.PlayStation.PUI","Transform");
    auto* loader=mono_class_from_name(pui,"Sce.PlayStation.PUI","CxmlLoader");
    if(!loader) return false;
    native_register_resources=native_exact(loader,"RegisterResourceFileFromSystemExVshAsset",{"System.String","System.String"});
    native_unregister_resources=native_exact(loader,"UnregisterResourceFile",{"System.String"});
    auto* a=mono_domain_assembly_open(domain,"/system_ex/common_ex/lib/Sce.PlayStation.Core.dll");
    native_texture_class=a?mono_class_from_name(mono_assembly_get_image(a),"Sce.PlayStation.Core.Graphics","Texture2D"):nullptr;
    if(!native_label_class||!native_font_class||!native_image_class||!native_asset_class||!native_texture_class) return false;
    native_font_ctor=native_exact(native_font_class,".ctor",{"System.Int32","Sce.PlayStation.Core.Imaging.FontStyle","Sce.PlayStation.Core.Imaging.FontWeight"});
    native_asset_uri_ctor=native_exact(native_asset_class,".ctor",{"System.String"});
    native_texture_ctor=native_exact(native_texture_class,".ctor",{"System.Int32","System.Int32","System.Boolean","Sce.PlayStation.Core.Graphics.PixelFormat"});
    native_asset_texture_ctor=native_exact(native_asset_class,".ctor",{"Sce.PlayStation.Core.Graphics.Texture"});
    native_pixels=native_exact(native_texture_class,"SetPixels",{"System.Int32","System.Array"});
    native_x=property(widget_class,"X",true); native_y=property(widget_class,"Y",true);
    native_width=property(widget_class,"Width",true); native_height=property(widget_class,"Height",true);
    native_opacity=property(widget_class,"Opacity",true); native_visible=property(widget_class,"Visible",true);
    native_text=property(native_label_class,"Text",true); native_font=property(native_label_class,"Font",true);
    native_color=property(native_label_class,"TextColor",true); native_image=property(native_image_class,"Image",true);
    native_filter=property(native_image_class,"FilterColor",true);
    // Optional: measuring text (the hint's Square) and the card's scale.
    native_text_width=native_exact(native_font_class,"GetTextWidth",{"System.String"});
    native_get_transform=native_exact(widget_class,"get_Transform",{});
    native_reset_transform=native_exact(widget_class,"ResetTransform",{"Sce.PlayStation.PUI.Transform"});
    native_notify=native_exact(widget_class,"NotifyTransformChanged",{});
    if(native_transform_class) {
        native_transform_ctor=native_exact(native_transform_class,".ctor",{});
        native_set_scale=native_exact(native_transform_class,"set_Scale",{"Sce.PlayStation.Core.Vector3"});
        native_pivot_centre=native_exact(native_transform_class,"get_PivotIsCenter",{});
    }
    const bool ok=native_font_ctor&&native_asset_uri_ctor&&native_texture_ctor&&native_asset_texture_ctor&&native_pixels&&
        native_x&&native_y&&native_width&&native_height&&native_opacity&&native_visible&&native_text&&native_font&&native_color&&native_image&&native_filter&&native_register_resources&&native_unregister_resources;
    bind_ui3(pui);
    bind_sounds(pui);
    if(auto* fm=mono_class_from_name(pui,"Sce.PlayStation.PUI","FocusManager")) {
        native_focus_manager=native_exact(fm,"get_Instance",{});
        native_focus_scene=native_exact(fm,"get_FocusActiveScene",{});
    }
    log("SHELLUI native bindings=%d measure=%d transform=%d scale=%d",ok,native_text_width!=nullptr,
        native_get_transform!=nullptr,native_set_scale!=nullptr);
    return ok;
}

static bool create_native(MonoObject*,float width,float height) {
    native_ok=true; native_unit=width/1920; native_scaling=true; native_view_seq=0;
    // A private in-process resource prefix; it does not change system settings or files.
    const auto prefix=mono_gchandle_new(reinterpret_cast<MonoObject*>(mono_string_new(domain,"custom_overlay")),1);
    const auto resource=mono_gchandle_new(reinterpret_cast<MonoObject*>(mono_string_new(domain,"Sce.PlayStation.PUI_UI3.rco")),1);
    if(!prefix||!resource) { if(prefix) mono_gchandle_free(prefix); if(resource) mono_gchandle_free(resource); return false; }
    void* resource_args[]={mono_gchandle_get_target(prefix),mono_gchandle_get_target(resource)};
    native_resources_registered=native_invoke(native_register_resources,nullptr,resource_args);
    mono_gchandle_free(prefix); mono_gchandle_free(resource);
    if(!native_resources_registered) return false;
    {
        const auto common=mono_gchandle_new(reinterpret_cast<MonoObject*>(mono_string_new(domain,"custom_common")),1);
        const auto file=mono_gchandle_new(reinterpret_cast<MonoObject*>(mono_string_new(domain,"ReactNative.Components.CommonAssets.rco")),1);
        if(common&&file) {
            void* args[]={mono_gchandle_get_target(common),mono_gchandle_get_target(file)};
            native_common_registered=native_try(native_register_resources,nullptr,args);
        }
        if(common) mono_gchandle_free(common);
        if(file) mono_gchandle_free(file);
        log("SHELLUI common assets registered=%d",native_common_registered);
    }
    native_root=NativeWidget{}; native_root.handle=visibility_handle;
    ui3_dispose(); ui3_read_mode();
    log("SHELLUI glass focus mode=%s",ui3_mode==1?"self":ui3_mode==2?"system":"off");
    native_layout=new(std::nothrow) overlay::native::Layout<NativeBackend>(native_backend);
    if(!native_layout||!native_layout->create()) return false;
    native_plugins=new(std::nothrow) kabuki::PluginHost<NativeBackend>(native_backend);
    if(!native_plugins)return false;
    native_plugins->action=[](const std::string& tile,const std::string& id,int value) {overlay_console::send_command(kabuki::kTileAction,value,(tile+"\t"+id+"\t").c_str());};
    native_plugins->timing=[](const std::string& tile,std::uint64_t us){static std::uint64_t last=0;const auto now=sceKernelGetProcessTime();if(now-last>5000000){last=now;log("PLUGIN frame tile=%s us=%llu",tile.c_str(),(unsigned long long)us);}};
    native_layout->plugin=[](NativeWidget& card,const overlay::NativeView& v,std::uint64_t now,float dt){return native_plugins->frame(card,v,now,dt);};
    log("SHELLUI created native control centre screen=%.0fx%.0f unit=%.3f",width,height,native_unit);
    native_sound_preload(); // loaded before the first sound is wanted
    return native_ok;
}
// The real control centre (or another system menu) taking over: ShellUI's focused scene changes to one
// that is neither ours nor the one focused when the panel opened. Then the panel closes. Every
// change is logged with the scene's class, to learn the control centre's (to close it from here).
static void native_watch_scenes(bool active,std::uint64_t now) {
    static std::uint64_t next=0,opened=0;
    static MonoObject* baseline=nullptr; static std::uint32_t baseline_pin=0,last_pin=0;
    static bool was=false;
    if(!native_focus_manager||!native_focus_scene) return;
    if(active&&!was) { opened=now; if(baseline_pin) { mono_gchandle_free(baseline_pin); baseline_pin=0; } baseline=nullptr; }
    was=active;
    if(!active||now<next) return;
    next=now+200000;
    MonoObject *manager=nullptr,*scene=nullptr;
    if(!native_try(native_focus_manager,nullptr,nullptr,&manager)||!manager) return;
    if(!native_try(native_focus_scene,manager,nullptr,&scene)) return;
    MonoObject* last=last_pin?mono_gchandle_get_target(last_pin):nullptr;
    if(scene!=last) {
        auto* type=scene?mono_object_get_class(scene):nullptr;
        log("SHELLUI focused scene %s.%s",type?mono_class_get_namespace(type):"",type?mono_class_get_name(type):"(none)");
        if(last_pin) mono_gchandle_free(last_pin);
        last_pin=scene?mono_gchandle_new(scene,0):0;
    }
    auto* ours=ui3_scene?mono_gchandle_get_target(ui3_scene):nullptr;
    auto* panel=scene_handle?mono_gchandle_get_target(scene_handle):nullptr;
    if(!baseline&&now-opened>=300000) { baseline=scene; if(scene) baseline_pin=mono_gchandle_new(scene,0); }
    else if(baseline&&scene&&scene!=ours&&scene!=panel&&scene!=mono_gchandle_get_target(baseline_pin)&&now-opened>=600000) {
        log("SHELLUI another scene took the focus: closing");
        release_input(SystemMenu);
    }
}
static void native_ui() {
    if(__atomic_load_n(&state.player_failed,__ATOMIC_ACQUIRE)) {
        release_input(SetupError); __atomic_store_n(&state.phase,Removing,__ATOMIC_RELEASE); return;
    }
    const auto now=sceKernelGetProcessTime();
    const bool active=__atomic_load_n(&state.input_active,__ATOMIC_ACQUIRE)&&!closing();
    // The newest state from the data thread; a busy lock keeps the last one for this frame.
    std::vector<int> sounds;
    if(native_view_seq!=__atomic_load_n(&native_snapshot_seq,__ATOMIC_RELAXED)&&pthread_mutex_trylock(&native_snapshot_lock)==0) {
        native_view=native_snapshot; native_view_seq=native_snapshot_seq;
        sounds.swap(native_sounds);
        pthread_mutex_unlock(&native_snapshot_lock);
    }
    // Opening and closing sound like the console's menus; then what the panel did.
    static bool was_active=false;
    if(active!=was_active) {
        was_active=active;
        if(overlay_console::sounds_on.load()) native_sound(active?overlay::sound::menu_open:overlay::sound::menu_close);
    }
    native_watch_scenes(active,now);
    if(active) for(int type:sounds) native_sound(type);
    if(native_layout) native_layout->frame(native_view,active,now,__atomic_load_n(&state.input_started,__ATOMIC_ACQUIRE));
    static std::uint64_t slow_logged=0;
    const auto took=sceKernelGetProcessTime()-now;
    if(took>100000&&now-slow_logged>2000000) {
        slow_logged=now;
        log("SHELLUI slow frame ms=%llu tab=%d focused=%d popup=%d",(unsigned long long)(took/1000),native_view.tab,
            native_view.focused,native_view.popup);
    }
    if(active) __atomic_fetch_add(&state.surface_frames,1,__ATOMIC_RELAXED);
    if(!native_ok) {
        __atomic_store_n(&state.player_failed,1,__ATOMIC_RELEASE);
        release_input(SetupError); __atomic_store_n(&state.phase,Removing,__ATOMIC_RELEASE); failed=true;
    }
}
static void dispose_native() {
    delete native_plugins;native_plugins=nullptr;
    // Scene.Dispose owns its widget tree; free our pins only after scene detachment.
    if(native_layout) {
        native_layout->each([](NativeWidget& w) {
            native_release_asset(w);
            for(auto h:{w.font,w.transform,w.handle,w.parent}) if(h) mono_gchandle_free(h);
            w=NativeWidget{};
        });
        delete native_layout; native_layout=nullptr;
    }
    native_root=NativeWidget{}; // visibility_handle belongs to the renderer
    native_sound_release();
    ui3_dispose();
    if(native_resources_registered) {
        auto* prefix=mono_string_new(domain,"custom_overlay");
        const auto pin=prefix?mono_gchandle_new(reinterpret_cast<MonoObject*>(prefix),1):0;
        if(pin) {
            void* args[]={mono_gchandle_get_target(pin)};
            native_invoke(native_unregister_resources,nullptr,args); mono_gchandle_free(pin);
        }
        native_resources_registered=false;
    }
    if(native_common_registered) {
        auto* prefix=mono_string_new(domain,"custom_common");
        const auto pin=prefix?mono_gchandle_new(reinterpret_cast<MonoObject*>(prefix),1):0;
        if(pin) {
            void* args[]={mono_gchandle_get_target(pin)};
            native_try(native_unregister_resources,nullptr,args); mono_gchandle_free(pin);
        }
        native_common_registered=false;
    }
}
