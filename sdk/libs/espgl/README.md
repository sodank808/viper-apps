# EspGL - ESP32-S3 Software Rendering Engine

Status: implemented; current as of 2026-11-24

EspGL is a software-based rendering engine designed to provide OpenGL-like functionality on the ESP32-S3. It supports basic 3D graphics operations including matrix transformations, wireframe rendering, and immediate mode drawing.

It's header-only and platform-free (no ESP-IDF, LVGL or `System`): `espgl.hpp`
declares it and includes `espgl_impl.hpp`, where the code is, all `inline`. So
built-in apps (Marble Maze, the EspGL demo), ELF apps and the host simulator
compile the same code. An ELF app asks for it in its `CMakeLists.txt`, before
`project_elf()`, as with the other shared libraries (`apps/README.md`):

```cmake
set(VIPER_LIBS espgl gfx)
```

`apps/rubik` is the example: it draws into the frame `viper_frame_begin()`
returns (see "Drawing into someone else's buffer" below).

## Features

- **OpenGL-like API**: Familiar matrix operations and immediate mode drawing
- **Software Rendering**: Pure software implementation for maximum compatibility
- **Wireframe Rendering**: Support for lines, line strips, and line loops
- **Filled triangles**: flat or Gouraud (perspective-correct) color, depth
  test, alpha blending, back-face culling, and indexed meshes
- **Matrix Stack**: Push/pop matrix operations for complex transformations
- **Viewport Management**: Flexible viewport and projection setup

## Conventions

EspGL follows OpenGL: matrices are stored column-major (element row r, column c
is `m[c * 4 + r]`, translation in `m[12..14]`) and transform column vectors.
`translate`/`rotate`/`scale`/`lookAt` multiply the current matrix on the right,
so the call nearest the draw is applied to the vertices first. `lookAt` and
`setProjection` build the same matrices as `gluLookAt` and `gluPerspective`; the
camera looks down -z. Vertices go through the perspective divide, are clipped
against the near plane, and are depth tested with NDC depth when depth testing
is enabled. `sim/tests/test_espgl.cpp` checks all of this.

## Usage

### Basic Setup

```cpp
#include "espgl.hpp"

// Initialize EspGL
auto gl = std::make_unique<espgl::EspGL>();
gl->init(width, height);

// Set up projection (vertical field of view in radians)
gl->setProjection(45.0f * M_PI / 180.0f, aspect_ratio, 0.1f, 100.0f);
gl->setViewport(0, 0, width, height);
```

### Drawing a Wireframe Cube

```cpp
// Clear the buffer
gl->clearColor(0.0f, 0.0f, 0.0f, 1.0f);
gl->clear(espgl::EspGL::COLOR_BUFFER_BIT);

// Set up transformations
gl->matrixMode(espgl::EspGL::MODELVIEW);
gl->loadIdentity();
gl->translate(0.0f, 0.0f, -5.0f);
gl->rotate(angle, 1.0f, 0.0f, 0.0f);

// Set color and draw
gl->color3f(1.0f, 1.0f, 1.0f);
gl->drawWireframeCube(2.0f, {255, 255, 255, 255});
```

### Drawing into someone else's buffer

`init(width, height)` allocates a color buffer and a depth buffer. To draw
straight into a buffer you already have, such as an ELF app's frame, skip the
color buffer and hand yours over each frame (the frame pointer can change from
frame to frame):

```cpp
gl->init(width, height, false);         // depth buffer only

uint16_t *pixels = viper_frame_begin(); // in loop()
gl->setFrameBuffer(pixels);
// ... clear and draw ...
gl->setFrameBuffer(nullptr);            // don't keep it past viper_frame_end()
viper_frame_end(1);
```

### Meshes

A mesh drawn with `drawIndexedTriangles()` transforms each vertex once, however
many triangles share it, and skips triangles wholly off one side of the screen
before rasterizing them, so it's much faster than a `drawTriangle()` per face:

```cpp
std::vector<espgl::EspGL::Vertex> vertices; // position, normal (unused), color
std::vector<uint16_t> indices;              // three per triangle
gl->enable(espgl::EspGL::CULL_FACE);        // skip faces turned away
gl->drawIndexedTriangles(vertices.data(), vertices.size(), indices.data(), indices.size());
```

Wind front faces counterclockwise as the camera sees them (OpenGL's default;
`frontFace(CW)` flips it). With `CULL_FACE` enabled, back faces of closed
meshes are dropped before rasterizing, about half the triangles. The built-in
solid cube and icosahedron are wound this way.

### Immediate Mode Drawing

```cpp
// Draw lines using immediate mode
gl->begin(espgl::EspGL::LINES);
gl->vertex3f(-1.0f, -1.0f, 0.0f);
gl->vertex3f(1.0f, 1.0f, 0.0f);
gl->end();
```

## API Reference

### Matrix Operations

- `matrixMode(mode)`: Set current matrix mode (MODELVIEW, PROJECTION, TEXTURE)
- `loadIdentity()`: Load identity matrix
- `translate(x, y, z)`: Apply translation
- `rotate(angle, x, y, z)`: Apply rotation around axis
- `scale(x, y, z)`: Apply scaling
- `pushMatrix()`: Push current matrix onto stack
- `popMatrix()`: Pop matrix from stack

### Rendering State

- `clearColor(r, g, b, a)`: Set clear color
- `clear(mask)`: Clear buffers
- `color3f(r, g, b)`: Set current color
- `color4f(r, g, b, a)`: Set current color with alpha
- `lineWidth(width)`: Set line width

### Drawing Primitives

- `enable(cap)` / `disable(cap)`: `DEPTH_TEST`, `BLEND`, `CULL_FACE`
- `cullFace(mode)`: `BACK` (default), `FRONT` or `FRONT_AND_BACK`
- `frontFace(mode)`: `CCW` (default) or `CW`
- `stats()` / `resetStats()`: triangles rasterized and culled, and pixels covered

- `begin(type)`: Start immediate mode drawing
- `end()`: End immediate mode drawing
- `vertex3f(x, y, z)`: Add vertex
- `drawLine(start, end, color)`: Draw line between points
- `drawTriangle(v1, v2, v3)`: Draw one filled triangle
- `drawIndexedTriangles(vertices, count, indices, count)`: Draw a mesh
- `drawWireframeCube(size, color)`: Draw wireframe cube

### Frame Buffer

- `getFrameBuffer()`: Get pointer to the buffer drawing goes to
- `setFrameBuffer(pixels)`: Draw into `pixels` (width x height RGB565) instead; `nullptr` goes back to EspGL's own
- `swapBuffers()`: Swap buffers (no-op in software mode)
- `getWidth()`: Get frame buffer width
- `getHeight()`: Get frame buffer height

## How triangles are drawn

`drawTriangle()` and meshes transform vertices by projection * modelview
(rebuilt only when a matrix changes), clip against the near plane, and hand
screen-space triangles to `rasterizeTriangle()`, a scanline rasterizer:

- It walks the triangle's edges row by row and fills only the pixels inside,
  instead of testing every pixel of its bounding box. Pixel centers on a left
  or top edge are drawn and those on a right or bottom edge aren't, so meshes
  draw each pixel exactly once (no double-blended seams, no gaps).
- Depth and color step along each span. A flat-colored triangle writes one
  precomputed RGB565 value. A Gouraud one steps 16.16 fixed-point colors.
- Perspective-correct color needs a divide, so it's done only at the ends of
  runs of up to 16 pixels and stepped linearly in between; a run is as long as
  1/w changes by at most 1/8, which keeps the color within about one level of
  exact (`kPerspectiveTolerance`). Triangles whose 1/w hardly changes (facing
  the camera, or 2D) skip it. Setting up a triangle takes four or five
  divides; pixels take one only at the ends of runs.
- Pixels go straight into the buffer (no per-pixel bounds checks), clipped to
  the viewport and the frame buffer once per span.
- Depth is 16 bits (`getDepthBuffer()`, 0 at the near plane to `kDepthMax` at
  the far one), stepped in fixed point. That's half the memory traffic of a
  float buffer, and with a near plane of 0.1 and a far one of 100 it still
  tells apart surfaces 5 cm apart 10 units away. Nothing is clipped at the far
  plane; beyond it, depth is `kDepthMax`. Where a closed mesh's back and front faces
  meet at an edge, they can round to the same depth, and the one drawn first
  wins a few pixels; `CULL_FACE` avoids that (and is faster).

`sim/tests/test_espgl.cpp` checks it against a reference rasterizer, that
meshes cover every pixel once, that color is perspective-correct, and culling,
viewports, blending, and that wild input (huge, degenerate, NaN) stays inside
the buffers.

## Test scenes and timing on a device

The EspGL demo app (`apps/espgl_app`) has test scenes that lean on what apps
would draw (`apps/espgl_app/main/scenes.hpp`): a lit terrain to the horizon,
64 spinning cubes, a tunnel whose walls cross the near plane, translucent
panels blended over the whole screen, and a 1536-triangle sphere lit on the CPU
each frame. X steps through them and Select turns culling off and on. Once a
second the app logs the frame rate, the draw time and what was drawn:

```
EspGLApp: terrain FPS=... frame=...ms draw=...ms tris=1177 culled=398 frags=55678 blend=OFF cull=ON
```

Read it on a device with `tools/harness/viperctl logs` (launch the app and press
X with `viperctl`). On the host, `./build-sim/tests/viper_sim_app_tests
"[espgl-bench]"` times the same scenes, only useful to compare two builds.

## Performance Considerations

- Mesh with `drawIndexedTriangles()` and enable `CULL_FACE` for closed models
- Flat-colored triangles are the cheapest; Gouraud costs a few adds a pixel,
  more where a surface recedes steeply
- Blending reads the frame buffer, so it costs about twice as much a pixel
- Clearing is a good part of a frame: the color and depth buffers are
  2 bytes a pixel each (300 KB at 320x240, in PSRAM). `clear()` writes whole
  words, and is fastest when the clear color's two RGB565 bytes match (black,
  white): then it's a `memset`. Skip `DEPTH_BUFFER_BIT` when nothing is
  depth tested

## Limitations

- No texture support
- Limited primitive types (no quads or polygons in immediate mode)
- No lighting (light vertices yourself, as the demo's sphere does)

## Future Enhancements

- Texture mapping
- Lighting and shading
- More primitive types
