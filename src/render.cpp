// SPDX-License-Identifier: Apache-2.0
#include "pose_atlas/render.hpp"

#include <algorithm>
#include <cstring>
#include <map>

#include <nlohmann/json.hpp>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/glcorearb.h>

namespace pose_atlas {

namespace {

constexpr const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 texcoord;
uniform mat4 view_projection;
out vec2 uv;
void main() {
    gl_Position = view_projection * vec4(position, 1.0);
    uv = texcoord;
}
)";

// Flat region colour: base colour factor times the base colour texture
// (sRGB-decoded by the sampler; a white texel when the material has none),
// linear light, opaque (premultiplied trivially). Alpha mode MASK discards
// below the cutoff (a negative cutoff never discards).
constexpr const char* kFragmentShader = R"(#version 330 core
in vec2 uv;
uniform vec4 base_colour;
uniform sampler2D base_texture;
uniform float alpha_cutoff;
out vec4 fragment;
void main() {
    vec4 c = base_colour * texture(base_texture, uv);
    if (c.a < alpha_cutoff) discard;
    fragment = vec4(c.rgb, 1.0);
}
)";

std::string egl_error(const char* what) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, " (EGL error 0x%04x)", static_cast<unsigned>(eglGetError()));
    return std::string(what) + buffer;
}

GLuint compile(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024] = {};
        glGetShaderInfoLog(shader, sizeof log, nullptr, log);
        glDeleteShader(shader);
        throw RenderError(std::string("shader compilation failed: ") + log);
    }
    return shader;
}

bool contains(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

GLint gl_wrap(Wrap wrap) {
    switch (wrap) {
        case Wrap::ClampToEdge: return GL_CLAMP_TO_EDGE;
        case Wrap::MirroredRepeat: return GL_MIRRORED_REPEAT;
        case Wrap::Repeat: break;
    }
    return GL_REPEAT;
}

GLuint upload_texture(const Image& image, Wrap wrap_s, Wrap wrap_t) {
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // glTF's texture coordinate (0, 0) is the image's first (top-left) texel,
    // which is also where GL puts the first row uploaded.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 image.rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_wrap(wrap_s));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wrap(wrap_t));
    return texture;
}

// One draw: a primitive's range in the vertex buffer and its material.
struct DrawItem {
    GLint first = 0;
    GLsizei count = 0;
    std::array<float, 4> base_colour{};
    int texture = -1;  // index into Model::textures
    float alpha_cutoff = -1.0f;
};

}  // namespace

std::string to_string(TextureFilter filter) { return filter == TextureFilter::Bilinear ? "bilinear" : "nearest"; }

TextureFilter parse_texture_filter(const std::string& text) {
    if (text == "nearest") return TextureFilter::Nearest;
    if (text == "bilinear") return TextureFilter::Bilinear;
    throw std::invalid_argument("unknown texture filter '" + text + "' (expected nearest or bilinear)");
}

std::string render_settings_json(const RenderSettings& settings) {
    nlohmann::ordered_json j;
    j["style"] = kRenderStyleName;
    j["outline"] = settings.style.outline;
    j["outline_colour"] = hex_colour(settings.style.outline_colour);
    j["texture_filter"] = to_string(settings.texture_filter);
    j["palette_stage"] = to_string(settings.palette_stage);
    j["palette_sha256"] = settings.palette.empty() ? nlohmann::ordered_json(nullptr)
                                                   : nlohmann::ordered_json(palette_sha256(settings.palette));
    j["alpha_threshold"] = settings.alpha_threshold;
    j["only_materials"] = settings.only_materials;
    j["hidden_materials"] = settings.hidden_materials;
    return j.dump();
}

std::vector<std::string> validate_render_settings(const RenderSettings& settings) {
    std::vector<std::string> problems;
    const bool has_palette = !settings.palette.empty();
    if (palette_active(settings) && !has_palette) {
        problems.push_back("palette_stage " + to_string(settings.palette_stage) + " needs a palette");
    }
    if (!palette_active(settings) && has_palette) {
        problems.push_back("a palette needs a palette_stage other than none");
    }
    if (has_palette) {
        if (settings.palette.size() < kMinPaletteColours || settings.palette.size() > kMaxPaletteColours) {
            problems.push_back("the palette has " + std::to_string(settings.palette.size()) + " colours, not 2 to 256");
        }
        std::vector<Rgb8> sorted = settings.palette;
        std::sort(sorted.begin(), sorted.end());
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
            problems.push_back("the palette repeats a colour");
        }
    }
    if (!(settings.alpha_threshold > 0.0 && settings.alpha_threshold <= 1.0)) {
        problems.push_back("alpha_threshold must be in (0, 1]");
    }
    return problems;
}

struct Renderer::State {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    std::string description;
    GLuint program = 0;
    GLint view_projection = -1;
    GLint base_colour = -1;
    GLint base_texture = -1;
    GLint alpha_cutoff = -1;
    GLuint white = 0;  // 1x1 texture for materials without a texture
    std::map<std::uint64_t, GLuint> textures;  // by Texture::id
    std::size_t texture_bytes = 0;             // uploaded size of `textures`
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint fbo = 0;
    GLuint colour = 0;
    GLuint depth = 0;
    int size = 0;  // current target size

    void make_current() const {
        if (eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context) != EGL_TRUE) {
            throw RenderError(egl_error("eglMakeCurrent failed"));
        }
    }

    void ensure_target(int pixels) {
        if (pixels == size) return;
        GLint max_size = 0;
        glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_size);
        if (pixels > max_size) {
            throw RenderError("render target of " + std::to_string(pixels) + " pixels exceeds the maximum of " +
                              std::to_string(max_size) + "; lower cell_pixels or supersampling");
        }
        if (fbo == 0) {
            glGenFramebuffers(1, &fbo);
            glGenRenderbuffers(1, &colour);
            glGenRenderbuffers(1, &depth);
        }
        glBindRenderbuffer(GL_RENDERBUFFER, colour);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA32F, pixels, pixels);
        glBindRenderbuffer(GL_RENDERBUFFER, depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, pixels, pixels);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colour);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            throw RenderError("framebuffer incomplete");
        }
        size = pixels;
    }
};

Renderer::Renderer() : state_(std::make_unique<State>()) {
    // Pick Mesa's software device explicitly: the same rasteriser on every
    // machine, with or without a GPU.
    const auto query_devices =
        reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
    const auto query_device_string =
        reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(eglGetProcAddress("eglQueryDeviceStringEXT"));
    const auto get_platform_display =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (query_devices == nullptr || query_device_string == nullptr || get_platform_display == nullptr) {
        throw RenderError("EGL lacks device enumeration (EGL_EXT_device_enumeration, EGL_EXT_platform_device)");
    }
    EGLDeviceEXT devices[16];
    EGLint count = 0;
    if (query_devices(16, devices, &count) != EGL_TRUE) {
        throw RenderError(egl_error("eglQueryDevicesEXT failed; is Mesa's EGL installed?"));
    }
    EGLDeviceEXT software = nullptr;
    for (EGLint i = 0; i < count && software == nullptr; ++i) {
        const char* extensions = query_device_string(devices[i], EGL_EXTENSIONS);
        if (extensions != nullptr && std::strstr(extensions, "EGL_MESA_device_software") != nullptr) {
            software = devices[i];
        }
    }
    if (software == nullptr) {
        throw RenderError("no EGL software device (EGL_MESA_device_software); install Mesa's EGL and DRI drivers");
    }
    State& s = *state_;
    s.display = get_platform_display(EGL_PLATFORM_DEVICE_EXT, software, nullptr);
    EGLint major = 0, minor = 0;
    if (s.display == EGL_NO_DISPLAY || eglInitialize(s.display, &major, &minor) != EGL_TRUE) {
        throw RenderError(egl_error("cannot initialise the EGL display"));
    }
    if (eglBindAPI(EGL_OPENGL_API) != EGL_TRUE) {
        throw RenderError(egl_error("eglBindAPI(OpenGL) failed"));
    }
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
                                 EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    s.context = eglCreateContext(s.display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
    if (s.context == EGL_NO_CONTEXT) {
        throw RenderError(egl_error("cannot create an OpenGL 3.3 core context"));
    }
    try {
        s.make_current();
        s.description = std::string(reinterpret_cast<const char*>(glGetString(GL_RENDERER))) + "; " +
                        reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader);
        const GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
        s.program = glCreateProgram();
        glAttachShader(s.program, vs);
        glAttachShader(s.program, fs);
        glLinkProgram(s.program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = GL_FALSE;
        glGetProgramiv(s.program, GL_LINK_STATUS, &ok);
        if (ok != GL_TRUE) throw RenderError("shader program link failed");
        s.view_projection = glGetUniformLocation(s.program, "view_projection");
        s.base_colour = glGetUniformLocation(s.program, "base_colour");
        s.base_texture = glGetUniformLocation(s.program, "base_texture");
        s.alpha_cutoff = glGetUniformLocation(s.program, "alpha_cutoff");
        Image white(1, 1);
        std::fill(white.rgba.begin(), white.rgba.end(), std::uint8_t{255});
        s.white = upload_texture(white, Wrap::Repeat, Wrap::Repeat);
        glGenVertexArrays(1, &s.vao);
        glGenBuffers(1, &s.vbo);
    } catch (...) {
        eglMakeCurrent(s.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(s.display, s.context);
        throw;
    }
}

Renderer::~Renderer() {
    State& s = *state_;
    if (eglMakeCurrent(s.display, EGL_NO_SURFACE, EGL_NO_SURFACE, s.context) == EGL_TRUE) {
        if (s.fbo != 0) {
            glDeleteFramebuffers(1, &s.fbo);
            glDeleteRenderbuffers(1, &s.colour);
            glDeleteRenderbuffers(1, &s.depth);
        }
        for (const auto& entry : s.textures) glDeleteTextures(1, &entry.second);
        glDeleteTextures(1, &s.white);
        glDeleteBuffers(1, &s.vbo);
        glDeleteVertexArrays(1, &s.vao);
        glDeleteProgram(s.program);
    }
    // The display is shared by every renderer on the device (EGL returns the
    // same handle), so it stays initialised; only this context goes.
    eglMakeCurrent(s.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(s.display, s.context);
}

const std::string& Renderer::description() const { return state_->description; }

LinearImage Renderer::render_linear(const Model& model, const Pose& pose, const Profile& profile,
                                    const CameraGuide& guide, const RenderSettings& settings) {
    State& s = *state_;
    s.make_current();
    const int pixels = profile.cell_pixels * profile.supersampling;
    s.ensure_target(pixels);

    // Triangle soup: position (3) and texture coordinates (2) per vertex,
    // one draw per primitive.
    std::vector<float> vertices;
    std::vector<DrawItem> draws;
    for (std::size_t n = 0; n < model.nodes.size(); ++n) {
        const Node& node = model.nodes[n];
        if (node.mesh < 0) continue;
        const Mesh& mesh = model.meshes[static_cast<std::size_t>(node.mesh)];
        for (std::size_t p = 0; p < mesh.primitives.size(); ++p) {
            const Primitive& primitive = mesh.primitives[p];
            if (!settings.only_materials.empty() && !contains(settings.only_materials, primitive.material)) continue;
            if (contains(settings.hidden_materials, primitive.material)) continue;
            const PosedPrimitive posed =
                pose_primitive(model, static_cast<int>(n), static_cast<int>(p), pose);
            DrawItem draw;
            draw.first = static_cast<GLint>(vertices.size() / 5);
            draw.count = static_cast<GLsizei>(primitive.indices.size());
            draw.base_colour = primitive.base_colour;
            draw.texture = primitive.base_colour_texture;
            if (primitive.alpha_mode == AlphaMode::Mask) draw.alpha_cutoff = primitive.alpha_cutoff;
            const bool textured = draw.texture >= 0;
            for (std::uint32_t index : primitive.indices) {
                const Vec3& v = posed.positions.at(index);
                const std::array<float, 2> uv = textured ? primitive.texcoords.at(index) : std::array<float, 2>{};
                vertices.insert(vertices.end(), {static_cast<float>(v.x), static_cast<float>(v.y),
                                                 static_cast<float>(v.z), uv[0], uv[1]});
            }
            draws.push_back(draw);
        }
    }

    // View: camera axes right, up, toward (it looks along -toward) at the
    // guide's position. Orthographic: the cell spans ortho_scale_m in both
    // directions; depth from 0 to twice the camera distance.
    const double half = guide.ortho_scale_m / 2;
    const double near_m = 0.0;
    const double far_m = 2 * profile.camera_distance_m;
    const Vec3 axes[3] = {guide.right, guide.up, guide.toward};
    double m[16] = {};  // column-major
    for (int row = 0; row < 3; ++row) {
        const Vec3& a = axes[row];
        const double t = -dot(a, guide.position);
        double scale = 1.0 / half, offset = 0.0;
        if (row == 2) {
            // z_ndc = (-2 z_eye - (far + near)) / (far - near)
            scale = -2.0 / (far_m - near_m);
            offset = -(far_m + near_m) / (far_m - near_m);
        }
        m[0 * 4 + row] = a.x * scale;
        m[1 * 4 + row] = a.y * scale;
        m[2 * 4 + row] = a.z * scale;
        m[3 * 4 + row] = t * scale + offset;
    }
    m[15] = 1.0;
    float mf[16];
    for (int i = 0; i < 16; ++i) mf[i] = static_cast<float>(m[i]);

    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(0, 0, pixels, pixels);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DITHER);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!vertices.empty()) {
        glUseProgram(s.program);
        glUniformMatrix4fv(s.view_projection, 1, GL_FALSE, mf);
        glUniform1i(s.base_texture, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(s.vao);
        glBindBuffer(GL_ARRAY_BUFFER, s.vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)), vertices.data(),
                     GL_STREAM_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                              reinterpret_cast<const void*>(3 * sizeof(float)));
        const GLint filter = settings.texture_filter == TextureFilter::Bilinear ? GL_LINEAR : GL_NEAREST;
        for (const DrawItem& draw : draws) {
            GLuint texture = s.white;
            if (draw.texture >= 0) {
                const Texture& t = model.textures.at(static_cast<std::size_t>(draw.texture));
                auto found = s.textures.find(t.id);
                if (found == s.textures.end()) {
                    // Keep at most 256 MiB of textures uploaded: models are
                    // reloaded per atlas, and every load gets new ids.
                    const std::size_t bytes = t.image.rgba.size();
                    if (s.texture_bytes + bytes > (std::size_t{256} << 20)) {
                        for (const auto& entry : s.textures) glDeleteTextures(1, &entry.second);
                        s.textures.clear();
                        s.texture_bytes = 0;
                    }
                    s.texture_bytes += bytes;
                    found = s.textures.emplace(t.id, upload_texture(t.image, t.wrap_s, t.wrap_t)).first;
                }
                texture = found->second;
            }
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
            glUniform4f(s.base_colour, draw.base_colour[0], draw.base_colour[1], draw.base_colour[2],
                        draw.base_colour[3]);
            glUniform1f(s.alpha_cutoff, draw.alpha_cutoff);
            glDrawArrays(GL_TRIANGLES, draw.first, draw.count);
        }
    }

    LinearImage image;
    image.width = pixels;
    image.height = pixels;
    image.rgba.resize(static_cast<std::size_t>(pixels) * static_cast<std::size_t>(pixels) * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, pixels, pixels, GL_RGBA, GL_FLOAT, image.rgba.data());
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) throw RenderError("OpenGL error " + std::to_string(error) + " while rendering");
    // GL rows run bottom to top; flip to top to bottom.
    const std::size_t row = static_cast<std::size_t>(pixels) * 4;
    std::vector<float> swap(row);
    for (std::size_t y = 0; y < static_cast<std::size_t>(pixels) / 2; ++y) {
        float* a = image.rgba.data() + y * row;
        float* b = image.rgba.data() + (static_cast<std::size_t>(pixels) - 1 - y) * row;
        std::memcpy(swap.data(), a, row * sizeof(float));
        std::memcpy(a, b, row * sizeof(float));
        std::memcpy(b, swap.data(), row * sizeof(float));
    }
    return image;
}

Image Renderer::render(const Model& model, const Pose& pose, const Profile& profile, const CameraGuide& guide,
                       const RenderSettings& settings, PaletteStats* stats) {
    const auto problems = validate_render_settings(settings);
    if (!problems.empty()) throw RenderError("render settings: " + problems.front());
    LinearImage linear = render_linear(model, pose, profile, guide, settings);
    if (!palette_active(settings)) {
        Image cell = downsample(linear, profile.supersampling, profile.resampling_filter);
        if (settings.style.outline) apply_outline(cell, settings.style.outline_colour);
        return cell;
    }
    PaletteMatcher matcher(settings.palette);
    if (snaps_before(settings.palette_stage)) snap_linear(linear, matcher);
    Image cell = downsample(linear, profile.supersampling, profile.resampling_filter);
    PaletteStats counts;
    for (std::size_t i = 0; i + 3 < cell.rgba.size(); i += 4) {
        const std::uint8_t* p = cell.rgba.data() + i;
        if (p[3] > 0 && p[3] < 255) ++counts.partial_alpha;
        if (alpha_passes(p[3], settings.alpha_threshold) && !matcher.contains({p[0], p[1], p[2]})) {
            ++counts.off_palette_before_snap;
        }
    }
    if (snaps_after(settings.palette_stage)) snap_image(cell, matcher);
    threshold_alpha(cell, settings.alpha_threshold);
    if (settings.style.outline) apply_outline(cell, settings.style.outline_colour);
    for (std::size_t i = 0; i + 3 < cell.rgba.size(); i += 4) {
        const std::uint8_t* p = cell.rgba.data() + i;
        if (p[3] == 0) continue;
        ++counts.opaque;
        const Rgb8 c{p[0], p[1], p[2]};
        if (!matcher.contains(c) && !(settings.style.outline && c == settings.style.outline_colour)) {
            ++counts.off_palette_output;
        }
    }
    if (stats != nullptr) *stats = counts;
    return cell;
}

}  // namespace pose_atlas
