#pragma once

// EspGL's implementation, inline so the library stays header-only.
// Included from espgl.hpp; include that instead.

#include "espgl.hpp"



namespace espgl {

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
    depth_buffer_ = std::make_unique<float[]>(width * height);

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
    return (current_matrix_mode_ == MODELVIEW) ? modelview_matrix_ : projection_matrix_;
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
        uint16_t clear_color_rgb565 = colorToRGB565(clear_color_);
        std::fill(pixels_, pixels_ + width_ * height_, clear_color_rgb565);
    }
    if (mask & DEPTH_BUFFER_BIT) {
        clearDepth();
    }
}

inline void EspGL::enable(uint32_t cap) {
    // For now, just a placeholder
}

inline void EspGL::disable(uint32_t cap) {
    // For now, just a placeholder
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
    const Vector4 clip = transformPoint(multiplyMatrices(projection_matrix_, modelview_matrix_), object);
    if (clip[3] <= 0.0f) {
        return false;
    }
    const ScreenVertex screen = toScreen({clip, {}});
    window = {screen.x, screen.y, screen.z};
    return true;
}

inline void EspGL::drawLine(const Vector3& start, const Vector3& end, const ColorRGBA& color) {
    const Matrix4x4 mvp = multiplyMatrices(projection_matrix_, modelview_matrix_);
    Vector4 p0 = transformPoint(mvp, start);
    Vector4 p1 = transformPoint(mvp, end);

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
    const Matrix4x4 mvp = multiplyMatrices(projection_matrix_, modelview_matrix_);
    auto clip_vertex = [&](const Vertex& v) {
        return ClipVertex{transformPoint(mvp, v.position),
                          {float(v.color[0]), float(v.color[1]), float(v.color[2]), float(v.color[3])}};
    };

    // Clip against the near plane (Sutherland-Hodgman): a triangle crossing it
    // becomes one or two triangles in front of the camera. The side planes are
    // handled by the rasterizer's viewport bounds.
    std::array<ClipVertex, 4> polygon;
    size_t count = 0;
    const std::array<ClipVertex, 3> input = {clip_vertex(v1), clip_vertex(v2), clip_vertex(v3)};
    for (size_t i = 0; i < 3; ++i) {
        const ClipVertex& a = input[i];
        const ClipVertex& b = input[(i + 1) % 3];
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
    std::vector<Vector3> vertices = {
        {-half_size, -half_size, -half_size},  // 0
        { half_size, -half_size, -half_size},  // 1
        { half_size,  half_size, -half_size},  // 2
        {-half_size,  half_size, -half_size},  // 3
        {-half_size, -half_size,  half_size},  // 4
        { half_size, -half_size,  half_size},  // 5
        { half_size,  half_size,  half_size},  // 6
        {-half_size,  half_size,  half_size}   // 7
    };
    
    // Define edges (pairs of vertex indices)
    std::vector<std::pair<int, int>> edges = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0},  // Front face
        {4, 5}, {5, 6}, {6, 7}, {7, 4},  // Back face
        {0, 4}, {1, 5}, {2, 6}, {3, 7}   // Connecting edges
    };
    

    
    // Draw all edges
    for (const auto& edge : edges) {
        drawLine(vertices[edge.first], vertices[edge.second], color);
    }
}

inline void EspGL::drawSolidCube(float size, const ColorRGBA& color) {
    float half_size = size * 0.5f;
    // Define cube vertices
    std::vector<Vector3> vertices = {
        {-half_size, -half_size, -half_size},  // 0
        { half_size, -half_size, -half_size},  // 1
        { half_size,  half_size, -half_size},  // 2
        {-half_size,  half_size, -half_size},  // 3
        {-half_size, -half_size,  half_size},  // 4
        { half_size, -half_size,  half_size},  // 5
        { half_size,  half_size,  half_size},  // 6
        {-half_size,  half_size,  half_size}   // 7
    };
    // Assign a unique color to each vertex (rainbow corners)
    std::vector<ColorRGBA> vcolors = {
        {255, 0, 0, 255},    // 0 - Red
        {0, 255, 0, 255},    // 1 - Green
        {0, 0, 255, 255},    // 2 - Blue
        {255, 255, 0, 255},  // 3 - Yellow
        {255, 0, 255, 255},  // 4 - Magenta
        {0, 255, 255, 255},  // 5 - Cyan
        {255, 128, 0, 255},  // 6 - Orange
        {255, 255, 255, 255} // 7 - White
    };
    // Consistent CCW winding for all faces
    std::vector<std::vector<int>> faces = {
        // Front face (z = -half_size)
        {0, 1, 2}, {0, 2, 3},
        // Back face (z = +half_size)
        {5, 4, 7}, {5, 7, 6},
        // Left face (x = -half_size)
        {4, 0, 3}, {4, 3, 7},
        // Right face (x = +half_size)
        {1, 5, 6}, {1, 6, 2},
        // Bottom face (y = -half_size)
        {4, 5, 1}, {4, 1, 0},
        // Top face (y = +half_size)
        {3, 2, 6}, {3, 6, 7}
    };
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
    std::vector<Vector3> vertices = {
        {-half_size, -half_size, -half_size},  // 0
        { half_size, -half_size, -half_size},  // 1
        { half_size,  half_size, -half_size},  // 2
        {-half_size,  half_size, -half_size},  // 3
        {-half_size, -half_size,  half_size},  // 4
        { half_size, -half_size,  half_size},  // 5
        { half_size,  half_size,  half_size},  // 6
        {-half_size,  half_size,  half_size}   // 7
    };
    // Assign a unique color to each vertex (rainbow corners)
    std::vector<ColorRGBA> vcolors = {
        {255, 0, 0, 255},    // 0 - Red
        {0, 255, 0, 255},    // 1 - Green
        {0, 0, 255, 255},    // 2 - Blue
        {255, 255, 0, 255},  // 3 - Yellow
        {255, 0, 255, 255},  // 4 - Magenta
        {0, 255, 255, 255},  // 5 - Cyan
        {255, 128, 0, 255},  // 6 - Orange
        {255, 255, 255, 255} // 7 - White
    };
    // Consistent CCW winding for all faces
    std::vector<std::vector<int>> faces = {
        // Front face (z = -half_size)
        {0, 1, 2}, {0, 2, 3},
        // Back face (z = +half_size)
        {5, 4, 7}, {5, 7, 6},
        // Left face (x = -half_size)
        {4, 0, 3}, {4, 3, 7},
        // Right face (x = +half_size)
        {1, 5, 6}, {1, 6, 2},
        // Bottom face (y = -half_size)
        {4, 5, 1}, {4, 1, 0},
        // Top face (y = +half_size)
        {3, 2, 6}, {3, 6, 7}
    };
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

    std::vector<Vector3> vertices = {
        { 0.0f,  1.0f,  phi}, { 0.0f,  1.0f, -phi}, { 0.0f, -1.0f,  phi}, { 0.0f, -1.0f, -phi},
        { 1.0f,  phi,  0.0f}, { 1.0f, -phi,  0.0f}, {-1.0f,  phi,  0.0f}, {-1.0f, -phi,  0.0f},
        { phi,  0.0f,  1.0f}, { phi,  0.0f, -1.0f},{-phi,  0.0f,  1.0f},{-phi,  0.0f, -1.0f}
    };

    for (auto& vertex : vertices) {
        vertex[0] *= half_size;
        vertex[1] *= half_size;
        vertex[2] *= half_size;
    }

    // Faces consistent with the current vertex ordering
    const std::vector<std::array<int,3>> faces = {
        {6,10,0}, {6,0,4}, {6,4,1}, {6,1,11}, {6,11,10},
        {4,0,8}, {0,10,2}, {10,11,7}, {11,1,3}, {1,4,9},
        {5,8,2}, {5,2,7}, {5,7,3}, {5,3,9}, {5,9,8},
        {2,8,0}, {7,2,10}, {3,7,11}, {9,3,1}, {8,9,4}
    };

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
    std::vector<Vector3> vertices = {
        // (0, ±1, ±φ)
        { 0.0f,  1.0f,  phi}, { 0.0f,  1.0f, -phi}, { 0.0f, -1.0f,  phi}, { 0.0f, -1.0f, -phi},
        // (±1, ±φ, 0)
        { 1.0f,  phi,  0.0f}, { 1.0f, -phi,  0.0f}, {-1.0f,  phi,  0.0f}, {-1.0f, -phi,  0.0f},
        // (±φ, 0, ±1)
        { phi,  0.0f,  1.0f}, { phi,  0.0f, -1.0f},{-phi,  0.0f,  1.0f},{-phi,  0.0f, -1.0f}
    };
    
    // Scale vertices by size
    for (auto& vertex : vertices) {
        vertex[0] *= half_size;
        vertex[1] *= half_size;
        vertex[2] *= half_size;
    }
    
    // Define the 20 triangular faces (each face uses 3 vertex indices)
    const std::vector<std::array<int,3>> faces = {
        {6,10,0}, {6,0,4}, {6,4,1}, {6,1,11}, {6,11,10},
        {4,0,8}, {0,10,2}, {10,11,7}, {11,1,3}, {1,4,9},
        {5,8,2}, {5,2,7}, {5,7,3}, {5,3,9}, {5,9,8},
        {2,8,0}, {7,2,10}, {3,7,11}, {9,3,1}, {8,9,4}
    };
    
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
    std::vector<Vector3> vertices = {
        // (0, ±1, ±φ)
        { 0.0f,  1.0f,  phi}, { 0.0f,  1.0f, -phi}, { 0.0f, -1.0f,  phi}, { 0.0f, -1.0f, -phi},
        // (±1, ±φ, 0)
        { 1.0f,  phi,  0.0f}, { 1.0f, -phi,  0.0f}, {-1.0f,  phi,  0.0f}, {-1.0f, -phi,  0.0f},
        // (±φ, 0, ±1)
        { phi,  0.0f,  1.0f}, { phi,  0.0f, -1.0f},{-phi,  0.0f,  1.0f},{-phi,  0.0f, -1.0f}
    };
    
    // Scale vertices by size
    for (auto& vertex : vertices) {
        vertex[0] *= half_size;
        vertex[1] *= half_size;
        vertex[2] *= half_size;
    }
    
    // Assign unique colors to each vertex for variety
    std::vector<ColorRGBA> vcolors = {
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
    };
    
    // Define the 20 triangular faces (each face uses 3 vertex indices)
    const std::vector<std::array<int,3>> faces = {
        {6,10,0}, {6,0,4}, {6,4,1}, {6,1,11}, {6,11,10},
        {4,0,8}, {0,10,2}, {10,11,7}, {11,1,3}, {1,4,9},
        {5,8,2}, {5,2,7}, {5,7,3}, {5,3,9}, {5,9,8},
        {2,8,0}, {7,2,10}, {3,7,11}, {9,3,1}, {8,9,4}
    };
    
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
    const float dz = steps > 0 ? (z1 - z0) / steps : 0.0f;
    float z = z0;
    
    while (true) {
        // Lines are depth tested but don't write depth, so they don't hide
        // anything drawn after them.
        if (!depth_test_enabled_ || z < depth_buffer_[y * width_ + x]) {
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

inline void EspGL::rasterizeTriangle(const ScreenVertex& v1, const ScreenVertex& v2, const ScreenVertex& v3) {
    // Twice the signed area; either winding is drawn (no face culling)
    auto edge = [](float ax, float ay, float bx, float by, float px, float py) {
        return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
    };
    const float area = edge(v1.x, v1.y, v2.x, v2.y, v3.x, v3.y);
    if (area == 0.0f || !std::isfinite(area)) return;
    const float inv_area = 1.0f / area;
    
    // Bounding box of the pixel centers inside the triangle, within the viewport
    const float vp_min_x = viewport_x_;
    const float vp_min_y = viewport_y_;
    const float vp_max_x = viewport_x_ + viewport_width_ - 1;
    const float vp_max_y = viewport_y_ + viewport_height_ - 1;
    const int minX = static_cast<int>(std::clamp(std::ceil(std::min({v1.x, v2.x, v3.x}) - 0.5f), vp_min_x, vp_max_x + 1));
    const int maxX = static_cast<int>(std::clamp(std::floor(std::max({v1.x, v2.x, v3.x}) - 0.5f), vp_min_x - 1, vp_max_x));
    const int minY = static_cast<int>(std::clamp(std::ceil(std::min({v1.y, v2.y, v3.y}) - 0.5f), vp_min_y, vp_max_y + 1));
    const int maxY = static_cast<int>(std::clamp(std::floor(std::max({v1.y, v2.y, v3.y}) - 0.5f), vp_min_y - 1, vp_max_y));
    
    for (int y = minY; y <= maxY; ++y) {
        const float py = y + 0.5f;
        for (int x = minX; x <= maxX; ++x) {
            const float px = x + 0.5f;
            // Barycentric weights of the pixel center; all >= 0 inside
            const float alpha = edge(v2.x, v2.y, v3.x, v3.y, px, py) * inv_area;
            const float beta  = edge(v3.x, v3.y, v1.x, v1.y, px, py) * inv_area;
            const float gamma = edge(v1.x, v1.y, v2.x, v2.y, px, py) * inv_area;
            if (alpha < 0.0f || beta < 0.0f || gamma < 0.0f) {
                continue;
            }
            
            // NDC depth is linear in screen space
            const float z = alpha * v1.z + beta * v2.z + gamma * v3.z;
            const int idx = y * width_ + x;
            if (depth_test_enabled_ && !(z < depth_buffer_[idx])) {
                continue;
            }
            
            // Colors are linear in eye space: interpolate c/w and 1/w
            const float w1 = alpha * v1.inv_w;
            const float w2 = beta * v2.inv_w;
            const float w3 = gamma * v3.inv_w;
            const float inv_sum = 1.0f / (w1 + w2 + w3);
            ColorRGBA color;
            for (int i = 0; i < 4; ++i) {
                const float c = (w1 * v1.color[i] + w2 * v2.color[i] + w3 * v3.color[i]) * inv_sum;
                color[i] = static_cast<uint8_t>(std::clamp(c + 0.5f, 0.0f, 255.0f));
            }
            setPixel(x, y, color);
            if (depth_test_enabled_) {
                depth_buffer_[idx] = z;
            }
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
    if (depth_buffer_) {
        std::fill(depth_buffer_.get(), depth_buffer_.get() + width_ * height_, std::numeric_limits<float>::infinity());
    }
}

} // namespace espgl 