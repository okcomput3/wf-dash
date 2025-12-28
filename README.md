# Wayfire Shader Dock

A beautiful dock plugin for Wayfire that renders application icons with custom shader effects including bevel, shimmer, and 3D button styling.

![Shader Dock Preview](preview.png)

## Features

- **Shader-based icon rendering** - Custom GLSL shaders provide:
  - 3D button/bevel effect with raised center
  - Directional lighting simulation
  - Animated shimmer sweep effect
  - Smooth hover animations with bounce
  - Gradient border on dock background

- **Desktop integration**
  - Reads .desktop files for application information
  - Automatic icon discovery from theme directories
  - Click-to-launch functionality

- **Configurable**
  - Customizable icon size, spacing, and margins
  - Adjustable corner radius
  - Configurable bevel and background colors
  - Choose which apps appear in the dock

## Requirements

- Wayfire 0.9.0 or later (tested with 0.11)
- wlroots 0.17.0 or later
- libpng
- OpenGL ES 3.0+
- meson & ninja build system

## Building

```bash
# Clone or extract the plugin
cd wayfire-shader-dock

# Configure build
meson setup build --prefix=/usr

# Build
ninja -C build

# Install (may require sudo)
sudo ninja -C build install
```

## Configuration

Add the following to your `~/.config/wayfire.ini`:

```ini
[shader-dock]
# Space-separated list of application IDs (desktop file names without .desktop)
apps = firefox thunar kitty code chromium

# Icon size in pixels (default: 64)
icon_size = 64

# Spacing between icons (default: 8)
spacing = 8

# Margin around dock (default: 8)
margin = 8

# Corner radius for rounded icons (default: 12.0)
corner_radius = 12

# Bevel/shimmer color (RGBA, 0.0-1.0)
bevel_color = 0.8 0.7 0.5 0.6

# Dock background color (RGBA, 0.0-1.0)
background_color = 0.1 0.1 0.1 0.85
```

Then add `shader-dock` to your plugins list:

```ini
[core]
plugins = ... shader-dock
```

## Finding Application IDs

Application IDs are the names of .desktop files without the `.desktop` extension.
Common locations for .desktop files:

- `/usr/share/applications/`
- `/usr/local/share/applications/`
- `~/.local/share/applications/`

Examples:
- Firefox: `firefox`
- Thunar: `thunar`
- Kitty: `kitty`
- VS Code: `code` or `visual-studio-code`
- Chrome: `google-chrome`

## Icon Theme Support

The dock searches for icons in these theme directories:
- `/usr/share/icons/hicolor`
- `/usr/share/icons/Adwaita`
- `/usr/share/icons/breeze`
- `/usr/share/icons/Papirus`
- `/usr/share/pixmaps`

Supported formats: PNG (SVG support planned)

## Troubleshooting

### Icons not appearing
1. Check that the application ID is correct (matches .desktop filename)
2. Verify the icon exists in your theme directories
3. Check Wayfire logs for error messages:
   ```bash
   wayfire 2>&1 | grep shader-dock
   ```

### Dock not visible
1. Ensure `shader-dock` is in your plugins list
2. Check that the configuration section `[shader-dock]` exists
3. Verify at least one valid app is specified

### Click not working
- The dock currently uses basic pointer signal handling
- Ensure no other fullscreen/overlay is capturing input

## Architecture

The plugin consists of:

1. **DockNode** - Scene graph node handling:
   - Icon texture management
   - Geometry calculations
   - Hit testing for mouse interaction

2. **DockRenderInstance** - Rendering:
   - Background shader (animated gradient border)
   - Icon shader (bevel/shimmer/3D effect)
   - Hover state animation

3. **ShaderDockPlugin** - Main plugin:
   - Configuration management
   - Input signal handling
   - Animation timer

## Shader Details

### Icon Shader Features
- Signed distance function for rounded rectangles
- Gaussian blur for soft glow effect
- Per-pixel lighting with configurable light direction
- Time-based shimmer animation
- Hover-responsive bounce effect

### Background Shader Features
- Rounded rectangle with anti-aliasing
- Animated HSV gradient border
- Configurable transparency

## License

MIT License - See LICENSE file

## Credits

Shader techniques inspired by various Shadertoy effects.
Built for Wayfire compositor - https://wayfire.org
