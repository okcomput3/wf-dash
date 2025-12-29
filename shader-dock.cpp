/**
 * Wayfire Shader Dock Plugin
 * 
 * A dock/panel plugin that renders application icons with bevel/shimmer/3D effects.
 * Uses custom OpenGL shaders rendered directly to output framebuffer.
 */

#include <wayfire/plugin.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/core.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>
#include <wayfire/util/log.hpp>
#include <wayfire/util.hpp>

#include <GLES3/gl3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <cmath>

#include <unistd.h>
#include <sys/types.h>
#include <png.h>
#include <linux/input-event-codes.h>

extern char **environ;

namespace shader_dock
{

// ============================================================================
// Shader Sources
// ============================================================================

static const char* vertex_shader_src = R"(#version 300 es
precision highp float;
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_texcoord;
out vec2 v_texcoord;
uniform mat4 u_mvp;
void main() {
    gl_Position = u_mvp * vec4(a_position, 0.0, 1.0);
    v_texcoord = a_texcoord;
}
)";

static const char* icon_fragment_shader_src = R"(#version 300 es
precision highp float;
in vec2 v_texcoord;
out vec4 frag_color;

uniform sampler2D u_texture;
uniform vec2 iResolution;
uniform float cornerRadius;
uniform vec4 bevelColor;
uniform float time;
uniform float hover;

const float bevelWidth = 12.0;
const float aa = 1.5;

float sdRoundedBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() {
    float bounce = 1.0 + hover * (sin(time * 6.0) * 0.05 + 0.08);
    
    vec2 p = (v_texcoord - 0.5) * iResolution;
    float scaledCornerRadius = cornerRadius * bounce;
    float d = sdRoundedBox(p, iResolution * 0.5 * bounce, scaledCornerRadius);
    float shape_alpha = 1.0 - smoothstep(-aa, aa, d);
    float bevel_intensity = smoothstep(-bevelWidth, 0.0, d) - smoothstep(0.0, aa, d);
    
    float center_distance = length(p) / (min(iResolution.x, iResolution.y) * 0.5);
    float button_height = pow(1.0 - smoothstep(0.0, 0.8, center_distance), 2.0);
    
    vec2 light_dir = normalize(vec2(-1.0, -1.0));
    float button_lighting = 0.5 + dot(normalize(p), light_dir) * 0.3 * button_height;
    
    float combined_bevel = max(bevel_intensity, button_height * 0.4);
    float angle = atan(p.y, p.x);
    float highlight_factor = pow(sin(angle * 2.0 - time * 2.5) * 0.5 + 0.5, 8.0);
    float brightness = (0.7 + highlight_factor * 0.6) * button_lighting;
    
    float shimmer = sin((p.x + p.y) / (iResolution.x + iResolution.y) * 8.0 + time * 4.0);
    float shimmer_intensity = smoothstep(0.6, 1.0, shimmer) * 0.3 * 
                              smoothstep(-bevelWidth * 0.5, bevelWidth * 0.5, -abs(d));
    
    vec2 scaled_uv = clamp((v_texcoord - 0.5) / bounce + 0.5, 0.0, 1.0);
    vec4 tex_color = texture(u_texture, scaled_uv);
    
    vec3 bevel_col = mix(bevelColor.rgb * brightness, vec3(1.0, 1.0, 0.9), shimmer_intensity);
    vec3 final_rgb = mix(tex_color.rgb, bevel_col, combined_bevel * bevelColor.a);
    final_rgb += vec3(0.2, 0.15, 0.1) * hover * (1.0 - center_distance);
    
    frag_color = vec4(final_rgb, tex_color.a * shape_alpha);
}
)";

static const char* background_fragment_shader_src = R"(#version 300 es
precision highp float;
in vec2 v_texcoord;
out vec4 frag_color;

uniform vec2 iResolution;
uniform float cornerRadius;
uniform vec4 backgroundColor;
uniform float time;

float sdRoundedBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

void main() {
    vec2 p = (v_texcoord - 0.5) * iResolution;
    float d = sdRoundedBox(p, iResolution * 0.5, cornerRadius);
    
    float aa = 1.5;
    float shape_alpha = 1.0 - smoothstep(-aa, aa, d);
    float border = smoothstep(-3.0, 0.0, d) - smoothstep(0.0, aa, d);
    
    float hue = fract((v_texcoord.x + v_texcoord.y) * 0.5 - time * 0.1);
    vec3 border_color = hsv2rgb(vec3(hue, 0.8, 1.0));
    
    vec3 final_color = mix(backgroundColor.rgb, border_color, border * 0.8);
    frag_color = vec4(final_color, backgroundColor.a * shape_alpha);
}
)";

// ============================================================================
// Structures
// ============================================================================

struct DockIcon
{
    std::string app_id;
    std::string name;
    std::string exec;
    std::string icon_path;
    GLuint texture_id = 0;
    int width = 0;
    int height = 0;
    float hover = 0.0f;
    bool texture_loaded = false;
};

class ShaderProgram
{
  public:
    GLuint program = 0;
    GLint u_mvp = -1;
    GLint u_texture = -1;
    GLint u_resolution = -1;
    GLint u_corner_radius = -1;
    GLint u_bevel_color = -1;
    GLint u_background_color = -1;
    GLint u_time = -1;
    GLint u_hover = -1;

    bool compile(const char* vert_src, const char* frag_src)
    {
        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vs, 1, &vert_src, nullptr);
        glCompileShader(vs);
        
        GLint success;
        glGetShaderiv(vs, GL_COMPILE_STATUS, &success);
        if (!success) {
            char log[512];
            glGetShaderInfoLog(vs, 512, nullptr, log);
            LOGD("VS compile error: ", log);
            glDeleteShader(vs);
            return false;
        }

        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fs, 1, &frag_src, nullptr);
        glCompileShader(fs);
        
        glGetShaderiv(fs, GL_COMPILE_STATUS, &success);
        if (!success) {
            char log[512];
            glGetShaderInfoLog(fs, 512, nullptr, log);
            LOGD("FS compile error: ", log);
            glDeleteShader(vs);
            glDeleteShader(fs);
            return false;
        }

        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        
        glDeleteShader(vs);
        glDeleteShader(fs);

        glGetProgramiv(program, GL_LINK_STATUS, &success);
        if (!success) {
            char log[512];
            glGetProgramInfoLog(program, 512, nullptr, log);
            LOGD("Program link error: ", log);
            glDeleteProgram(program);
            program = 0;
            return false;
        }

        u_mvp = glGetUniformLocation(program, "u_mvp");
        u_texture = glGetUniformLocation(program, "u_texture");
        u_resolution = glGetUniformLocation(program, "iResolution");
        u_corner_radius = glGetUniformLocation(program, "cornerRadius");
        u_bevel_color = glGetUniformLocation(program, "bevelColor");
        u_background_color = glGetUniformLocation(program, "backgroundColor");
        u_time = glGetUniformLocation(program, "time");
        u_hover = glGetUniformLocation(program, "hover");

        return true;
    }

    void destroy()
    {
        if (program) {
            glDeleteProgram(program);
            program = 0;
        }
    }
};

// ============================================================================
// Helper Functions
// ============================================================================

bool load_png_texture(const std::string& path, GLuint& texture_id, int& width, int& height)
{
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) { fclose(fp); return false; }

    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, nullptr, nullptr); fclose(fp); return false; }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        fclose(fp);
        return false;
    }

    png_init_io(png, fp);
    png_read_info(png, info);

    width = png_get_image_width(png, info);
    height = png_get_image_height(png, info);
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth = png_get_bit_depth(png, info);

    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);

    png_read_update_info(png, info);

    std::vector<png_bytep> row_pointers(height);
    std::vector<png_byte> image_data(width * height * 4);
    for (int y = 0; y < height; y++)
        row_pointers[y] = image_data.data() + y * width * 4;

    png_read_image(png, row_pointers.data());
    png_destroy_read_struct(&png, &info, nullptr);
    fclose(fp);

    // Save current texture binding
    GLint prev_texture;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_texture);

    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image_data.data());

    // Restore previous texture binding
    glBindTexture(GL_TEXTURE_2D, prev_texture);

    return true;
}

std::string find_icon_path(const std::string& icon_name, [[maybe_unused]] int size)
{
    std::vector<std::string> theme_dirs = {
        "/usr/share/icons/hicolor",
        "/usr/share/icons/Adwaita", 
        "/usr/share/icons/breeze",
        "/usr/share/icons/Papirus"
    };
    std::vector<std::string> sizes = {"256x256", "128x128", "96x96", "64x64", "48x48"};
    std::vector<std::string> cats = {"apps", "applications"};

    if (icon_name.find('/') != std::string::npos && std::filesystem::exists(icon_name))
        return icon_name;

    for (const auto& theme : theme_dirs)
        for (const auto& sz : sizes)
            for (const auto& cat : cats) {
                std::string path = theme + "/" + sz + "/" + cat + "/" + icon_name + ".png";
                if (std::filesystem::exists(path)) return path;
            }

    std::string pixmap = "/usr/share/pixmaps/" + icon_name + ".png";
    if (std::filesystem::exists(pixmap)) return pixmap;
    return "";
}

bool parse_desktop_file(const std::string& app_id, DockIcon& icon)
{
    std::vector<std::string> paths = {
        "/usr/share/applications/",
        "/usr/local/share/applications/",
        std::string(getenv("HOME") ? getenv("HOME") : "") + "/.local/share/applications/"
    };

    std::string desktop_file;
    for (const auto& p : paths) {
        std::string full = p + app_id + ".desktop";
        if (std::filesystem::exists(full)) { desktop_file = full; break; }
    }
    if (desktop_file.empty()) return false;

    std::ifstream file(desktop_file);
    if (!file.is_open()) return false;

    std::string line;
    bool in_entry = false;
    while (std::getline(file, line)) {
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') { in_entry = (line == "[Desktop Entry]"); continue; }
        if (!in_entry) continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        
        // Trim
        key.erase(key.find_last_not_of(" \t") + 1);
        size_t vs = val.find_first_not_of(" \t");
        if (vs != std::string::npos) val = val.substr(vs);

        if (key == "Name") icon.name = val;
        else if (key == "Exec") {
            size_t pos;
            while ((pos = val.find('%')) != std::string::npos)
                val.erase(pos, pos + 1 < val.size() ? 2 : 1);
            val.erase(val.find_last_not_of(" \t") + 1);
            icon.exec = val;
        }
        else if (key == "Icon") icon.icon_path = val;
    }
    icon.app_id = app_id;
    return !icon.exec.empty();
}

void launch_application(const std::string& exec_cmd)
{
    LOGD("shader-dock: launching '", exec_cmd, "'");
    
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        // Detach from parent process group
        setsid();
        
        // Close file descriptors
        for (int fd = 3; fd < 256; fd++) {
            close(fd);
        }
        
        // Execute via shell for proper parsing
        execl("/bin/sh", "sh", "-c", exec_cmd.c_str(), nullptr);
        
        // If exec fails, exit child
        _exit(127);
    } else if (pid > 0) {
        // Parent - don't wait, let the child run independently
        LOGD("shader-dock: spawned process ", pid);
    } else {
        LOGD("shader-dock: fork failed");
    }
}

// ============================================================================
// Main Plugin
// ============================================================================

class ShaderDockPlugin : public wf::per_output_plugin_instance_t
{
    wf::option_wrapper_t<int> opt_icon_size{"shader-dock/icon_size"};
    wf::option_wrapper_t<int> opt_spacing{"shader-dock/spacing"};
    wf::option_wrapper_t<int> opt_margin{"shader-dock/margin"};
    wf::option_wrapper_t<double> opt_corner_radius{"shader-dock/corner_radius"};
    wf::option_wrapper_t<wf::color_t> opt_bevel_color{"shader-dock/bevel_color"};
    wf::option_wrapper_t<wf::color_t> opt_background_color{"shader-dock/background_color"};
    wf::option_wrapper_t<std::string> opt_apps{"shader-dock/apps"};

    std::vector<DockIcon> icons;
    ShaderProgram icon_shader, bg_shader;
    GLuint vao = 0, vbo = 0, ebo = 0;
    bool gl_initialized = false;

    wf::geometry_t dock_geometry{0, 0, 0, 0};
    int icon_size = 64, spacing = 8, margin = 8;
    float corner_radius = 12.0f;
    glm::vec4 bevel_color{0.8f, 0.7f, 0.5f, 0.6f};
    glm::vec4 bg_color{0.1f, 0.1f, 0.1f, 0.85f};

    std::chrono::steady_clock::time_point start_time;
    int hovered_icon = -1;
    wf::wl_timer<false> animation_timer;

    wf::signal::connection_t<wf::output_configuration_changed_signal> on_output_changed =
        [=] (wf::output_configuration_changed_signal*) {
            update_geometry();
            output->render->damage_whole();
        };

    // Pointer button signal connection
    wf::signal::connection_t<wf::post_input_event_signal<wlr_pointer_button_event>> on_button =
        [=] (wf::post_input_event_signal<wlr_pointer_button_event> *ev) {
            handle_button(ev->event);
        };

  public:
    void init() override
    {
        start_time = std::chrono::steady_clock::now();
        
        // Get config values with sensible defaults
        icon_size = opt_icon_size;
        spacing = opt_spacing;
        margin = opt_margin;
        corner_radius = opt_corner_radius;
        
        // Ensure sensible minimums
        if (icon_size <= 0) icon_size = 64;
        if (spacing < 0) spacing = 8;
        if (margin < 0) margin = 8;
        if (corner_radius < 0) corner_radius = 12.0f;
        
        LOGD("shader-dock: icon_size=", icon_size, " spacing=", spacing, " margin=", margin);

        wf::color_t bc = opt_bevel_color;
        bevel_color = glm::vec4(bc.r, bc.g, bc.b, bc.a);
        wf::color_t bgc = opt_background_color;
        bg_color = glm::vec4(bgc.r, bgc.g, bgc.b, bgc.a);

        std::string apps_str = opt_apps;
        LOGD("shader-dock: apps = '", apps_str, "'");
        
        std::istringstream iss(apps_str);
        std::string app;
        while (iss >> app) {
            DockIcon icon;
            if (parse_desktop_file(app, icon)) {
                std::string path = find_icon_path(icon.icon_path, icon_size);
                if (!path.empty()) {
                    icon.icon_path = path;
                    icons.push_back(std::move(icon));
                    LOGD("shader-dock: added ", app);
                }
            }
        }

        update_geometry();

        output->render->add_effect(&damage_hook, wf::OUTPUT_EFFECT_DAMAGE);
        output->render->add_effect(&overlay_hook, wf::OUTPUT_EFFECT_OVERLAY);
        output->connect(&on_output_changed);
        
        // Connect to pointer button events
        wf::get_core().connect(&on_button);

        animation_timer.set_timeout(16, [this] () {
            output->render->damage(dock_geometry);
            return true;
        });

        output->render->damage_whole();
        LOGD("shader-dock: initialized with ", icons.size(), " icons");
    }

    void handle_button(wlr_pointer_button_event *event)
    {
        auto cursor = wf::get_core().get_cursor_position();
        
        LOGD("shader-dock: button event - button=", event->button, 
             " state=", event->state, " cursor=(", cursor.x, ",", cursor.y, ")");
        
        // Only handle left click release
        if (event->button != BTN_LEFT || event->state != WLR_BUTTON_RELEASED) {
            return;
        }
        
        int clicked = get_icon_at(cursor.x, cursor.y);
        LOGD("shader-dock: left click release, icon index=", clicked);
        
        if (clicked >= 0 && clicked < (int)icons.size()) {
            LOGD("shader-dock: launching '", icons[clicked].exec, "'");
            launch_application(icons[clicked].exec);
        }
    }

    void update_geometry()
    {
        auto og = output->get_layout_geometry();
        int n = icons.empty() ? 1 : (int)icons.size();
        // Vertical layout on left edge
        int w = icon_size + margin * 2;
        int h = n * icon_size + (n > 1 ? (n - 1) * spacing : 0) + margin * 2;
        dock_geometry.x = og.x + margin;
        dock_geometry.y = og.y + (og.height - h) / 2;  // Centered vertically
        dock_geometry.width = w;
        dock_geometry.height = h;
        
        LOGD("shader-dock: geometry x=", dock_geometry.x, " y=", dock_geometry.y, 
             " w=", dock_geometry.width, " h=", dock_geometry.height,
             " icons=", n, " icon_size=", icon_size, " spacing=", spacing);
    }

    float get_time() const {
        return std::chrono::duration<float>(std::chrono::steady_clock::now() - start_time).count();
    }

    int get_icon_at(int x, int y) const {
        if (x < dock_geometry.x || x >= dock_geometry.x + dock_geometry.width ||
            y < dock_geometry.y || y >= dock_geometry.y + dock_geometry.height)
            return -1;
        int ly = y - dock_geometry.y - margin;
        if (ly < 0) return -1;
        int idx = ly / (icon_size + spacing);
        int off = ly % (icon_size + spacing);
        if (idx >= 0 && idx < (int)icons.size() && off < icon_size) {
            // Reverse index to match visual order (icons render bottom-to-top in array)
            return (int)icons.size() - 1 - idx;
        }
        return -1;
    }

    void init_gl()
    {
        if (gl_initialized) return;

        // Save GL state
        GLint prev_vao, prev_vbo, prev_texture;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_vbo);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_texture);

        if (!icon_shader.compile(vertex_shader_src, icon_fragment_shader_src)) {
            LOGD("shader-dock: icon shader failed");
            return;
        }
        if (!bg_shader.compile(vertex_shader_src, background_fragment_shader_src)) {
            LOGD("shader-dock: bg shader failed");
            return;
        }

        float verts[] = {
            // pos x, pos y, tex u, tex v (flipped V)
            0, 0, 0, 1,
            1, 0, 1, 1,
            1, 1, 1, 0,
            0, 1, 0, 0
        };
        unsigned int inds[] = {0, 1, 2, 2, 3, 0};

        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ebo);

        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(inds), inds, GL_STATIC_DRAW);

        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);
        glEnableVertexAttribArray(1);

        for (auto& icon : icons) {
            if (!icon.texture_loaded && !icon.icon_path.empty()) {
                if (load_png_texture(icon.icon_path, icon.texture_id, icon.width, icon.height)) {
                    icon.texture_loaded = true;
                    LOGD("shader-dock: loaded ", icon.app_id);
                }
            }
        }

        // Restore GL state
        glBindVertexArray(prev_vao);
        glBindBuffer(GL_ARRAY_BUFFER, prev_vbo);
        glBindTexture(GL_TEXTURE_2D, prev_texture);

        gl_initialized = true;
    }

    void render_dock(const wf::render_target_t& fb)
    {
        if (icons.empty()) return;

        auto cursor = wf::get_core().get_cursor_position();
        hovered_icon = get_icon_at(cursor.x, cursor.y);

        float time = get_time();

        // Save GL state to prevent black screen flashes
        GLint prev_program;
        glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
        GLint prev_vao;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
        GLint prev_texture;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_texture);
        GLint prev_active_texture;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active_texture);
        GLboolean prev_blend = glIsEnabled(GL_BLEND);
        GLint prev_blend_src_rgb, prev_blend_dst_rgb, prev_blend_src_a, prev_blend_dst_a;
        glGetIntegerv(GL_BLEND_SRC_RGB, &prev_blend_src_rgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &prev_blend_dst_rgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &prev_blend_src_a);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &prev_blend_dst_a);

        // Use wayfire's projection (Y down, top-left origin)
        glm::mat4 proj = glm::ortho(
            (float)fb.geometry.x,
            (float)(fb.geometry.x + fb.geometry.width),
            (float)(fb.geometry.y + fb.geometry.height),
            (float)fb.geometry.y,
            -1.0f, 1.0f
        );

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Background
        glUseProgram(bg_shader.program);
        glm::mat4 model = glm::translate(glm::mat4(1), glm::vec3(dock_geometry.x, dock_geometry.y, 0));
        model = glm::scale(model, glm::vec3(dock_geometry.width, dock_geometry.height, 1));
        glm::mat4 mvp = proj * model;

        glUniformMatrix4fv(bg_shader.u_mvp, 1, GL_FALSE, glm::value_ptr(mvp));
        glUniform2f(bg_shader.u_resolution, dock_geometry.width, dock_geometry.height);
        glUniform1f(bg_shader.u_corner_radius, corner_radius + 4);
        glUniform4fv(bg_shader.u_background_color, 1, glm::value_ptr(bg_color));
        glUniform1f(bg_shader.u_time, time);

        glBindVertexArray(vao);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

        // Icons - vertical layout
        glUseProgram(icon_shader.program);
        float icon_x = (float)(dock_geometry.x + margin);
        float icon_y = (float)(dock_geometry.y + margin);
        float icon_step = (float)(icon_size + spacing);

        for (size_t i = 0; i < icons.size(); i++) {
            auto& icon = icons[i];
            if (!icon.texture_loaded) { 
                icon_y += icon_step; 
                continue; 
            }

            float target = (hovered_icon == (int)i) ? 1.0f : 0.0f;
            icon.hover += (target - icon.hover) * 0.2f;

            model = glm::translate(glm::mat4(1.0f), glm::vec3(icon_x, icon_y, 0.0f));
            model = glm::scale(model, glm::vec3((float)icon_size, (float)icon_size, 1.0f));
            mvp = proj * model;

            glUniformMatrix4fv(icon_shader.u_mvp, 1, GL_FALSE, glm::value_ptr(mvp));
            glUniform1i(icon_shader.u_texture, 0);
            glUniform2f(icon_shader.u_resolution, (float)icon_size, (float)icon_size);
            glUniform1f(icon_shader.u_corner_radius, corner_radius);
            glUniform4fv(icon_shader.u_bevel_color, 1, glm::value_ptr(bevel_color));
            glUniform1f(icon_shader.u_time, time);
            glUniform1f(icon_shader.u_hover, icon.hover);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, icon.texture_id);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

            icon_y += icon_step;  // Move down for vertical layout
        }

        // Restore GL state
        glBindVertexArray(prev_vao);
        glActiveTexture(prev_active_texture);
        glBindTexture(GL_TEXTURE_2D, prev_texture);
        glUseProgram(prev_program);
        
        if (prev_blend)
            glEnable(GL_BLEND);
        else
            glDisable(GL_BLEND);
        glBlendFuncSeparate(prev_blend_src_rgb, prev_blend_dst_rgb, prev_blend_src_a, prev_blend_dst_a);
    }

    wf::effect_hook_t damage_hook = [=] () {
        output->render->damage(dock_geometry, false);
    };

    wf::effect_hook_t overlay_hook = [=] () {
        if (icons.empty()) return;

        if (!gl_initialized) {
            init_gl();
            if (!gl_initialized) return;
        }

        auto fb = output->render->get_target_framebuffer();
        render_dock(fb);
    };

    void fini() override
    {
        animation_timer.disconnect();
        output->render->rem_effect(&damage_hook);
        output->render->rem_effect(&overlay_hook);
        on_button.disconnect();

        for (auto& icon : icons)
            if (icon.texture_id) glDeleteTextures(1, &icon.texture_id);

        if (vao) glDeleteVertexArrays(1, &vao);
        if (vbo) glDeleteBuffers(1, &vbo);
        if (ebo) glDeleteBuffers(1, &ebo);
        icon_shader.destroy();
        bg_shader.destroy();

        output->render->damage(dock_geometry);
        LOGD("shader-dock: finalized");
    }
};

} // namespace shader_dock

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<shader_dock::ShaderDockPlugin>);
