/* menu.cpp -- see menu.h.
 *
 * Dear ImGui (third_party/imgui, MIT) for the widgets, and a small renderer
 * for the context the game actually has: OpenGL ES-CM 1.1, fixed function.
 * ImGui's own GL2 backend is the closest match but uses desktop-only calls
 * (glPushAttrib, glPolygonMode, GL_UNPACK_ROW_LENGTH), so the renderer below
 * saves and restores what the game could have set, one call at a time. The
 * game keeps drawing every frame and never learns the menu was there. */
#include <cstdio>
#include <cstring>
#include <ctime>

#include <GLES/gl.h>
#include <GLES/glext.h>
#include <switch.h>

#include "imgui.h"

extern "C" {
#include "menu.h"
#include "settings.h"
}

namespace {

int menu_current_tab();

/* ---- open / close ----------------------------------------------------- */

/* Seconds to hold "-". Two, not three: holding "-" for four or five seconds
 * makes the system show its own notification popup, and a player waiting for
 * the menu easily holds that long. */
const uint64_t kHoldTicksNum = 2;

bool     g_open;
bool     g_swallow;                    /* block the game until buttons lift */
uint64_t g_minus_since;                /* tick "-" went down, 0 if up */
bool     g_minus_fired;                /* this hold already toggled */
uint64_t g_prev_held;

/* Pad state captured in menu_input, turned into ImGui events at render. */
uint64_t g_held;
int      g_lx, g_ly;
bool     g_close_request;
int      g_tab_step;                   /* -1 / +1 from L / R this frame */

bool     g_ctx_ready;
int      g_font_px_built;
bool     g_dirty;                      /* a setting changed while open */

const char *g_help;                    /* description of the focused item */
bool g_focus_first;                    /* focus the tab's first setting */
int  g_focus_tab = -1;                 /* ...once this tab is really drawn */
int  g_tab;                            /* the selected tab */
int menu_current_tab() { return g_tab; }

/* ---- setting helpers ---------------------------------------------------- */

void set_live(const char *key, int value) {
    port_setting_set(key, value);
    settings_set_int(key, value);
    g_dirty = true;
}

/* With a controller, focus starts on a tab's first setting whenever the menu
 * opens or L/R changes tab, rather than wherever it last was. */
void focus_here_if_first() {
    if (g_focus_first) {
        ImGui::SetKeyboardFocusHere();
        ImGui::SetNavCursorVisible(true);
        g_focus_first = false;
        g_focus_tab = -1;
    }
}

/* A hovered or keyboard/gamepad-focused item publishes its help text. */
void help(const char *text) {
    if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
        g_help = text;
}

bool valid_name(const char *v) {
    size_t n = strlen(v);
    if (!n || n > 13)
        return false;
    for (size_t i = 0; i < n; i++) {
        const char c = v[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_' ||
              c == '.'))
            return false;
    }
    return true;
}

/* The system keyboard. Blocks until the player confirms or cancels, which is
 * fine: the game is not drawing while it is up. */
bool keyboard(const char *header, const char *initial, char *out, size_t cap,
              int max_len) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0)))
        return false;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header);
    swkbdConfigSetInitialText(&kbd, initial);
    swkbdConfigSetStringLenMax(&kbd, max_len);
    Result rc = swkbdShow(&kbd, out, cap);
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc) && out[0];
}

/* ---- the GLES 1.1 renderer ---------------------------------------------- */

void update_texture(ImTextureData *tex) {
    if (tex->Status == ImTextureStatus_WantCreate ||
        tex->Status == ImTextureStatus_WantUpdates) {
        GLint last = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &last);
        GLuint id = (GLuint)(intptr_t)tex->TexID;
        if (tex->Status == ImTextureStatus_WantCreate) {
            glGenTextures(1, &id);
            glBindTexture(GL_TEXTURE_2D, id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else {
            glBindTexture(GL_TEXTURE_2D, id);
        }
        /* GLES1 has no GL_UNPACK_ROW_LENGTH, so partial updates cannot address
         * a sub-rectangle of the atlas in place. The atlas is small and only
         * changes when new glyphs are first drawn: upload it whole. */
        GLint last_align = 4;
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &last_align);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex->Width, tex->Height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, tex->GetPixels());
        glPixelStorei(GL_UNPACK_ALIGNMENT, last_align);
        glBindTexture(GL_TEXTURE_2D, (GLuint)last);
        tex->SetTexID((ImTextureID)(intptr_t)id);
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantDestroy) {
        GLuint id = (GLuint)(intptr_t)tex->TexID;
        glDeleteTextures(1, &id);
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
}

struct Saved {
    GLint tex_binding, active_tex, client_active_tex, array_buf, element_buf;
    GLint viewport[4], scissor[4], blend_src, blend_dst, shade, tex_env;
    GLboolean color_mask[4], depth_mask;
    GLboolean blend, cull, depth, stencil, scissor_test, lighting, alpha,
              fog, color_material, texture2d;
    GLboolean vertex_array, color_array, texcoord_array, normal_array;
    GLboolean other_tex2d[3];
    int other_units;
};

void save_state(Saved &s) {
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.active_tex);
    glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &s.client_active_tex);
    GLint units = 1;
    glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
    s.other_units = units > 4 ? 3 : (units > 1 ? units - 1 : 0);
    for (int u = 0; u < s.other_units; u++) {
        glActiveTexture(GL_TEXTURE1 + u);
        s.other_tex2d[u] = glIsEnabled(GL_TEXTURE_2D);
        glDisable(GL_TEXTURE_2D);
    }
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex_binding);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.array_buf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.element_buf);
    glGetIntegerv(GL_VIEWPORT, s.viewport);
    glGetIntegerv(GL_SCISSOR_BOX, s.scissor);
    glGetIntegerv(GL_BLEND_SRC, &s.blend_src);
    glGetIntegerv(GL_BLEND_DST, &s.blend_dst);
    glGetIntegerv(GL_SHADE_MODEL, &s.shade);
    glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &s.tex_env);
    glGetBooleanv(GL_COLOR_WRITEMASK, s.color_mask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &s.depth_mask);
    s.blend = glIsEnabled(GL_BLEND);
    s.cull = glIsEnabled(GL_CULL_FACE);
    s.depth = glIsEnabled(GL_DEPTH_TEST);
    s.stencil = glIsEnabled(GL_STENCIL_TEST);
    s.scissor_test = glIsEnabled(GL_SCISSOR_TEST);
    s.lighting = glIsEnabled(GL_LIGHTING);
    s.alpha = glIsEnabled(GL_ALPHA_TEST);
    s.fog = glIsEnabled(GL_FOG);
    s.color_material = glIsEnabled(GL_COLOR_MATERIAL);
    s.texture2d = glIsEnabled(GL_TEXTURE_2D);
    s.vertex_array = glIsEnabled(GL_VERTEX_ARRAY);
    s.color_array = glIsEnabled(GL_COLOR_ARRAY);
    s.texcoord_array = glIsEnabled(GL_TEXTURE_COORD_ARRAY);
    s.normal_array = glIsEnabled(GL_NORMAL_ARRAY);
}

void set_enabled(GLenum cap, GLboolean on) {
    if (on)
        glEnable(cap);
    else
        glDisable(cap);
}

void set_client(GLenum array, GLboolean on) {
    if (on)
        glEnableClientState(array);
    else
        glDisableClientState(array);
}

void restore_state(const Saved &s) {
    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glBindTexture(GL_TEXTURE_2D, (GLuint)s.tex_binding);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)s.array_buf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)s.element_buf);
    glViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    glScissor(s.scissor[0], s.scissor[1], s.scissor[2], s.scissor[3]);
    glBlendFunc((GLenum)s.blend_src, (GLenum)s.blend_dst);
    glShadeModel((GLenum)s.shade);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, s.tex_env);
    glColorMask(s.color_mask[0], s.color_mask[1], s.color_mask[2], s.color_mask[3]);
    glDepthMask(s.depth_mask);
    set_enabled(GL_BLEND, s.blend);
    set_enabled(GL_CULL_FACE, s.cull);
    set_enabled(GL_DEPTH_TEST, s.depth);
    set_enabled(GL_STENCIL_TEST, s.stencil);
    set_enabled(GL_SCISSOR_TEST, s.scissor_test);
    set_enabled(GL_LIGHTING, s.lighting);
    set_enabled(GL_ALPHA_TEST, s.alpha);
    set_enabled(GL_FOG, s.fog);
    set_enabled(GL_COLOR_MATERIAL, s.color_material);
    set_enabled(GL_TEXTURE_2D, s.texture2d);
    set_client(GL_VERTEX_ARRAY, s.vertex_array);
    set_client(GL_COLOR_ARRAY, s.color_array);
    set_client(GL_TEXTURE_COORD_ARRAY, s.texcoord_array);
    set_client(GL_NORMAL_ARRAY, s.normal_array);
    for (int u = 0; u < s.other_units; u++) {
        glActiveTexture(GL_TEXTURE1 + u);
        set_enabled(GL_TEXTURE_2D, s.other_tex2d[u]);
    }
    glActiveTexture((GLenum)s.active_tex);
    glClientActiveTexture((GLenum)s.client_active_tex);
}

void render_draw_data(ImDrawData *dd, int fb_w, int fb_h) {
    if (dd->Textures)
        for (ImTextureData *tex : *dd->Textures)
            if (tex->Status != ImTextureStatus_OK)
                update_texture(tex);

    Saved s;
    save_state(s);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_FOG);
    glDisable(GL_COLOR_MATERIAL);
    glEnable(GL_SCISSOR_TEST);
    glEnable(GL_TEXTURE_2D);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glShadeModel(GL_SMOOTH);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glViewport(0, 0, fb_w, fb_h);
    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrthof(dd->DisplayPos.x, dd->DisplayPos.x + dd->DisplaySize.x,
             dd->DisplayPos.y + dd->DisplaySize.y, dd->DisplayPos.y, -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    const ImVec2 clip_off = dd->DisplayPos;
    for (const ImDrawList *list : dd->CmdLists) {
        const ImDrawVert *vtx = list->VtxBuffer.Data;
        const ImDrawIdx *idx = list->IdxBuffer.Data;
        glVertexPointer(2, GL_FLOAT, sizeof(ImDrawVert),
                        (const GLvoid *)((const char *)vtx + offsetof(ImDrawVert, pos)));
        glTexCoordPointer(2, GL_FLOAT, sizeof(ImDrawVert),
                          (const GLvoid *)((const char *)vtx + offsetof(ImDrawVert, uv)));
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(ImDrawVert),
                       (const GLvoid *)((const char *)vtx + offsetof(ImDrawVert, col)));
        for (int i = 0; i < list->CmdBuffer.Size; i++) {
            const ImDrawCmd *cmd = &list->CmdBuffer[i];
            if (cmd->UserCallback) {
                if (cmd->UserCallback != ImDrawCallback_ResetRenderState)
                    cmd->UserCallback(list, cmd);
                continue;
            }
            const float x0 = cmd->ClipRect.x - clip_off.x, y0 = cmd->ClipRect.y - clip_off.y;
            const float x1 = cmd->ClipRect.z - clip_off.x, y1 = cmd->ClipRect.w - clip_off.y;
            if (x1 <= x0 || y1 <= y0)
                continue;
            glScissor((int)x0, (int)((float)fb_h - y1), (int)(x1 - x0), (int)(y1 - y0));
            glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)cmd->GetTexID());
            glDrawElements(GL_TRIANGLES, (GLsizei)cmd->ElemCount,
                           sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                           idx + cmd->IdxOffset);
        }
    }
    restore_state(s);
}

/* ---- context, font and style ---------------------------------------------- */

void apply_style(float scale) {
    ImGuiStyle &st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark(&st);
    /* Fizeau's look: square corners, a translucent dark panel over the game,
     * blue tabs and headers. */
    st.WindowRounding = 0.0f;
    st.FrameRounding = 0.0f;
    st.TabRounding = 0.0f;
    st.GrabRounding = 0.0f;
    st.WindowBorderSize = 1.0f;
    st.FramePadding = ImVec2(8.0f, 5.0f);
    st.ItemSpacing = ImVec2(10.0f, 10.0f);
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.10f, 0.92f);
    st.Colors[ImGuiCol_TitleBg] = ImVec4(0.16f, 0.29f, 0.48f, 1.00f);
    st.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.29f, 0.48f, 1.00f);
    st.ScaleAllSizes(scale);
    st.FontSizeBase = 26.0f * scale;
}

bool ensure_context(int surface_h) {
    if (!g_ctx_ready) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad |
                           ImGuiBackendFlags_RendererHasTextures;
        io.BackendRendererName = "codboz_gles1";
        io.BackendPlatformName = "codboz_switch";

        /* The console's own UI font, so the menu looks native and needs no
         * font file on the card. ImGui's built-in font is the fallback. */
        PlFontData font;
        bool have_font = false;
        if (R_SUCCEEDED(plInitialize(PlServiceType_User)) &&
            R_SUCCEEDED(plGetSharedFontByType(&font, PlSharedFontType_Standard))) {
            ImFontConfig cfg;
            cfg.FontDataOwnedByAtlas = false;
            have_font = io.Fonts->AddFontFromMemoryTTF(font.address, (int)font.size,
                                                       26.0f, &cfg) != nullptr;
        }
        if (!have_font)
            io.Fonts->AddFontDefault();
        g_ctx_ready = true;
        printf("  [menu ] ImGui %s ready (%s font)\n", ImGui::GetVersion(),
               have_font ? "system" : "built-in");
    }
    if (g_font_px_built != surface_h) {
        apply_style((float)surface_h / 720.0f);
        g_font_px_built = surface_h;
    }
    return true;
}

/* ---- the menu ------------------------------------------------------------ */

struct ServerPreset {
    const char *label;
    const char *host;
};

const ServerPreset kServers[] = {
    {"Community (PS Vita and PortMaster)", "boz-online.xubi.org"},
    {"boz-nx-online.aaaoz.fr", "boz-nx-online.aaaoz.fr"},
    {"Off (no Play Online)", ""},
};
const int kServerCount = (int)(sizeof kServers / sizeof kServers[0]);

void tab_controls() {
    int layout = port_setting_get("control_layout");
    const char *layouts[] = {"Touch (on-screen sticks)", "Console (analog sticks)"};
    ImGui::TextUnformatted("Controller layout");
    ImGui::SetNextItemWidth(-1.0f);
    focus_here_if_first();
    if (ImGui::Combo("##layout", &layout, layouts, 2)) {
        set_live("control_layout", layout);
        set_live("hide_sticks", layout);        /* sticks are drawn for touch */
    }
    help("Console: the analog sticks move and aim, as on a console. Touch: the "
         "game's own on-screen sticks, driven by touch.");

    bool aim_hold = port_setting_get("aim_hold") != 0;
    if (ImGui::Checkbox("Hold ZL to aim", &aim_hold))
        set_live("aim_hold", aim_hold);
    help("On: aim while ZL is held. Off: the game's own behaviour, where each "
         "press of ZL toggles aiming.");

    bool run_toggle = port_setting_get("run_toggle") != 0;
    if (ImGui::Checkbox("Click L3 to run", &run_toggle))
        set_live("run_toggle", run_toggle);
    help("On: click the left stick once to sprint; stopping ends it. Off: hold "
         "the left stick to sprint.");

    int yhold = port_setting_get("y_hold_frames");
    ImGui::TextUnformatted("Y hold to buy/repair on the move");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderInt("##yhold", &yhold, 4, 40))
        set_live("y_hold_frames", yhold);
    help("A tap of Y while moving reloads. Holding it this long also buys, "
         "repairs or opens. 12 frames is about 0.2 seconds.");

    int aim_stick = port_setting_get("aim_stick");
    const char *aims[] = {"Swipe (drag and re-anchor)", "Held stick"};
    ImGui::TextUnformatted("Right stick aiming");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##aimstick", &aim_stick, aims, 2))
        set_live("aim_stick", aim_stick);
    help("How the right stick turns the camera when the touch layout is used.");

    int speed = port_setting_get("aim_speed");
    ImGui::TextUnformatted("Aim speed");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderInt("##aimspeed", &speed, 20, 300))
        set_live("aim_speed", speed);
    help("How fast the camera turns at full right-stick deflection.");
}

void tab_online() {
    const char *current = settings_get("multiplayer_server", "boz-online.xubi.org");
    int preset = -1;
    for (int i = 0; i < kServerCount; i++)
        if (!strcmp(current, kServers[i].host))
            preset = i;

    ImGui::TextUnformatted("Play Online server");
    ImGui::SetNextItemWidth(-1.0f);
    const char *preview = preset >= 0 ? kServers[preset].label : current;
    focus_here_if_first();
    if (ImGui::BeginCombo("##server", preview)) {
        for (int i = 0; i < kServerCount; i++) {
            if (ImGui::Selectable(kServers[i].label, i == preset)) {
                settings_set("multiplayer_server", kServers[i].host);
                g_dirty = true;
            }
        }
        if (ImGui::Selectable("Custom address...", preset < 0)) {
            char buf[128] = {0};
            if (keyboard("Server address", current, buf, sizeof buf, 100)) {
                settings_set("multiplayer_server", buf);
                g_dirty = true;
            }
        }
        ImGui::EndCombo();
    }
    help("Every player in a match must use the same server. The community "
         "server is the one PS Vita and PortMaster players use.");
    ImGui::TextDisabled("Current: %s", current[0] ? current : "(off)");

    const char *name = settings_get("player_name", "Player");
    char label[64];
    snprintf(label, sizeof label, "Player name: %s", name);
    if (ImGui::Button(label, ImVec2(-1.0f, 0.0f))) {
        char buf[32] = {0};
        if (keyboard("Player name (letters, numbers, - _ . and spaces)", name, buf,
                     sizeof buf, 13) && valid_name(buf)) {
            settings_set("player_name", buf);
            g_dirty = true;
        }
    }
    help("Shown to other players. Up to 13 letters, numbers, spaces, - _ or .");

    ImGui::Spacing();
    ImGui::TextWrapped("Server and name changes apply the next time the game "
                       "starts.");
}

void tab_display() {
    bool fps = port_setting_get("show_fps") != 0;
    focus_here_if_first();
    if (ImGui::Checkbox("Show FPS counter", &fps))
        set_live("show_fps", fps);
    help("The frame rate in the top-left corner.");

    bool hide = port_setting_get("hide_sticks") != 0;
    if (ImGui::Checkbox("Hide on-screen sticks", &hide))
        set_live("hide_sticks", hide);
    help("Hides the game's virtual sticks. Keep them shown with the touch "
         "layout.");

    ImGui::Separator();
    bool music = port_setting_get("music") != 0;
    if (ImGui::Checkbox("Music", &music))
        set_live("music", music);
    help("Menu and game-over music. Sound effects are not affected.");
}

void tab_help() {
    if (g_focus_first) {                /* nothing to focus on this tab */
        g_focus_first = false;
        g_focus_tab = -1;
    }
    ImGui::TextWrapped("Hold - for 2 seconds to open or close this menu.");
    ImGui::BulletText("A: select    B: back / close");
    ImGui::BulletText("L / R: previous / next tab");
    ImGui::BulletText("D-pad or left stick: move");
    ImGui::BulletText("- and + together for 1 s: quit the game");
    ImGui::Spacing();
    ImGui::TextWrapped("Settings are saved to sdmc:/switch/boz/config.txt when "
                       "the menu closes.");
    ImGui::Spacing();
    ImGui::TextDisabled("%s", port_build_label());
    help("");
}

/* ---- the Advanced tab ----------------------------------------------------
 *
 * Development switches: the translation cache, the profilers, benchmark
 * mode. Keys in the same config.txt as everything else here.
 *
 * All of them are read once, at launch, so this tab edits the NEXT run. It
 * says so, and it shows what the run in progress actually settled on, which
 * is not always what was asked for: dynarmic can decline to start, and safe
 * mode ignores the lot. */

/* A launch-time setting: written to the file, never applied live. */
void set_boot(const char *key, int value) {
    settings_set_int(key, value);
    g_dirty = true;
}

bool boot_check(const char *label, const char *key, bool def, const char *text) {
    bool v = settings_get_int(key, def ? 1 : 0) != 0;
    if (ImGui::Checkbox(label, &v))
        set_boot(key, v);
    help(text);
    return v;
}

void tab_advanced() {
    if (port_safe_mode()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.2f, 1.0f));
        ImGui::TextWrapped("Safe mode: the last launch did not finish "
                           "starting, so everything on this tab was ignored "
                           "for this run. It applies again next launch.");
        ImGui::PopStyleColor();
    }
    ImGui::TextDisabled("Running now: %s", port_runtime_label());
    ImGui::Separator();

    int mb = settings_get_int("dynarmic_cache_mb", 32);
    ImGui::TextUnformatted("Translation cache");
    ImGui::SetNextItemWidth(-1.0f);
    focus_here_if_first();
    if (ImGui::SliderInt("##dyncache", &mb, 4, 256, "%d MB"))
        set_boot("dynarmic_cache_mb", mb);
    help("Memory held for the game's translated code. Larger is not faster "
         "once the game fits; this memory comes out of what the game itself "
         "uses.");

    ImGui::Separator();
    ImGui::TextUnformatted("Diagnostics");
    help("");
    boot_check("Profilers", "profilers", false,
               "Measure where time goes, reported every 300 frames on the log. "
               "Costs speed in every frame, so leave it off to play.");
    boot_check("Fixed 16 ms clock", "fixed_clock", false,
               "Give the game the same time step every frame, so a run is "
               "reproducible. Game speed then follows frame rate.");
    boot_check("Touch markers", "touch_debug", false,
               "Draw where the screen is being touched, and log it.");

    const bool bench = boot_check("Benchmark mode", "bench", false,
                                  "Run a fixed number of instructions and "
                                  "stop, ignoring the controller, so two "
                                  "builds can be compared. Not for playing.");
    if (bench) {
        int m = settings_get_int("bench_million", 0);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderInt("##benchm", &m, 0, 4000,
                             m > 0 ? "%d million instructions" : "built-in default"))
            set_boot("bench_million", m);
        help("How far the benchmark runs before it stops.");
    }

    ImGui::Spacing();
    ImGui::TextWrapped("Everything on this tab applies the next time the game "
                       "starts.");
}

void draw_menu(float w, float h) {
    const float s = h / 720.0f;
    g_help = nullptr;

    ImGui::SetNextWindowPos(ImVec2(40.0f * s, 36.0f * s), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w * 0.43f, h - 72.0f * s), ImGuiCond_Always);
    ImGui::Begin("CoD BOZ NX", nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

    std::time_t now = std::time(nullptr);
    std::tm local = {};
    localtime_r(&now, &local);
    ImGui::Text("Time: %02d:%02d:%02d - Fps: %.2f", local.tm_hour, local.tm_min,
                local.tm_sec, ImGui::GetIO().Framerate);

    int &tab = g_tab;
    static const char *names[] = {"Controls", "Online", "Display", "Advanced",
                                  "Help"};
    const int count = 5;
    int forced = -1;
    if (g_tab_step) {
        tab = (tab + g_tab_step + count) % count;
        forced = tab;
        g_tab_step = 0;
        g_focus_tab = tab;
    }
    if (ImGui::BeginTabBar("tabs")) {
        for (int i = 0; i < count; i++) {
            ImGuiTabItemFlags flags = i == forced ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(names[i], nullptr, flags)) {
                if (forced < 0)
                    tab = i;
                /* A tab switch lands a frame late: only focus once the tab that
                 * was asked for is the one being drawn. */
                g_focus_first = g_focus_tab == i;
                switch (i) {
                case 0: tab_controls(); break;
                case 1: tab_online(); break;
                case 2: tab_display(); break;
                case 3: tab_advanced(); break;
                default: tab_help(); break;
                }
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::End();

    /* The right-hand panel, where Fizeau shows its preview: what the focused
     * setting does. */
    ImGui::SetNextWindowPos(ImVec2(w * 0.43f + 60.0f * s, 36.0f * s), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w - (w * 0.43f + 100.0f * s), 190.0f * s),
                             ImGuiCond_Always);
    ImGui::Begin("About this setting", nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::TextWrapped("%s", g_help && g_help[0] ? g_help
                                                 : "Move to a setting to see what it does.");
    ImGui::End();
}

void feed_input(ImGuiIO &io, int surface_w, int surface_h) {
    struct Map {
        uint64_t button;
        ImGuiKey key;
    };
    static const Map map[] = {
        {HidNpadButton_A, ImGuiKey_GamepadFaceDown},
        {HidNpadButton_B, ImGuiKey_GamepadFaceRight},
        {HidNpadButton_X, ImGuiKey_GamepadFaceUp},
        {HidNpadButton_Y, ImGuiKey_GamepadFaceLeft},
        {HidNpadButton_Up, ImGuiKey_GamepadDpadUp},
        {HidNpadButton_Down, ImGuiKey_GamepadDpadDown},
        {HidNpadButton_Left, ImGuiKey_GamepadDpadLeft},
        {HidNpadButton_Right, ImGuiKey_GamepadDpadRight},
    };
    for (const Map &m : map)
        io.AddKeyEvent(m.key, (g_held & m.button) != 0);
    const float dead = 8000.0f;
    auto axis = [&](ImGuiKey key, float v) {
        const float a = v > dead ? (v - dead) / (32767.0f - dead) : 0.0f;
        io.AddKeyAnalogEvent(key, a > 0.0f, a > 1.0f ? 1.0f : a);
    };
    axis(ImGuiKey_GamepadLStickRight, (float)g_lx);
    axis(ImGuiKey_GamepadLStickLeft, (float)-g_lx);
    axis(ImGuiKey_GamepadLStickUp, (float)g_ly);
    axis(ImGuiKey_GamepadLStickDown, (float)-g_ly);

    /* Touch as a mouse, scaled from the 1280x720 panel to the surface. */
    HidTouchScreenState ts;
    static bool was_down;
    if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent((float)ts.touches[0].x * (float)surface_w / 1280.0f,
                            (float)ts.touches[0].y * (float)surface_h / 720.0f);
        if (!was_down)
            io.AddMouseButtonEvent(0, true);
        was_down = true;
    } else if (was_down) {
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMouseButtonEvent(0, false);
        was_down = false;
    }
}

void close_menu() {
    g_open = false;
    g_swallow = true;
    if (g_dirty) {
        const bool ok = settings_save() != 0;
        printf("  [menu ] settings %s\n", ok ? "saved to config.txt" : "NOT saved");
        g_dirty = false;
    }
    printf("  [menu ] closed\n");
}

} // namespace

extern "C" {

void menu_input(uint64_t held, int lx, int ly) {
    const uint64_t now = armGetSystemTick();
    const uint64_t pressed = held & ~g_prev_held;
    const bool minus_alone = (held & HidNpadButton_Minus) && !(held & HidNpadButton_Plus);

    if (minus_alone) {
        if (!g_minus_since)
            g_minus_since = now;
        if (!g_minus_fired &&
            now - g_minus_since >= kHoldTicksNum * armGetSystemTickFreq()) {
            g_minus_fired = true;
            if (g_open) {
                close_menu();
            } else {
                g_open = true;
                g_swallow = true;
                g_focus_tab = menu_current_tab();
                settings_load();       /* pick up hand edits to the file */
                printf("  [menu ] opened\n");
            }
        }
    } else {
        g_minus_since = 0;
        g_minus_fired = false;
    }

    if (g_open) {
        g_held = held;
        g_lx = lx;
        g_ly = ly;
        if (pressed & HidNpadButton_B)
            g_close_request = true;
        if (pressed & HidNpadButton_L)
            g_tab_step = -1;
        if (pressed & HidNpadButton_R)
            g_tab_step = 1;
    } else if (g_swallow && !(held & ~(uint64_t)HidNpadButton_Minus)) {
        g_swallow = false;             /* everything lifted: the game again */
    }
    g_prev_held = held;
}

int menu_is_open(void) {
    return g_open;
}

void menu_set_open(int open) {
    if (open && !g_open) {
        g_open = true;
        g_swallow = true;
        g_focus_tab = menu_current_tab();
        settings_load();
        printf("  [menu ] opened (control socket)\n");
    } else if (!open && g_open) {
        close_menu();
    }
}

int menu_blocks_input(void) {
    return g_open || g_swallow;
}

void menu_render(int surface_w, int surface_h) {
    if (!g_open || surface_w <= 0 || surface_h <= 0)
        return;
    ensure_context(surface_h);
    ImGuiIO &io = ImGui::GetIO();

    static uint64_t last;
    const uint64_t now = armGetSystemTick();
    io.DeltaTime = last ? (float)(now - last) / (float)armGetSystemTickFreq() : 1.0f / 60.0f;
    if (io.DeltaTime <= 0.0f || io.DeltaTime > 0.5f)
        io.DeltaTime = 1.0f / 60.0f;
    last = now;
    io.DisplaySize = ImVec2((float)surface_w, (float)surface_h);
    feed_input(io, surface_w, surface_h);

    /* B closes the menu only when no combo or popup was open when it was
     * pressed; otherwise that B is ImGui closing the popup. Judged from the
     * previous frame, because ImGui has already closed the popup by the time
     * this frame's widgets run. */
    static bool popup_last_frame;
    const bool close_now = g_close_request && !popup_last_frame;
    g_close_request = false;

    ImGui::NewFrame();
    draw_menu((float)surface_w, (float)surface_h);
    popup_last_frame = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId |
                                                       ImGuiPopupFlags_AnyPopupLevel);
    ImGui::Render();
    render_draw_data(ImGui::GetDrawData(), surface_w, surface_h);

    if (close_now)
        close_menu();
}

} // extern "C"
