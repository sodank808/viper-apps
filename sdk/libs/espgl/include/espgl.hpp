#pragma once

// EspGL: OpenGL-style software 3D rendering into an RGB565 buffer.
//
// Header-only and platform-free (no ESP-IDF, LVGL or System), so built-in
// apps, ELF apps (set(VIPER_LIBS espgl) before project_elf()) and the host
// simulator compile the same code. The implementation is in espgl_impl.hpp,
// included at the end of this file.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <vector>

namespace espgl {

/**
 * @brief OpenGL-like API for ESP32-S3 software rendering
 */
class EspGL {
public:
    // Matrix modes
    enum MatrixMode {
        MODELVIEW,
        PROJECTION,
        TEXTURE
    };
    
    // Primitive types
    enum PrimitiveType {
        POINTS,
        LINES,
        LINE_STRIP,
        LINE_LOOP,
        TRIANGLES,
        TRIANGLE_STRIP,
        TRIANGLE_FAN,
        QUADS,
        QUAD_STRIP,
        POLYGON
    };
    
    // Clear masks
    static constexpr uint32_t COLOR_BUFFER_BIT = 0x00004000;
    static constexpr uint32_t DEPTH_BUFFER_BIT = 0x00000100;
    static constexpr uint32_t STENCIL_BUFFER_BIT = 0x00000400;
    
    // Capabilities
    static constexpr uint32_t DEPTH_TEST = 0x0B71;
    static constexpr uint32_t BLEND = 0x0BE2;
    static constexpr uint32_t CULL_FACE = 0x0B44;
    static constexpr uint32_t LIGHTING = 0x0B50;
    static constexpr uint32_t TEXTURE_2D = 0x0DE1;

    // Blend factors
    static constexpr uint32_t ZERO = 0;
    static constexpr uint32_t ONE = 1;
    static constexpr uint32_t SRC_COLOR = 0x0300;
    static constexpr uint32_t ONE_MINUS_SRC_COLOR = 0x0301;
    static constexpr uint32_t SRC_ALPHA = 0x0302;
    static constexpr uint32_t ONE_MINUS_SRC_ALPHA = 0x0303;
    static constexpr uint32_t DST_ALPHA = 0x0304;
    static constexpr uint32_t ONE_MINUS_DST_ALPHA = 0x0305;

    // Matrix types. Matrices are column-major like OpenGL: the element in row
    // r, column c is m[c * 4 + r], and they transform column vectors
    // (v' = M * v), so the translation is in m[12], m[13], m[14].
    using Matrix4x4 = std::array<float, 16>;
    using Vector3 = std::array<float, 3>;
    using Vector4 = std::array<float, 4>;
    
    // Color types
    using ColorRGB = std::array<uint8_t, 3>;
    using ColorRGBA = std::array<uint8_t, 4>;
    
    // Vertex types
    struct Vertex {
        Vector3 position;
        Vector3 normal;
        ColorRGBA color;
    };
    
    // Line types
    struct Line {
        Vector3 start;
        Vector3 end;
        ColorRGBA color;
    };

    EspGL();
    ~EspGL();
    
    // Initialization. Allocates the depth buffer, and the color buffer unless
    // own_frame_buffer is false; then draw into a buffer you hand over with
    // setFrameBuffer() (e.g. an ELF app's viper_frame_begin()), saving a copy.
    void init(uint16_t width, uint16_t height, bool own_frame_buffer = true);
    void deinit();
    /// Draw into `pixels` (width x height RGB565, rows not padded) from now
    /// on, or back into EspGL's own buffer with nullptr. Drawing without
    /// either does nothing.
    void setFrameBuffer(uint16_t* pixels);
    
    // Matrix operations
    void matrixMode(MatrixMode mode);
    void loadIdentity();
    void loadMatrix(const Matrix4x4& matrix);
    void multMatrix(const Matrix4x4& matrix);
    void translate(float x, float y, float z);
    void rotate(float angle, float x, float y, float z);
    void scale(float x, float y, float z);
    void lookAt(float eyeX, float eyeY, float eyeZ,
                float centerX, float centerY, float centerZ,
                float upX, float upY, float upZ);
    void pushMatrix();
    void popMatrix();
    Matrix4x4 getMatrix() const;

    // Matrix builders (the same matrices as the OpenGL/GLU calls of the same
    // name). Angles are in degrees, except the perspective field of view.
    static Matrix4x4 createIdentityMatrix();
    static Matrix4x4 createTranslationMatrix(float x, float y, float z);
    static Matrix4x4 createRotationMatrix(float angle, float x, float y, float z);
    static Matrix4x4 createScaleMatrix(float x, float y, float z);
    static Matrix4x4 createLookAtMatrix(float eyeX, float eyeY, float eyeZ,
                                        float centerX, float centerY, float centerZ,
                                        float upX, float upY, float upZ);
    /// gluPerspective, with the vertical field of view in radians
    static Matrix4x4 createProjectionMatrix(float fov, float aspect, float near, float far);
    /// a * b (applies b first, then a)
    static Matrix4x4 multiplyMatrices(const Matrix4x4& a, const Matrix4x4& b);
    /// m * (v, 1), without the perspective divide
    static Vector4 transformPoint(const Matrix4x4& m, const Vector3& v);

    /// Like gluProject: object coordinates through the modelview, projection,
    /// and viewport to window coordinates (pixels, y down) and depth (0 at the
    /// near plane, 1 at the far plane). False if the point is behind the camera.
    bool project(const Vector3& object, Vector3& window) const;
    
    // Rendering state
    void clearColor(float r, float g, float b, float a = 1.0f);
    void clear(uint32_t mask);
    void enable(uint32_t cap);
    void disable(uint32_t cap);
    void lineWidth(float width);
    void color3f(float r, float g, float b);
    void color4f(float r, float g, float b, float a);
    
    // Drawing primitives
    void begin(PrimitiveType type);
    void end();
    void vertex3f(float x, float y, float z);
    void vertex3fv(const Vector3& v);
    
    // Immediate mode drawing
    void drawLine(const Vector3& start, const Vector3& end, const ColorRGBA& color);
    void drawTriangle(const Vertex& v1, const Vertex& v2, const Vertex& v3);
    void drawTriangleBarycentric(int x1, int y1, int x2, int y2, int x3, int y3, const ColorRGBA& c1, const ColorRGBA& c2, const ColorRGBA& c3);
    void drawWireframeCube(float size, const ColorRGBA& color);
    void drawSolidCube(float size, const ColorRGBA& color);
    void drawSolidCubeWithFaceColors(float size, const std::vector<ColorRGBA>& face_colors);
    void drawWireframeIcosahedron(float size, const ColorRGBA& color);
    void drawSolidIcosahedron(float size, const ColorRGBA& color);
    void drawSolidIcosahedronWithFaceColors(float size, const std::vector<ColorRGBA>& face_colors);
    
    // Frame buffer operations
    void swapBuffers();
    uint16_t* getFrameBuffer() const;
    uint16_t getWidth() const { return width_; }
    uint16_t getHeight() const { return height_; }
    
    // Utility functions
    void setViewport(uint16_t x, uint16_t y, uint16_t width, uint16_t height);
    /// Replaces the projection matrix with gluPerspective(fov, aspect, near, far).
    /// fov is the vertical field of view in radians.
    void setProjection(float fov, float aspect, float near, float far);
    void setPixel(int x, int y, const ColorRGBA& color);

    // Depth buffer control
    void enableDepthTest(bool enable);
    void clearDepth();
    float* getDepthBuffer() const { return depth_buffer_.get(); }
    
    // Blending control
    void enableBlending(bool enable);
    void setBlendFunc(uint32_t src, uint32_t dst);

private:
    // Matrix stacks (one per mode, as in OpenGL)
    std::vector<Matrix4x4> modelview_stack_;
    std::vector<Matrix4x4> projection_stack_;
    MatrixMode current_matrix_mode_;
    Matrix4x4 modelview_matrix_;
    Matrix4x4 projection_matrix_;
    
    // Rendering state
    uint16_t width_;
    uint16_t height_;
    std::unique_ptr<uint16_t[]> own_pixels_; ///< the color buffer init() allocated, if any
    uint16_t* pixels_ = nullptr;             ///< where drawing goes: own_pixels_ or setFrameBuffer()'s
    ColorRGBA clear_color_;
    float line_width_;
    ColorRGBA current_color_;
    
    // Viewport
    uint16_t viewport_x_;
    uint16_t viewport_y_;
    uint16_t viewport_width_;
    uint16_t viewport_height_;
    
    // Immediate mode state
    PrimitiveType current_primitive_;
    std::vector<Vertex> vertices_;
    bool immediate_mode_active_;
    
    // Hardware acceleration flag
    bool hw_accel_enabled_;
    
    // Depth buffer
    std::unique_ptr<float[]> depth_buffer_; ///< Z-buffer for depth testing
    bool depth_test_enabled_; ///< True if depth test is enabled
    
    // Blending state
    bool blend_enabled_; ///< True if blending is enabled
    uint32_t blend_src_factor_; ///< Source blend factor
    uint32_t blend_dst_factor_; ///< Destination blend factor
    
    Matrix4x4& currentMatrix();
    std::vector<Matrix4x4>& currentStack();

    // A vertex after the projection: clip coordinates and its color
    struct ClipVertex {
        Vector4 position;
        std::array<float, 4> color;
    };
    // ...and after the perspective divide and viewport transform
    struct ScreenVertex {
        float x, y;        // pixels
        float z;           // depth, 0 (near) to 1 (far)
        float inv_w;       // 1/w, for perspective-correct color
        std::array<float, 4> color;
    };
    ScreenVertex toScreen(const ClipVertex& v) const;
    void rasterizeTriangle(const ScreenVertex& v1, const ScreenVertex& v2, const ScreenVertex& v3);

    // Rasterization
    void drawLineBresenham(int x0, int y0, float z0, int x1, int y1, float z1, const ColorRGBA& color);
    uint16_t colorToRGB565(const ColorRGBA& color) const;
    ColorRGBA blendColors(const ColorRGBA& src, const ColorRGBA& dst);
    
    // Clipping
    bool isPointInViewport(int x, int y) const;
};

} // namespace espgl

#include "espgl_impl.hpp" 