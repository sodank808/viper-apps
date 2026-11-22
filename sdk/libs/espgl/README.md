# EspGL - ESP32-S3 Software Rendering Engine

Status: implemented; current as of 2026-11-07

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

- `begin(type)`: Start immediate mode drawing
- `end()`: End immediate mode drawing
- `vertex3f(x, y, z)`: Add vertex
- `drawLine(start, end, color)`: Draw line between points
- `drawWireframeCube(size, color)`: Draw wireframe cube

### Frame Buffer

- `getFrameBuffer()`: Get pointer to the buffer drawing goes to
- `setFrameBuffer(pixels)`: Draw into `pixels` (width x height RGB565) instead; `nullptr` goes back to EspGL's own
- `swapBuffers()`: Swap buffers (no-op in software mode)
- `getWidth()`: Get frame buffer width
- `getHeight()`: Get frame buffer height

## Performance Considerations

- Software rendering is CPU-intensive
- Frame buffer operations are memory-intensive
- Consider using smaller viewports for better performance

## Limitations

- No texture support
- No face culling (both windings are drawn)
- Limited primitive types (no quads or polygons in immediate mode)
- No lighting or shading

## Future Enhancements

- Texture mapping
- Lighting and shading
- More primitive types
- Optimized rasterization algorithms 