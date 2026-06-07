#include <jni.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string.h>
#include <android/log.h>

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#define LOG_TAG "INEBWithToggle"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

JNIEnv* env = nullptr;

static bool g_Initialized = false;
static int g_Width = 0, g_Height = 0;
static EGLContext g_TargetContext = EGL_NO_CONTEXT;
static EGLSurface g_TargetSurface = EGL_NO_SURFACE;

// ==============================
// INEB Configuration
// ==============================
struct Config {
    bool enabled;
    bool showChests;
    bool showEntities;
    bool showSigns;
    bool showBeds;
    float opacity;
    bool debugMode;
};

static Config g_Config = {
    true,    // enabled
    true,    // showChests
    true,    // showEntities
    true,    // showSigns
    true,    // showBeds
    0.0f,    // opacity
    false    // debugMode
};

// ==============================
// Original function pointers
// ==============================
static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
static void* (*orig_AAssetManager_open)(void*, const char*, int) = nullptr;

// ==============================
// OpenGL state
// ==============================
struct GLState {
    GLint prog, tex, aTex, aBuf, eBuf, vao, fbo, vp[4], sc[4], bSrc, bDst;
    GLboolean blend, cull, depth, scissor;
};

static void SaveGL(GLState& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.aTex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.aBuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.eBuf);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_VIEWPORT, s.vp);
    glGetIntegerv(GL_SCISSOR_BOX, s.sc);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.bSrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s.bDst);
    s.blend = glIsEnabled(GL_BLEND);
    s.cull = glIsEnabled(GL_CULL_FACE);
    s.depth = glIsEnabled(GL_DEPTH_TEST);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
}

static void RestoreGL(const GLState& s) {
    glUseProgram(s.prog);
    glActiveTexture(s.aTex);
    glBindTexture(GL_TEXTURE_2D, s.tex);
    glBindBuffer(GL_ARRAY_BUFFER, s.aBuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.eBuf);
    glBindVertexArray(s.vao);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
    glScissor(s.sc[0], s.sc[1], s.sc[2], s.sc[3]);
    glBlendFunc(s.bSrc, s.bDst);
    s.blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    s.cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
    s.depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    s.scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
}

// ==============================
// Draw ImGui Menu
// ==============================
static void DrawMenu() {
    ImGuiIO& io = ImGui::GetIO();
    
    ImGui::SetNextWindowSize(ImVec2(350, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("INEB Control Center", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 10));
    if (ImGui::Checkbox("##mainToggle", &g_Config.enabled));
    ImGui::PopStyleVar();
    ImGui::SameLine();
    ImGui::TextColored(g_Config.enabled ? ImVec4(0, 1, 0, 1) : ImVec4(1, 0, 0, 1),
                       g_Config.enabled ? " INEB Enabled" : " INEB Disabled");
    
    ImGui::Separator();
    
    if (g_Config.enabled) {
        ImGui::Text("Visibility Options:");
        ImGui::Indent();
        ImGui::Checkbox("Show Chests", &g_Config.showChests);
        ImGui::Checkbox("Show Entities", &g_Config.showEntities);
        ImGui::Checkbox("Show Signs", &g_Config.showSigns);
        ImGui::Checkbox("Show Beds", &g_Config.showBeds);
        ImGui::Unindent();
        
        ImGui::Separator();
        ImGui::Text("Opacity:");
        ImGui::SliderFloat("##opacitySlider", &g_Config.opacity, 0.0f, 1.0f, "%.2f");
        
        ImGui::Separator();
        ImGui::Checkbox("Debug Mode", &g_Config.debugMode);
        
        if (g_Config.debugMode) {
            ImGui::Indent();
            ImGui::Text("FPS: %.1f", io.Framerate);
            ImGui::Text("Resolution: %dx%d", g_Width, g_Height);
            ImGui::Unindent();
        }
    } else {
        ImGui::TextDisabled("Enable INEB to adjust settings");
    }
    
    ImGui::Separator();
    ImGui::TextWrapped("Note: This is a UI prototype. Full asset blocking requires integrating with Minecraft's rendering system.");
    
    ImGui::End();
}

static void Setup() {
    if (g_Initialized || g_Width <= 0 || g_Height <= 0) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    float scale = (float)g_Height / 720.0f;
    if (scale < 1.5f) scale = 1.5f;
    if (scale > 4.0f) scale = 4.0f;
    ImFontConfig cfg;
    cfg.SizePixels = 32.0f * scale;
    io.Fonts->AddFontDefault(&cfg);
    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");
    ImGui::GetStyle().ScaleAllSizes(scale);
    g_Initialized = true;
    LOGI("ImGui initialized: %dx%d", g_Width, g_Height);
}

static void Render() {
    if (!g_Initialized) return;
    GLState s;
    SaveGL(s);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_Width, (float)g_Height);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_Width, g_Height);
    ImGui::NewFrame();
    DrawMenu();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    RestoreGL(s);
}

static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglSwapBuffers) return EGL_FALSE;
    
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglSwapBuffers(dpy, surf);
    
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    
    if (w < 500 || h < 500) return orig_eglSwapBuffers(dpy, surf);
    
    if (g_TargetContext == EGL_NO_CONTEXT) {
        EGLint buf = 0;
        eglQuerySurface(dpy, surf, EGL_RENDER_BUFFER, &buf);
        if (buf == EGL_BACK_BUFFER) {
            g_TargetContext = ctx;
            g_TargetSurface = surf;
        }
    }
    
    if (ctx != g_TargetContext || surf != g_TargetSurface)
        return orig_eglSwapBuffers(dpy, surf);
    
    g_Width = w;
    g_Height = h;
    Setup();
    Render();
    return orig_eglSwapBuffers(dpy, surf);
}

// ==============================
// Simple hooking (without preloader)
// ==============================
static void SimpleHook() {
    void* egl_handle = dlopen("libEGL.so", RTLD_LAZY | RTLD_NOLOAD);
    if (egl_handle) {
        orig_eglSwapBuffers = reinterpret_cast<EGLBoolean (*)(EGLDisplay, EGLSurface)>(
            dlsym(egl_handle, "eglSwapBuffers"));
        LOGI("Found eglSwapBuffers: %p", (void*)orig_eglSwapBuffers);
    }
    
    // Try to hook AAssetManager_open too (for actual INEB functionality)
    void* android_handle = dlopen("libandroid.so", RTLD_LAZY | RTLD_NOLOAD);
    if (android_handle) {
        orig_AAssetManager_open = reinterpret_cast<void* (*)(void*, const char*, int)>(
            dlsym(android_handle, "AAssetManager_open"));
        LOGI("Found AAssetManager_open: %p", (void*)orig_AAssetManager_open);
    }
    
    LOGI("Hooking setup complete!");
}

static void* MainThread(void*) {
    sleep(3);
    SimpleHook();
    LOGI("INEBWithToggle mod loaded successfully!");
    return nullptr;
}

extern "C" {
    __attribute__((visibility("default")))
    void LeviMod_Load(JavaVM* vm) {
        if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_4) != JNI_OK) {
            LOGE("Failed to get JNIEnv");
            return;
        }
        
        pthread_t t;
        pthread_create(&t, nullptr, MainThread, nullptr);
        LOGI("INEBWithToggle mod initialized!");
    }
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_4) != JNI_OK)
        return JNI_VERSION_1_4;
    
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
    return JNI_VERSION_1_4;
}
