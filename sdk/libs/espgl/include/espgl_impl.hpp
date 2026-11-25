#pragma once

// EspGL's implementation, inline so the library stays header-only.
// Included from espgl.hpp; include that instead.

#include "espgl.hpp"

#include <cstring>



namespace espgl {

namespace detail {

// Fills n 16-bit values at p with v. The buffers are in PSRAM, where clearing
// them is a good part of a frame, so this writes whole words: memset when v's
// two bytes match (black, white, a cleared depth buffer), else 32-bit stores.
inline void fill16(uint16_t* p, size_t n, uint16_t v) {
    if ((v & 0xFF) == (v >> 8)) {
        std::memset(p, v & 0xFF, n * sizeof(uint16_t));
        return;
    }
    if (n > 0 && (reinterpret_cast<uintptr_t>(p) & 2)) {
        *p++ = v;
        --n;
    }
    using Word = uint32_t __attribute__((may_alias));
    Word* w = reinterpret_cast<Word*>(p);
    const uint32_t vv = v | (uint32_t(v) << 16);
    size_t words = n / 2;
    for (; words >= 8; words -= 8, w += 8) {
        w[0] = vv; w[1] = vv; w[2] = vv; w[3] = vv;
        w[4] = vv; w[5] = vv; w[6] = vv; w[7] = vv;
    }
    for (; words > 0; --words) {
        *w++ = vv;
    }
    if (n & 1) {
        p[n - 1] = v;
    }
}

} // namespace detail

inline EspGL::EspGL()
    : current_matrix_mode_(MODELVIEW)
    , width_(0)
    , height_(0)
    , clear_color_({0, 0, 0, 255})
    , line_width_(1.0f)
    , current_color_({255, 255, 255, 255})
    , viewport_x_(0)
    , viewport_y_(0)
    , viewport_width_(0)
    , viewport_height_(0)
    , current_primitive_(POINTS)
    , immediate_mode_active_(false)
    , hw_accel_enabled_(false)
    , depth_test_enabled_(false) // initialize depth test off
    , blend_enabled_(false) // initialize blending off
    , blend_src_factor_(SRC_ALPHA)
    , blend_dst_factor_(ONE_MINUS_SRC_ALPHA)
{
    // Initialize matrices
    modelview_matrix_ = createIdentityMatrix();
    projection_matrix_ = createIdentityMatrix();
}

inline EspGL::~EspGL() {
    deinit();
}

inline void EspGL::init(uint16_t width, uint16_t height, bool own_frame_buffer) {
    width_ = width;
    height_ = height;
    viewport_width_ = width;
    viewport_height_ = height;

    // The color buffer, unless the caller hands one over with setFrameBuffer()
    if (own_frame_buffer) {
        own_pixels_ = std::make_unique<uint16_t[]>(width * height);
    } else {
        own_pixels_.reset();
    }
    pixels_ = own_pixels_.get();
    depth_buffer_ = std::make_unique<uint16_t[]>(width * height);
    // All of the new depth buffer needs clearing
    depth_dirty_x0_ = 0;
    depth_dirty_y0_ = 0;
    depth_dirty_x1_ = width;
    depth_dirty_y1_ = height;

    clear(COLOR_BUFFER_BIT | DEPTH_BUFFER_BIT);
}

inline void EspGL::setFrameBuffer(uint16_t* pixels) {
    pixels_ = pixels ? pixels : own_pixels_.get();
}

inline void EspGL::deinit() {
    own_pixels_.reset();
    pixels_ = nullptr;
    depth_buffer_.reset();
    width_ = 0;
    height_ = 0;
}

inline void EspGL::matrixMode(MatrixMode mode) {
    current_matrix_mode_ = mode;
}

inline EspGL::Matrix4x4& EspGL::currentMatrix() {
    // Every change to either matrix comes through here (or setProjection)
    mvp_dirty_ = true;
    return (current_matrix_mode_ == MODELVIEW) ? modelview_matrix_ : projection_matrix_;
}

inline const EspGL::Matrix4x4& EspGL::mvp() const {
    if (mvp_dirty_) {
        mvp_ = multiplyMatrices(projection_matrix_, modelview_matrix_);
        mvp_dirty_ = false;
    }
    return mvp_;
}

inline std::vector<EspGL::Matrix4x4>& EspGL::currentStack() {
    return (current_matrix_mode_ == MODELVIEW) ? modelview_stack_ : projection_stack_;
}

inline void EspGL::loadIdentity() {
    currentMatrix() = createIdentityMatrix();
}

inline void EspGL::loadMatrix(const Matrix4x4& matrix) {
    currentMatrix() = matrix;
}

inline void EspGL::multMatrix(const Matrix4x4& matrix) {
    // Right-multiplication, as in OpenGL: the last transform specified is the
    // first one applied to the vertices.
    Matrix4x4& target = currentMatrix();
    target = multiplyMatrices(target, matrix);
}

inline void EspGL::translate(float x, float y, float z) {
    multMatrix(createTranslationMatrix(x, y, z));
}

inline void EspGL::rotate(float angle, float x, float y, float z) {
    multMatrix(createRotationMatrix(angle, x, y, z));
}

inline void EspGL::scale(float x, float y, float z) {
    multMatrix(createScaleMatrix(x, y, z));
}

inline void EspGL::lookAt(float eyeX, float eyeY, float eyeZ,
                   float centerX, float centerY, float centerZ,
                   float upX, float upY, float upZ) {
    multMatrix(createLookAtMatrix(eyeX, eyeY, eyeZ,
                                  centerX, centerY, centerZ,
                                  upX, upY, upZ));
}

inline void EspGL::pushMatrix() {
    currentStack().push_back(currentMatrix());
}

inline void EspGL::popMatrix() {
    auto& stack = currentStack();
    if (!stack.empty()) {
        currentMatrix() = stack.back();
        stack.pop_back();
    }
}

inline EspGL::Matrix4x4 EspGL::getMatrix() const {
    return (current_matrix_mode_ == MODELVIEW) ? modelview_matrix_ : projection_matrix_;
}

inline void EspGL::clearColor(float r, float g, float b, float a) {
    clear_color_ = {
        static_cast<uint8_t>(r * 255),
        static_cast<uint8_t>(g * 255),
        static_cast<uint8_t>(b * 255),
        static_cast<uint8_t>(a * 255)
    };
}

inline void EspGL::clear(uint32_t mask) {
    if ((mask & COLOR_BUFFER_BIT) && pixels_) {
        detail::fill16(pixels_, size_t(width_) * height_, colorToRGB565(clear_color_));
    }
    if (mask & DEPTH_BUFFER_BIT) {
        clearDepth();
    }
}

inline void EspGL::enable(uint32_t cap) {
    switch (cap) {
        case DEPTH_TEST: depth_test_enabled_ = true; break;
        case BLEND: blend_enabled_ = true; break;
        case CULL_FACE: cull_enabled_ = true; break;
        default: break;
    }
}

inline void EspGL::disable(uint32_t cap) {
    switch (cap) {
        case DEPTH_TEST: depth_test_enabled_ = false; break;
        case BLEND: blend_enabled_ = false; break;
        case CULL_FACE: cull_enabled_ = false; break;
        default: break;
    }
}

inline void EspGL::lineWidth(float width) {
    line_width_ = width;
}

inline void EspGL::color3f(float r, float g, float b) {
    color4f(r, g, b, 1.0f);
}

inline void EspGL::color4f(float r, float g, float b, float a) {
    current_color_ = {
        static_cast<uint8_t>(r * 255),
        static_cast<uint8_t>(g * 255),
        static_cast<uint8_t>(b * 255),
        static_cast<uint8_t>(a * 255)
    };
}

inline void EspGL::begin(PrimitiveType type) {
    current_primitive_ = type;
    immediate_mode_active_ = true;
    vertices_.clear();
}

inline void EspGL::end() {
    if (!immediate_mode_active_) return;
    

    
    // Process vertices based on primitive type
    switch (current_primitive_) {
        case LINES:
            for (size_t i = 0; i < vertices_.size(); i += 2) {
                if (i + 1 < vertices_.size()) {
                    drawLine(vertices_[i].position, vertices_[i + 1].position, vertices_[i].color);
                }
            }
            break;
        case LINE_STRIP:
            for (size_t i = 0; i + 1 < vertices_.size(); ++i) {
                drawLine(vertices_[i].position, vertices_[i + 1].position, vertices_[i].color);
            }
            break;
        case LINE_LOOP:
            for (size_t i = 0; vertices_.size() >= 2 && i < vertices_.size(); ++i) {
                size_t next = (i + 1) % vertices_.size();
                drawLine(vertices_[i].position, vertices_[next].position, vertices_[i].color);
            }
            break;
        case TRIANGLES:
            for (size_t i = 0; i < vertices_.size(); i += 3) {
                if (i + 2 < vertices_.size()) {
                    drawTriangle(vertices_[i], vertices_[i + 1], vertices_[i + 2]);
                }
            }
            break;
        case TRIANGLE_STRIP:
            for (size_t i = 0; i + 2 < vertices_.size(); ++i) {
                drawTriangle(vertices_[i], vertices_[i + 1], vertices_[i + 2]);
            }
            break;
        case TRIANGLE_FAN:
            if (vertices_.size() >= 3) {
                for (size_t i = 1; i < vertices_.size() - 1; ++i) {
                    drawTriangle(vertices_[0], vertices_[i], vertices_[i + 1]);
                }
            }
            break;
        default:
            // Primitive type not implemented
            break;
    }
    
    immediate_mode_active_ = false;
    vertices_.clear();
}

inline void EspGL::vertex3f(float x, float y, float z) {
    if (!immediate_mode_active_) return;
    
    Vertex vertex;
    vertex.position = {x, y, z};
    vertex.color = current_color_;
    vertices_.push_back(vertex);
}

inline void EspGL::vertex3fv(const Vector3& v) {
    vertex3f(v[0], v[1], v[2]);
}

namespace detail {

// How far the near plane (z >= -w) and the side planes (-w <= x, y <= w) are
// from a point in clip coordinates. Positive is inside.
constexpr int kClipPlaneCount = 5;
inline float clipDistance(const EspGL::Vector4& p, int plane) {
    switch (plane) {
        case 0: return p[2] + p[3]; // near
        case 1: return p[3] + p[0]; // left
        case 2: return p[3] - p[0]; // right
        case 3: return p[3] + p[1]; // bottom
        default: return p[3] - p[1]; // top
    }
}

// Depth in [0, 1] (NDC, near to far) to the depth buffer's 16 bits, with
// kDepthFraction more bits of fraction (so differences fit an int32_t).
// Beyond the far plane clamps to kDepthMax, so it still draws over a cleared
// buffer (EspGL doesn't clip at the far plane).
constexpr int kDepthFraction = 15;
inline int32_t toDepth16(float z) {
    return static_cast<int32_t>(std::clamp(z, 0.0f, 1.0f) * float(EspGL::kDepthMax << kDepthFraction));
}

template <size_t N>
std::array<float, N> lerp(const std::array<float, N>& a, const std::array<float, N>& b, float t) {
    std::array<float, N> result;
    for (size_t i = 0; i < N; ++i) {
        result[i] = a[i] + (b[i] - a[i]) * t;
    }
    return result;
}

// Lines are drawn this much nearer than they are, so that edges drawn over
// their own faces pass the depth test (like glPolygonOffset).
constexpr float kLineDepthBias = 5e-4f;

} // namespace detail

inline EspGL::ScreenVertex EspGL::toScreen(const ClipVertex& v) const {
    const float inv_w = 1.0f / v.position[3];
    ScreenVertex result;
    result.x = viewport_x_ + (v.position[0] * inv_w + 1.0f) * 0.5f * viewport_width_;
    result.y = viewport_y_ + (1.0f - v.position[1] * inv_w) * 0.5f * viewport_height_;
    result.z = v.position[2] * inv_w * 0.5f + 0.5f;
    result.inv_w = inv_w;
    result.color = v.color;
    return result;
}

inline bool EspGL::project(const Vector3& object, Vector3& window) const {
    const Vector4 clip = transformPoint(mvp(), object);
    if (clip[3] <= 0.0f) {
        return false;
    }
    const ScreenVertex screen = toScreen({clip, {}});
    window = {screen.x, screen.y, screen.z};
    return true;
}

inline void EspGL::drawLine(const Vector3& start, const Vector3& end, const ColorRGBA& color) {
    const Matrix4x4& m = mvp();
    Vector4 p0 = transformPoint(m, start);
    Vector4 p1 = transformPoint(m, end);

    // Clip the segment to the near and side planes (Liang-Barsky in clip
    // coordinates), so nothing behind the camera is projected and the screen
    // coordinates stay within the viewport.
    float t0 = 0.0f, t1 = 1.0f;
    for (int plane = 0; plane < detail::kClipPlaneCount; ++plane) {
        const float d0 = detail::clipDistance(p0, plane);
        const float d1 = detail::clipDistance(p1, plane);
        if (d0 < 0.0f && d1 < 0.0f) {
            return;
        }
        if (d0 < 0.0f) {
            t0 = std::max(t0, d0 / (d0 - d1));
        } else if (d1 < 0.0f) {
            t1 = std::min(t1, d0 / (d0 - d1));
        }
    }
    if (t0 > t1) {
        return;
    }
    const Vector4 c0 = detail::lerp(p0, p1, t0);
    const Vector4 c1 = detail::lerp(p0, p1, t1);

    const ScreenVertex s0 = toScreen({c0, {}});
    const ScreenVertex s1 = toScreen({c1, {}});
    // Pixel (x, y) covers [x, x + 1); keep the viewport's far edges inside it
    auto pixel = [](float v, int lo, int hi) {
        return std::clamp(static_cast<int>(std::floor(v)), lo, hi);
    };
    const int max_x = viewport_x_ + viewport_width_ - 1;
    const int max_y = viewport_y_ + viewport_height_ - 1;
    drawLineBresenham(pixel(s0.x, viewport_x_, max_x), pixel(s0.y, viewport_y_, max_y), s0.z - detail::kLineDepthBias,
                      pixel(s1.x, viewport_x_, max_x), pixel(s1.y, viewport_y_, max_y), s1.z - detail::kLineDepthBias,
                      color);
}

inline void EspGL::drawTriangle(const Vertex& v1, const Vertex& v2, const Vertex& v3) {
    const Matrix4x4& m = mvp();
    auto clip_vertex = [&](const Vertex& v) {
        return ClipVertex{transformPoint(m, v.position),
                          {float(v.color[0]), float(v.color[1]), float(v.color[2]), float(v.color[3])}};
    };
    drawClipTriangle(clip_vertex(v1), clip_vertex(v2), clip_vertex(v3));
}

inline void EspGL::drawIndexedTriangles(const Vertex* vertices, size_t vertex_count,
                                        const uint16_t* indices, size_t index_count) {
    if (!vertices || !indices || vertex_count == 0) return;
    // Transform each vertex once, and project the ones in front of the near plane
    const Matrix4x4& m = mvp();
    clip_scratch_.resize(vertex_count);
    screen_scratch_.resize(vertex_count);
    for (size_t i = 0; i < vertex_count; ++i) {
        const Vertex& v = vertices[i];
        ClipVertex& clip = clip_scratch_[i];
        clip.position = transformPoint(m, v.position);
        clip.color = {float(v.color[0]), float(v.color[1]), float(v.color[2]), float(v.color[3])};
        if (detail::clipDistance(clip.position, 0) >= 0.0f) {
            screen_scratch_[i] = toScreen(clip);
        }
    }

    for (size_t i = 0; i + 2 < index_count; i += 3) {
        const size_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a >= vertex_count || b >= vertex_count || c >= vertex_count) continue;
        const ClipVertex& ca = clip_scratch_[a];
        const ClipVertex& cb = clip_scratch_[b];
        const ClipVertex& cc = clip_scratch_[c];
        if (detail::clipDistance(ca.position, 0) >= 0.0f && detail::clipDistance(cb.position, 0) >= 0.0f &&
            detail::clipDistance(cc.position, 0) >= 0.0f) {
            // In front of the camera: skip it if it's all off one side of the
            // screen, otherwise draw the already-projected vertices
            bool outside = false;
            for (int plane = 1; plane < detail::kClipPlaneCount && !outside; ++plane) {
                outside = detail::clipDistance(ca.position, plane) < 0.0f &&
                          detail::clipDistance(cb.position, plane) < 0.0f &&
                          detail::clipDistance(cc.position, plane) < 0.0f;
            }
            if (!outside) {
                rasterizeTriangle(screen_scratch_[a], screen_scratch_[b], screen_scratch_[c]);
            }
        } else {
            drawClipTriangle(ca, cb, cc);
        }
    }
}

inline void EspGL::drawClipTriangle(const ClipVertex& c1, const ClipVertex& c2, const ClipVertex& c3) {
    const std::array<const ClipVertex*, 3> input = {&c1, &c2, &c3};

    // Nothing to draw if it's all outside one plane (behind the camera, or
    // off one side of the screen)
    for (int plane = 0; plane < detail::kClipPlaneCount; ++plane) {
        if (detail::clipDistance(c1.position, plane) < 0.0f && detail::clipDistance(c2.position, plane) < 0.0f &&
            detail::clipDistance(c3.position, plane) < 0.0f) {
            return;
        }
    }

    // Clip against the near plane (Sutherland-Hodgman): a triangle crossing it
    // becomes one or two triangles in front of the camera. The side planes are
    // handled by the rasterizer's viewport bounds.
    std::array<ClipVertex, 4> polygon;
    size_t count = 0;
    for (size_t i = 0; i < 3; ++i) {
        const ClipVertex& a = *input[i];
        const ClipVertex& b = *input[(i + 1) % 3];
        const float da = detail::clipDistance(a.position, 0);
        const float db = detail::clipDistance(b.position, 0);
        if (da >= 0.0f) {
            polygon[count++] = a;
        }
        if ((da >= 0.0f) != (db >= 0.0f)) {
            const float t = da / (da - db);
            polygon[count++] = {detail::lerp(a.position, b.position, t), detail::lerp(a.color, b.color, t)};
        }
    }
    if (count < 3) {
        return;
    }

    const ScreenVertex s0 = toScreen(polygon[0]);
    ScreenVertex prev = toScreen(polygon[1]);
    for (size_t i = 2; i < count; ++i) {
        const ScreenVertex next = toScreen(polygon[i]);
        rasterizeTriangle(s0, prev, next);
        prev = next;
    }
}

inline void EspGL::drawWireframeCube(float size, const ColorRGBA& color) {
    float half_size = size * 0.5f;
    
    // Define cube vertices
    std::array<Vector3, 8> vertices = {{
        {-half_size, -half_size, -half_size},  // 0
        { half_size, -half_size, -half_size},  // 1
        { half_size,  half_size, -half_size},  // 2
        {-half_size,  half_size, -half_size},  // 3
        {-half_size, -half_size,  half_size},  // 4
        { half_size, -half_size,  half_size},  // 5
        { half_size,  half_size,  half_size},  // 6
        {-half_size,  half_size,  half_size}   // 7
    }};
    
    // Define edges (pairs of vertex indices)
    std::array<std::pair<int, int>, 12> edges = {{
        {0, 1}, {1, 2}, {2, 3}, {3, 0},  // Front face
        {4, 5}, {5, 6}, {6, 7}, {7, 4},  // Back face
        {0, 4}, {1, 5}, {2, 6}, {3, 7}   // Connecting edges
    }};
    

    
    // Draw all edges
    for (const auto& edge : edges) {
        drawLine(vertices[edge.first], vertices[edge.second], color);
    }
}

inline void EspGL::drawSolidCube(float size, const ColorRGBA& color) {
    float half_size = size * 0.5f;
    // Define cube vertices
    std::array<Vector3, 8> vertices = {{
        {-half_size, -half_size, -half_size},  // 0
        { half_size, -half_size, -half_size},  // 1
        { half_size,  half_size, -half_size},  // 2
        {-half_size,  half_size, -half_size},  // 3
        {-half_size, -half_size,  half_size},  // 4
        { half_size, -half_size,  half_size},  // 5
        { half_size,  half_size,  half_size},  // 6
        {-half_size,  half_size,  half_size}   // 7
    }};
    // Assign a unique color to each vertex (rainbow corners)
    std::array<ColorRGBA, 8> vcolors = {{
        {255, 0, 0, 255},    // 0 - Red
        {0, 255, 0, 255},    // 1 - Green
        {0, 0, 255, 255},    // 2 - Blue
        {255, 255, 0, 255},  // 3 - Yellow
        {255, 0, 255, 255},  // 4 - Magenta
        {0, 255, 255, 255},  // 5 - Cyan
        {255, 128, 0, 255},  // 6 - Orange
        {255, 255, 255, 255} // 7 - White
    }};
    // Counterclockwise seen from outside (OpenGL's front faces), so
    // enable(CULL_FACE) drops the far side
    std::array<std::array<int, 3>, 12> faces = {{
        // Front face (z = -half_size)
        {0, 2, 1}, {0, 3, 2},
        // Back face (z = +half_size)
        {5, 7, 4}, {5, 6, 7},
        // Left face (x = -half_size)
        {4, 3, 0}, {4, 7, 3},
        // Right face (x = +half_size)
        {1, 6, 5}, {1, 2, 6},
        // Bottom face (y = -half_size)
        {4, 1, 5}, {4, 0, 1},
        // Top face (y = +half_size)
        {3, 6, 2}, {3, 7, 6}
    }};
    // Draw all faces as triangles with per-vertex colors
    for (const auto& face : faces) {
        if (face.size() == 3) {
            Vertex v1, v2, v3;
            v1.position = vertices[face[0]]; v1.color = vcolors[face[0]];
            v2.position = vertices[face[1]]; v2.color = vcolors[face[1]];
            v3.position = vertices[face[2]]; v3.color = vcolors[face[2]];
            drawTriangle(v1, v2, v3);
        }
    }
}

inline void EspGL::drawSolidCubeWithFaceColors(float size, const std::vector<ColorRGBA>& face_colors) {
    float half_size = size * 0.5f;
    // Define cube vertices
    std::array<Vector3, 8> vertices = {{
        {-half_size, -half_size, -half_size},  // 0
        { half_size, -half_size, -half_size},  // 1
        { half_size,  half_size, -half_size},  // 2
        {-half_size,  half_size, -half_size},  // 3
        {-half_size, -half_size,  half_size},  // 4
        { half_size, -half_size,  half_size},  // 5
        { half_size,  half_size,  half_size},  // 6
        {-half_size,  half_size,  half_size}   // 7
    }};
    // Assign a unique color to each vertex (rainbow corners)
    std::array<ColorRGBA, 8> vcolors = {{
        {255, 0, 0, 255},    // 0 - Red
        {0, 255, 0, 255},    // 1 - Green
        {0, 0, 255, 255},    // 2 - Blue
        {255, 255, 0, 255},  // 3 - Yellow
        {255, 0, 255, 255},  // 4 - Magenta
        {0, 255, 255, 255},  // 5 - Cyan
        {255, 128, 0, 255},  // 6 - Orange
        {255, 255, 255, 255} // 7 - White
    }};
    // Counterclockwise seen from outside (OpenGL's front faces), so
    // enable(CULL_FACE) drops the far side
    std::array<std::array<int, 3>, 12> faces = {{
        // Front face (z = -half_size)
        {0, 2, 1}, {0, 3, 2},
        // Back face (z = +half_size)
        {5, 7, 4}, {5, 6, 7},
        // Left face (x = -half_size)
        {4, 3, 0}, {4, 7, 3},
        // Right face (x = +half_size)
        {1, 6, 5}, {1, 2, 6},
        // Bottom face (y = -half_size)
        {4, 1, 5}, {4, 0, 1},
        // Top face (y = +half_size)
        {3, 6, 2}, {3, 7, 6}
    }};
    // Draw all faces as triangles with per-vertex colors (alpha from face_colors if provided)
    for (size_t i = 0; i < faces.size(); i += 2) {
        ColorRGBA alpha_color = (face_colors.size() > i/2) ? face_colors[i/2] : ColorRGBA{255,255,255,255};
        // First triangle
        if (faces[i].size() == 3) {
            Vertex v1, v2, v3;
            v1.position = vertices[faces[i][0]]; v1.color = vcolors[faces[i][0]]; v1.color[3] = alpha_color[3];
            v2.position = vertices[faces[i][1]]; v2.color = vcolors[faces[i][1]]; v2.color[3] = alpha_color[3];
            v3.position = vertices[faces[i][2]]; v3.color = vcolors[faces[i][2]]; v3.color[3] = alpha_color[3];
            drawTriangle(v1, v2, v3);
        }
        // Second triangle
        if (faces[i+1].size() == 3) {
            Vertex v1, v2, v3;
            v1.position = vertices[faces[i+1][0]]; v1.color = vcolors[faces[i+1][0]]; v1.color[3] = alpha_color[3];
            v2.position = vertices[faces[i+1][1]]; v2.color = vcolors[faces[i+1][1]]; v2.color[3] = alpha_color[3];
            v3.position = vertices[faces[i+1][2]]; v3.color = vcolors[faces[i+1][2]]; v3.color[3] = alpha_color[3];
            drawTriangle(v1, v2, v3);
        }
    }
}

inline void EspGL::drawWireframeIcosahedron(float size, const ColorRGBA& color) {
    const float phi = (1.0f + std::sqrt(5.0f)) / 2.0f;
    float half_size = size * 0.5f;

    std::array<Vector3, 12> vertices = {{
        { 0.0f,  1.0f,  phi}, { 0.0f,  1.0f, -phi}, { 0.0f, -1.0f,  phi}, { 0.0f, -1.0f, -phi},
        { 1.0f,  phi,  0.0f}, { 1.0f, -phi,  0.0f}, {-1.0f,  phi,  0.0f}, {-1.0f, -phi,  0.0f},
        { phi,  0.0f,  1.0f}, { phi,  0.0f, -1.0f},{-phi,  0.0f,  1.0f},{-phi,  0.0f, -1.0f}
    }};

    for (auto& vertex : vertices) {
        vertex[0] *= half_size;
        vertex[1] *= half_size;
        vertex[2] *= half_size;
    }

    // Faces consistent with the current vertex ordering
    const std::array<std::array<int, 3>, 20> faces = {{
        {6,10,0}, {6,0,4}, {6,4,1}, {6,1,11}, {6,11,10},
        {4,0,8}, {0,10,2}, {10,11,7}, {11,1,3}, {1,4,9},
        {5,8,2}, {5,2,7}, {5,7,3}, {5,3,9}, {5,9,8},
        {2,8,0}, {7,2,10}, {3,7,11}, {9,3,1}, {8,9,4}
    }};

    // Each edge is shared by two faces: mark them in a table and draw each
    // once, in (lower, higher) vertex order. A table rather than a std::set:
    // no allocation each call, and ELF apps (Marble Maze) don't get the
    // const-iterator half of std::set's runtime from the firmware.
    bool edges[12][12] = {};
    for (const auto& f : faces) {
        auto add_edge = [&edges](int i, int j) {
            if (i > j) std::swap(i, j);
            edges[i][j] = true;
        };
        add_edge(f[0], f[1]);
        add_edge(f[1], f[2]);
        add_edge(f[2], f[0]);
    }

    for (int i = 0; i < 12; ++i) {
        for (int j = i + 1; j < 12; ++j) {
            if (edges[i][j]) {
                drawLine(vertices[i], vertices[j], color);
            }
        }
    }
}

inline void EspGL::drawSolidIcosahedron(float size, const ColorRGBA& color) {
    const float phi = (1.0f + std::sqrt(5.0f)) / 2.0f; // Golden ratio
    float half_size = size * 0.5f;
    
    // Define icosahedron vertices using golden ratio coordinates
    std::array<Vector3, 12> vertices = {{
        // (0, ±1, ±φ)
        { 0.0f,  1.0f,  phi}, { 0.0f,  1.0f, -phi}, { 0.0f, -1.0f,  phi}, { 0.0f, -1.0f, -phi},
        // (±1, ±φ, 0)
        { 1.0f,  phi,  0.0f}, { 1.0f, -phi,  0.0f}, {-1.0f,  phi,  0.0f}, {-1.0f, -phi,  0.0f},
        // (±φ, 0, ±1)
        { phi,  0.0f,  1.0f}, { phi,  0.0f, -1.0f},{-phi,  0.0f,  1.0f},{-phi,  0.0f, -1.0f}
    }};
    
    // Scale vertices by size
    for (auto& vertex : vertices) {
        vertex[0] *= half_size;
        vertex[1] *= half_size;
        vertex[2] *= half_size;
    }
    
    // Define the 20 triangular faces (each face uses 3 vertex indices)
    const std::array<std::array<int, 3>, 20> faces = {{
        {6,10,0}, {6,0,4}, {6,4,1}, {6,1,11}, {6,11,10},
        {4,0,8}, {0,10,2}, {10,11,7}, {11,1,3}, {1,4,9},
        {5,8,2}, {5,2,7}, {5,7,3}, {5,3,9}, {5,9,8},
        {2,8,0}, {7,2,10}, {3,7,11}, {9,3,1}, {8,9,4}
    }};
    
    // Draw all faces as triangles
    for (const auto& face : faces) {
        Vertex v1, v2, v3;
        v1.position = vertices[face[0]]; v1.color = color;
        v2.position = vertices[face[1]]; v2.color = color;
        v3.position = vertices[face[2]]; v3.color = color;
        drawTriangle(v1, v2, v3);
    }
}

inline void EspGL::drawSolidIcosahedronWithFaceColors(float size, const std::vector<ColorRGBA>& face_colors) {
    const float phi = (1.0f + std::sqrt(5.0f)) / 2.0f; // Golden ratio
    float half_size = size * 0.5f;
    
    // Define icosahedron vertices using golden ratio coordinates
    std::array<Vector3, 12> vertices = {{
        // (0, ±1, ±φ)
        { 0.0f,  1.0f,  phi}, { 0.0f,  1.0f, -phi}, { 0.0f, -1.0f,  phi}, { 0.0f, -1.0f, -phi},
        // (±1, ±φ, 0)
        { 1.0f,  phi,  0.0f}, { 1.0f, -phi,  0.0f}, {-1.0f,  phi,  0.0f}, {-1.0f, -phi,  0.0f},
        // (±φ, 0, ±1)
        { phi,  0.0f,  1.0f}, { phi,  0.0f, -1.0f},{-phi,  0.0f,  1.0f},{-phi,  0.0f, -1.0f}
    }};
    
    // Scale vertices by size
    for (auto& vertex : vertices) {
        vertex[0] *= half_size;
        vertex[1] *= half_size;
        vertex[2] *= half_size;
    }
    
    // Assign unique colors to each vertex for variety
    std::array<ColorRGBA, 12> vcolors = {{
        {255, 0, 0, 255},    // 0 - Red
        {0, 255, 0, 255},    // 1 - Green
        {0, 0, 255, 255},    // 2 - Blue
        {255, 255, 0, 255},  // 3 - Yellow
        {255, 0, 255, 255},  // 4 - Magenta
        {0, 255, 255, 255},  // 5 - Cyan
        {255, 128, 0, 255},  // 6 - Orange
        {128, 255, 0, 255},  // 7 - Lime
        {255, 0, 128, 255},  // 8 - Pink
        {128, 0, 255, 255},  // 9 - Purple
        {0, 128, 255, 255},  // 10 - Sky blue
        {255, 255, 255, 255} // 11 - White
    }};
    
    // Define the 20 triangular faces (each face uses 3 vertex indices)
    const std::array<std::array<int, 3>, 20> faces = {{
        {6,10,0}, {6,0,4}, {6,4,1}, {6,1,11}, {6,11,10},
        {4,0,8}, {0,10,2}, {10,11,7}, {11,1,3}, {1,4,9},
        {5,8,2}, {5,2,7}, {5,7,3}, {5,3,9}, {5,9,8},
        {2,8,0}, {7,2,10}, {3,7,11}, {9,3,1}, {8,9,4}
    }};
    
    // Draw all faces as triangles with face colors
    for (size_t i = 0; i < faces.size(); ++i) {
        const auto& face = faces[i];
        ColorRGBA face_color = (face_colors.size() > i) ? face_colors[i] : ColorRGBA{255,255,255,255};
        Vertex v1, v2, v3;
        v1.position = vertices[face[0]]; v1.color = vcolors[face[0]]; v1.color[3] = face_color[3];
        v2.position = vertices[face[1]]; v2.color = vcolors[face[1]]; v2.color[3] = face_color[3];
        v3.position = vertices[face[2]]; v3.color = vcolors[face[2]]; v3.color[3] = face_color[3];
        drawTriangle(v1, v2, v3);
    }
}

inline void EspGL::swapBuffers() {
    // In software rendering, this is a no-op
    // The frame buffer is already updated
}

inline uint16_t* EspGL::getFrameBuffer() const {
    return pixels_;
}

inline void EspGL::setViewport(uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
    viewport_x_ = x;
    viewport_y_ = y;
    viewport_width_ = width;
    viewport_height_ = height;
}

inline void EspGL::setProjection(float fov, float aspect, float near, float far) {
    projection_matrix_ = createProjectionMatrix(fov, aspect, near, far);
    mvp_dirty_ = true;
}

// Matrix operations (column-major: element (row r, column c) is m[c * 4 + r])
inline EspGL::Matrix4x4 EspGL::createIdentityMatrix() {
    Matrix4x4 matrix = {0};
    matrix[0] = 1.0f;
    matrix[5] = 1.0f;
    matrix[10] = 1.0f;
    matrix[15] = 1.0f;
    return matrix;
}

inline EspGL::Matrix4x4 EspGL::createTranslationMatrix(float x, float y, float z) {
    Matrix4x4 matrix = createIdentityMatrix();
    matrix[12] = x;
    matrix[13] = y;
    matrix[14] = z;
    return matrix;
}

inline EspGL::Matrix4x4 EspGL::createRotationMatrix(float angle, float x, float y, float z) {
    // glRotate: counterclockwise by angle degrees about the axis (x, y, z)
    const float radians = angle * (3.14159265358979f / 180.0f);
    float c = std::cos(radians);
    float s = std::sin(radians);
    float len = std::sqrt(x * x + y * y + z * z);
    
    if (len == 0.0f) return createIdentityMatrix();
    
    x /= len;
    y /= len;
    z /= len;
    
    Matrix4x4 matrix = {0};
    // Column 0
    matrix[0] = x * x * (1 - c) + c;
    matrix[1] = y * x * (1 - c) + z * s;
    matrix[2] = x * z * (1 - c) - y * s;
    // Column 1
    matrix[4] = x * y * (1 - c) - z * s;
    matrix[5] = y * y * (1 - c) + c;
    matrix[6] = y * z * (1 - c) + x * s;
    // Column 2
    matrix[8] = x * z * (1 - c) + y * s;
    matrix[9] = y * z * (1 - c) - x * s;
    matrix[10] = z * z * (1 - c) + c;
    matrix[15] = 1.0f;
    
    return matrix;
}

inline EspGL::Matrix4x4 EspGL::createScaleMatrix(float x, float y, float z) {
    Matrix4x4 matrix = {0};
    matrix[0] = x;
    matrix[5] = y;
    matrix[10] = z;
    matrix[15] = 1.0f;
    return matrix;
}

inline EspGL::Matrix4x4 EspGL::createLookAtMatrix(float eyeX, float eyeY, float eyeZ,
                                           float centerX, float centerY, float centerZ,
                                           float upX, float upY, float upZ) {
    // gluLookAt: the camera at eye, looking down its -z axis toward center
    float forwardX = centerX - eyeX;
    float forwardY = centerY - eyeY;
    float forwardZ = centerZ - eyeZ;
    float forwardLen = std::sqrt(forwardX * forwardX + forwardY * forwardY + forwardZ * forwardZ);
    if (forwardLen == 0.0f) forwardLen = 1.0f;
    forwardX /= forwardLen;
    forwardY /= forwardLen;
    forwardZ /= forwardLen;
    
    // Right vector (camera +x) = forward × up
    float rightX = forwardY * upZ - forwardZ * upY;
    float rightY = forwardZ * upX - forwardX * upZ;
    float rightZ = forwardX * upY - forwardY * upX;
    float rightLen = std::sqrt(rightX * rightX + rightY * rightY + rightZ * rightZ);
    if (rightLen == 0.0f) rightLen = 1.0f;
    rightX /= rightLen;
    rightY /= rightLen;
    rightZ /= rightLen;
    
    // Camera +y = right × forward (already unit length)
    float upVecX = rightY * forwardZ - rightZ * forwardY;
    float upVecY = rightZ * forwardX - rightX * forwardZ;
    float upVecZ = rightX * forwardY - rightY * forwardX;
    
    // Rows are the camera axes (right, up, -forward); column-major storage
    Matrix4x4 matrix = {0};
    matrix[0] = rightX;
    matrix[4] = rightY;
    matrix[8] = rightZ;
    matrix[1] = upVecX;
    matrix[5] = upVecY;
    matrix[9] = upVecZ;
    matrix[2] = -forwardX;
    matrix[6] = -forwardY;
    matrix[10] = -forwardZ;
    matrix[15] = 1.0f;
    
    // ...after moving the eye to the origin
    return multiplyMatrices(matrix, createTranslationMatrix(-eyeX, -eyeY, -eyeZ));
}

inline EspGL::Matrix4x4 EspGL::createProjectionMatrix(float fov, float aspect, float near, float far) {
    // gluPerspective. Maps eye-space z = -near to NDC -1 and z = -far to +1,
    // and puts -z (the distance in front of the camera) in w.
    float f = 1.0f / std::tan(fov / 2.0f);
    float range = near - far;
    
    Matrix4x4 matrix = {0};
    matrix[0] = f / aspect;
    matrix[5] = f;
    matrix[10] = (far + near) / range;
    matrix[11] = -1.0f;
    matrix[14] = (2.0f * far * near) / range;
    
    return matrix;
}

inline EspGL::Matrix4x4 EspGL::multiplyMatrices(const Matrix4x4& a, const Matrix4x4& b) {
    Matrix4x4 result = {0};
    
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            result[col * 4 + row] = sum;
        }
    }
    
    return result;
}

inline EspGL::Vector4 EspGL::transformPoint(const Matrix4x4& m, const Vector3& v) {
    Vector4 result;
    for (int row = 0; row < 4; ++row) {
        result[row] = m[row] * v[0] + m[4 + row] * v[1] + m[8 + row] * v[2] + m[12 + row];
    }
    return result;
}

// Rasterization
inline void EspGL::drawLineBresenham(int x0, int y0, float z0, int x1, int y1, float z1, const ColorRGBA& color) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    
    int x = x0;
    int y = y0;
    // Depth is linear in screen space; step it along the longer axis
    const int steps = std::max(dx, dy);
    int32_t z = detail::toDepth16(z0);
    const int32_t dz = steps > 0 ? (detail::toDepth16(z1) - z) / steps : 0;
    const bool depth = depth_test_enabled_ && depth_buffer_;

    while (true) {
        // Lines are depth tested but don't write depth, so they don't hide
        // anything drawn after them.
        if (!depth || (z >> detail::kDepthFraction) < depth_buffer_[y * width_ + x]) {
            setPixel(x, y, color);
        }
        
        if (x == x1 && y == y1) break;
        
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x += sx;
        }
        if (e2 < dx) {
            err += dx;
            y += sy;
        }
        z += dz;
    }
}

inline void EspGL::drawTriangleBarycentric(int x1, int y1, int x2, int y2, int x3, int y3, const ColorRGBA& c1, const ColorRGBA& c2, const ColorRGBA& c3) {
    // A flat screen-space triangle (pixel corners, all at depth 0)
    auto vertex = [](int x, int y, const ColorRGBA& c) {
        return ScreenVertex{float(x), float(y), 0.0f, 1.0f,
                            {float(c[0]), float(c[1]), float(c[2]), float(c[3])}};
    };
    rasterizeTriangle(vertex(x1, y1, c1), vertex(x2, y2, c2), vertex(x3, y3, c3));
}

namespace detail {

inline uint16_t toRgb565(uint32_t r, uint32_t g, uint32_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// r, g, b with alpha a (all 0-255) over the RGB565 pixel dst
inline uint16_t blendRgb565(uint32_t r, uint32_t g, uint32_t b, uint32_t a, uint16_t dst) {
    const uint32_t ia = 255 - a;
    const uint32_t dr = (dst >> 8) & 0xF8;
    const uint32_t dg = (dst >> 3) & 0xFC;
    const uint32_t db = (dst << 3) & 0xF8;
    return toRgb565((r * a + dr * ia) / 255, (g * a + dg * ia) / 255, (b * a + db * ia) / 255);
}

// Colors in the span loops are 16.16 fixed point, 0-255 in the integer part
constexpr int kColorShift = 16;

// Perspective-correct color takes a divide, so it's computed exactly only at
// the ends of runs of pixels along a span and stepped linearly in between (as
// Quake did for texture coordinates). A run is as long as 1/w changes by at
// most kPerspectiveTolerance of itself, up to kPerspectiveRun pixels: the
// color is then off by at most (tolerance / 2)^2 of its range, about one
// level in 255, below what RGB565 shows. Spans that recede steeply get short
// runs, down to a divide per pixel.
constexpr int kPerspectiveRun = 16;
constexpr float kPerspectiveTolerance = 0.125f;

// Fills n pixels of a span starting at dst (and depth, its depth buffer
// entries). z is fixed-point depth (toDepth16) stepping by dzdx. A Gouraud
// span steps its colors c by dc; a flat one writes `flat`, or blends c when
// blending.
template <bool kDepth, bool kGouraud, bool kBlend>
inline void fillSpan(uint16_t* dst, uint16_t* depth, int n, int32_t& z, int32_t dzdx, int32_t (&c)[4],
                     const int32_t (&dc)[4], uint16_t flat) {
    for (int i = 0; i < n; ++i) {
        const uint16_t zi = static_cast<uint16_t>(z >> kDepthFraction);
        if (!kDepth || zi < depth[i]) {
            if (kDepth) {
                depth[i] = zi;
            }
            if (kBlend) {
                dst[i] = blendRgb565(c[0] >> kColorShift, c[1] >> kColorShift, c[2] >> kColorShift,
                                     c[3] >> kColorShift, dst[i]);
            } else if (kGouraud) {
                dst[i] = toRgb565(c[0] >> kColorShift, c[1] >> kColorShift, c[2] >> kColorShift);
            } else {
                dst[i] = flat;
            }
        }
        if (kDepth) {
            z += dzdx;
        }
        if (kGouraud) {
            c[0] += dc[0];
            c[1] += dc[1];
            c[2] += dc[2];
            c[3] += dc[3];
        }
    }
}

using SpanFill = void (*)(uint16_t*, uint16_t*, int, int32_t&, int32_t, int32_t (&)[4],
                          const int32_t (&)[4], uint16_t);
// Indexed by depth * 4 + gouraud * 2 + blend
inline constexpr SpanFill kSpanFills[8] = {
    fillSpan<false, false, false>, fillSpan<false, false, true>, fillSpan<false, true, false>,
    fillSpan<false, true, true>,   fillSpan<true, false, false>, fillSpan<true, false, true>,
    fillSpan<true, true, false>,   fillSpan<true, true, true>,
};

} // namespace detail

// A scanline rasterizer: walks the triangle's edges row by row, so it only
// visits pixels inside it, and steps depth and color along each span. Pixel
// centers on a left or top edge are drawn and those on a right or bottom edge
// aren't, so triangles that share an edge never both draw a pixel of it.
inline void EspGL::rasterizeTriangle(const ScreenVertex& v1, const ScreenVertex& v2, const ScreenVertex& v3) {
    if (!pixels_) return;

    // Twice the signed area: positive when the triangle is counterclockwise
    // as seen on screen (y up), OpenGL's default front face
    const float area = (v3.x - v1.x) * (v2.y - v1.y) - (v3.y - v1.y) * (v2.x - v1.x);
    if (area == 0.0f || !std::isfinite(area)) return;
    if (cull_enabled_) {
        const bool front = (area > 0.0f) == (front_face_ != CW);
        if (cull_mode_ == FRONT_AND_BACK || (cull_mode_ == BACK) != front) {
            ++stats_.culled;
            return;
        }
    }

    // Draw within the viewport and the frame buffer
    const int clip_x0 = viewport_x_;
    const int clip_y0 = viewport_y_;
    const int clip_x1 = std::min<int>(viewport_x_ + viewport_width_, width_);
    const int clip_y1 = std::min<int>(viewport_y_ + viewport_height_, height_);
    if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1) return;
    ++stats_.triangles;

    // Plane equations for the attributes: a(x, y) = a1 + dadx * (x - x1) + dady * (y - y1)
    const float x21 = v2.x - v1.x, y21 = v2.y - v1.y;
    const float x31 = v3.x - v1.x, y31 = v3.y - v1.y;
    const float inv_d = -1.0f / area; // the triangle's only divide besides the edge slopes
    auto gradient = [&](float a1, float a2, float a3, float& ddx, float& ddy) {
        const float d2 = a2 - a1, d3 = a3 - a1;
        ddx = (d2 * y31 - d3 * y21) * inv_d;
        ddy = (d3 * x21 - d2 * x31) * inv_d;
    };
    float dzdx, dzdy;
    gradient(v1.z, v2.z, v3.z, dzdx, dzdy);

    const bool depth = depth_test_enabled_ && depth_buffer_;
    const bool blend = blend_enabled_ &&
                       std::min({v1.color[3], v2.color[3], v3.color[3]}) < 254.5f;
    const bool gouraud = !(v1.color == v2.color && v1.color == v3.color);
    const int channels = blend ? 4 : 3;
    // Colors are linear in eye space, so in screen space c/w and 1/w are
    // linear and c is their quotient. Skip that when 1/w hardly changes
    // across the triangle (it faces the camera, or it's 2D).
    const float q_min = std::min({v1.inv_w, v2.inv_w, v3.inv_w});
    const float q_max = std::max({v1.inv_w, v2.inv_w, v3.inv_w});
    const bool perspective = gouraud && (q_max - q_min) > q_max * (1.0f / 1024.0f);

    float q1 = 1.0f, dqdx = 0.0f, dqdy = 0.0f, run_scale = 0.0f;
    float c1[4] = {}, dcdx[4] = {}, dcdy[4] = {};
    int32_t flat_c[4] = {};
    uint16_t flat = 0;
    if (gouraud) {
        if (perspective) {
            q1 = v1.inv_w;
            gradient(v1.inv_w, v2.inv_w, v3.inv_w, dqdx, dqdy);
            // Pixels per run per unit of 1/w (see kPerspectiveTolerance)
            run_scale = dqdx != 0.0f ? detail::kPerspectiveTolerance / std::abs(dqdx) : 1e30f;
        }
        for (int k = 0; k < channels; ++k) {
            const float a = v1.color[k] * (perspective ? v1.inv_w : 1.0f);
            const float b = v2.color[k] * (perspective ? v2.inv_w : 1.0f);
            const float c = v3.color[k] * (perspective ? v3.inv_w : 1.0f);
            c1[k] = a;
            gradient(a, b, c, dcdx[k], dcdy[k]);
        }
    } else {
        for (int k = 0; k < 4; ++k) {
            const uint32_t v = static_cast<uint32_t>(std::clamp(v1.color[k] + 0.5f, 0.0f, 255.0f));
            flat_c[k] = static_cast<int32_t>(v << detail::kColorShift);
        }
        flat = detail::toRgb565(flat_c[0] >> detail::kColorShift, flat_c[1] >> detail::kColorShift,
                                flat_c[2] >> detail::kColorShift);
    }
    const detail::SpanFill fill = detail::kSpanFills[(depth ? 4 : 0) + (gouraud ? 2 : 0) + (blend ? 1 : 0)];

    // Sort the vertices top to bottom. The long edge a-c spans every row; the
    // short edges a-b and b-c are on the other side.
    const ScreenVertex* a = &v1;
    const ScreenVertex* b = &v2;
    const ScreenVertex* c = &v3;
    if (b->y < a->y) std::swap(a, b);
    if (c->y < a->y) std::swap(a, c);
    if (c->y < b->y) std::swap(b, c);
    const float slope_ac = (c->x - a->x) / (c->y - a->y); // c->y > a->y, as the area isn't 0
    const float slope_ab = b->y > a->y ? (b->x - a->x) / (b->y - a->y) : 0.0f;
    const float slope_bc = c->y > b->y ? (c->x - b->x) / (c->y - b->y) : 0.0f;
    const bool long_on_left = b->x > a->x + (b->y - a->y) * slope_ac;

    // The first pixel whose center is at or past v, within [lo, hi]
    auto first_pixel = [](float v, int lo, int hi) {
        return static_cast<int>(std::clamp(std::ceil(v - 0.5f), static_cast<float>(lo), static_cast<float>(hi)));
    };
    const int y_start = first_pixel(a->y, clip_y0, clip_y1);
    const int y_end = first_pixel(c->y, clip_y0, clip_y1);
    static constexpr int32_t kNoStep[4] = {};

    // What this triangle writes to the depth buffer, for clearDepth()
    int dirty_x0 = clip_x1, dirty_x1 = clip_x0, dirty_y0 = clip_y1, dirty_y1 = clip_y0;
    for (int y = y_start; y < y_end; ++y) {
        const float py = y + 0.5f;
        // Each edge's x is computed from its top vertex, the same way in every
        // triangle that shares it, so shared edges meet exactly
        const float x_long = a->x + (py - a->y) * slope_ac;
        const float x_short = py < b->y ? a->x + (py - a->y) * slope_ab : b->x + (py - b->y) * slope_bc;
        const int x_start = first_pixel(long_on_left ? x_long : x_short, clip_x0, clip_x1);
        const int x_end = first_pixel(long_on_left ? x_short : x_long, clip_x0, clip_x1);
        const int n = x_end - x_start;
        if (n <= 0) continue;
        stats_.fragments += n;
        if (depth) {
            dirty_x0 = std::min(dirty_x0, x_start);
            dirty_x1 = std::max(dirty_x1, x_end);
            dirty_y0 = std::min(dirty_y0, y);
            dirty_y1 = y + 1;
        }

        const size_t offset = static_cast<size_t>(y) * width_ + x_start;
        uint16_t* dst = pixels_ + offset;
        uint16_t* zbuf = depth ? depth_buffer_.get() + offset : nullptr;
        // Attributes relative to v1, at the first pixel's center
        const float rel_y = py - v1.y;
        const float rel_x = x_start + 0.5f - v1.x;
        // Depth in fixed point, from its (clamped) values at the span's ends
        int32_t z = 0, dz = 0;
        if (depth) {
            const float z_row = v1.z + dzdy * rel_y;
            z = detail::toDepth16(z_row + dzdx * rel_x);
            if (n > 1) {
                dz = (detail::toDepth16(z_row + dzdx * (rel_x + n - 1)) - z) / (n - 1);
            }
        }

        if (!gouraud) {
            fill(dst, zbuf, n, z, dz, flat_c, kNoStep, flat);
            continue;
        }

        // This row's values at x = v1.x
        const float q_row = q1 + dqdy * rel_y;
        float c_row[4];
        for (int k = 0; k < 4; ++k) {
            c_row[k] = c1[k] + dcdy[k] * rel_y;
        }
        // The color i pixels into the span, in fixed point
        auto color_at = [&](int i, int32_t (&out)[4]) {
            const float x = rel_x + i;
            float scale = 1.0f;
            if (perspective) {
                scale = 1.0f / std::max(q_row + dqdx * x, 1e-20f);
            }
            for (int k = 0; k < 4; ++k) {
                const float v = k < channels ? std::clamp((c_row[k] + dcdx[k] * x) * scale, 0.0f, 255.0f) : 255.0f;
                out[k] = static_cast<int32_t>(v * float(1 << detail::kColorShift) + float(1 << (detail::kColorShift - 1)));
            }
        };

        // In runs: exact colors at the ends of each run, linear in between.
        // Without perspective the whole span is one run.
        int32_t color[4], next[4], step[4];
        color_at(0, color);
        for (int i = 0; i < n;) {
            int run = n;
            if (perspective) {
                const float q = q_row + dqdx * (rel_x + i);
                const float fit = std::min(q * run_scale, float(detail::kPerspectiveRun));
                run = std::clamp(static_cast<int>(fit), 1, n - i);
            }
            // Step toward the first pixel of the next run, or the last pixel
            // of this one at the end of the span
            const int steps = i + run < n ? run : run - 1;
            if (steps > 0) {
                color_at(i + steps, next);
                for (int k = 0; k < 4; ++k) {
                    step[k] = (next[k] - color[k]) / steps;
                }
            } else {
                std::copy(color, color + 4, next);
                std::fill(step, step + 4, 0);
            }
            fill(dst + i, zbuf ? zbuf + i : nullptr, run, z, dz, color, step, 0);
            std::copy(next, next + 4, color);
            i += run;
        }
    }

    if (dirty_x0 < dirty_x1) {
        if (depth_dirty_x0_ >= depth_dirty_x1_) {
            depth_dirty_x0_ = dirty_x0;
            depth_dirty_x1_ = dirty_x1;
            depth_dirty_y0_ = dirty_y0;
            depth_dirty_y1_ = dirty_y1;
        } else {
            depth_dirty_x0_ = std::min(depth_dirty_x0_, dirty_x0);
            depth_dirty_x1_ = std::max(depth_dirty_x1_, dirty_x1);
            depth_dirty_y0_ = std::min(depth_dirty_y0_, dirty_y0);
            depth_dirty_y1_ = std::max(depth_dirty_y1_, dirty_y1);
        }
    }
}

inline void EspGL::enableBlending(bool enable) {
    blend_enabled_ = enable;
}

inline void EspGL::enableDepthTest(bool enable) {
    depth_test_enabled_ = enable;
}

inline void EspGL::setBlendFunc(uint32_t src, uint32_t dst) {
    blend_src_factor_ = src;
    blend_dst_factor_ = dst;
}

// Helper function to blend two colors
inline EspGL::ColorRGBA EspGL::blendColors(const ColorRGBA& src, const ColorRGBA& dst) {
    float src_alpha = src[3] / 255.0f;
    float dst_alpha = dst[3] / 255.0f;
    
    ColorRGBA result;
    for (int i = 0; i < 3; ++i) {
        result[i] = static_cast<uint8_t>(src[i] * src_alpha + dst[i] * (1.0f - src_alpha));
    }
    result[3] = static_cast<uint8_t>((src_alpha + dst_alpha * (1.0f - src_alpha)) * 255.0f);
    
    return result;
}

inline void EspGL::setPixel(int x, int y, const ColorRGBA& color) {
    if (!pixels_ || x < 0 || x >= static_cast<int>(width_) || y < 0 || y >= static_cast<int>(height_)) {
        return;
    }
    
    if (blend_enabled_ && color[3] < 255) {
        // Get existing pixel color
        uint16_t existing_rgb565 = pixels_[y * width_ + x];
        ColorRGBA existing_color = {
            static_cast<uint8_t>((existing_rgb565 >> 8) & 0xF8),
            static_cast<uint8_t>((existing_rgb565 >> 3) & 0xFC),
            static_cast<uint8_t>((existing_rgb565 << 3) & 0xF8),
            255
        };
        
        // Blend colors
        ColorRGBA blended = blendColors(color, existing_color);
        uint16_t rgb565 = colorToRGB565(blended);
        pixels_[y * width_ + x] = rgb565;
    } else {
        uint16_t rgb565 = colorToRGB565(color);
        pixels_[y * width_ + x] = rgb565;
    }
}

inline uint16_t EspGL::colorToRGB565(const ColorRGBA& color) const {
    uint8_t r = color[0];
    uint8_t g = color[1];
    uint8_t b = color[2];
    
    uint16_t rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return rgb565;
}

inline bool EspGL::isPointInViewport(int x, int y) const {
    return x >= 0 && x < static_cast<int>(width_) && 
           y >= 0 && y < static_cast<int>(height_);
}

inline void EspGL::clearDepth() {
    // Only the part drawn since the last clear: clearing is mostly PSRAM
    // bandwidth, and a model in the middle of the screen covers little of it
    if (!depth_buffer_ || depth_dirty_x0_ >= depth_dirty_x1_ || depth_dirty_y0_ >= depth_dirty_y1_) {
        return;
    }
    const int w = depth_dirty_x1_ - depth_dirty_x0_;
    uint16_t* row = depth_buffer_.get() + size_t(depth_dirty_y0_) * width_ + depth_dirty_x0_;
    if (w == width_) {
        detail::fill16(row, size_t(w) * (depth_dirty_y1_ - depth_dirty_y0_), 0xFFFF);
    } else {
        for (int y = depth_dirty_y0_; y < depth_dirty_y1_; ++y, row += width_) {
            detail::fill16(row, w, 0xFFFF);
        }
    }
    depth_dirty_x0_ = depth_dirty_y0_ = depth_dirty_x1_ = depth_dirty_y1_ = 0;
}

} // namespace espgl 