# ResizerC

A high-performance Windows C GUI-based batch image processing tool designed for modern workflows with maximum speed and I/O throughput efficiency.

## Features

- **Drag-and-drop batch input** - Drop individual files or entire folders (recursively scanned)
- **Format conversion** - Convert between JPEG, PNG, WebP, BMP, TIFF, and ICO formats
- **Image resizing** - Resize by percentage or select resolution
- **Quality control** - Adjustable quality settings for lossy formats
- **Multithreaded processing** - User-selectable thread count for parallel processing
- **Performance metrics** - Real-time display of files/second, MB/sec throughput, compression ratio
- **Folder structure preservation** - Option to maintain original folder hierarchy
- **EXIF/timestamp preservation** - Maintains file modification times
- **Settings persistence** - Remembers your preferences between sessions
- **Update checks** - Checks GitHub Releases for newer versions

## Building

### Prerequisites

You need the following libraries installed:

- **CMake** 3.20 or later
- **C compiler** with C11 support (MSVC, GCC, or Clang)
- **zlib** (required)
- Optional image format libraries:
  - **libjpeg-turbo** - for JPEG support
  - **libpng** - for PNG support
  - **libwebp** - for WebP support
  - **libtiff** - for TIFF support

### Installing Dependencies (Windows)

#### Using vcpkg (recommended)

```bash
vcpkg install --triplet x64-windows-static
```

#### Or manually:
- Download and build each library from source
- Or use pre-built binaries from each project's website

### Build Instructions

1. Clone or extract the source code
2. Create a build directory:

```bash
mkdir build
cd build
```

3. Configure with CMake:

```bash
cmake .. -DCMAKE_TOOLCHAIN_FILE=[path to vcpkg]/scripts/buildsystems/vcpkg.cmake -DRESIZERC_GITHUB_OWNER=your-user -DRESIZERC_GITHUB_REPO=ResizerC
```

Or without vcpkg:
```bash
cmake ..
```

4. Build:

```bash
cmake --build . --config Release
```

5. The executable will be in `build/bin/Release/ResizerC.exe`

Or use the local script:

```bash
.\build.ps1 -NoIncrement
```

## GitHub Releases

This repo includes `.github/workflows/release.yml`. Push a version tag and GitHub Actions will build `ResizerC.exe` and upload it to that release.

```bash
git tag v1.0.1
git push origin v1.0.1
```

Or let the release script bump/build/commit/tag/push:

```bash
.\release.ps1 -Version 1.0.1
```

The app version is taken from the tag during the release build. `Help -> Check for updates` reads the latest GitHub Release and opens it when a newer tag exists.

For local release builds, pass the same version and repo settings:

```bash
cmake -S . -B build -DRESIZERC_VERSION=1.0.1 -DRESIZERC_GITHUB_OWNER=your-user -DRESIZERC_GITHUB_REPO=ResizerC
cmake --build build --config Release
```

### Using Visual Studio

1. Open CMakeLists.txt in Visual Studio 2022 or later
2. Select the desired configuration (Release/Debug)
3. Build → Build All

## Usage

### Basic Workflow

1. **Launch ResizerC** - The application will load your previous settings
2. **Add files** - Drag and drop files or folders onto the drop target area
3. **Configure settings**:
   - Select output directory (click Browse)
   - Choose output format (WebP recommended for best compression)
   - Adjust quality slider (1-100, or use "Optimized" checkbox for 85%)
   - Set resolution percentage (100% = original size)
   - Configure thread count (default = CPU logical cores)
4. **Process** - Click "Process Images" to begin conversion

### Optimized Mode

Check the "Optimized" checkbox for automatic settings that provide:
- 85% quality (subjectively <1% visual loss)
- Maximum compression for selected format
- Best balance between file size and quality

### Supported Formats

**Input:** JPEG, PNG, WebP, BMP, TIFF, GIF, ICO

**Output:** JPEG, PNG, WebP, BMP, TIFF

**Note:** GIF is read-only (converts to other formats)

### Performance Tips

- Use WebP output for best compression with minimal quality loss
- Set thread count to match your CPU's logical core count
- For batch processing large folders, SSDs significantly improve throughput
- Quality 85-90 provides excellent compression with minimal visual loss

## Project Structure

```
ResizerC/
├── CMakeLists.txt          # Build configuration
├── README.md               # This file
├── include/                # Header files
│   ├── common.h           # Shared definitions and AppState
│   ├── window.h           # Window management
│   ├── dragdrop.h         # Drag-and-drop handling
│   ├── image.h            # Image loading/saving
│   ├── processing.h       # Multithreaded processing
│   ├── settings.h         # Settings persistence
│   ├── update.h           # GitHub release update checks
│   ├── utils.h            # Utility functions
│   └── resource.h         # Resource IDs
└── src/                   # Source files
    ├── main.c             # Application entry point
    ├── window.c           # Window and UI implementation
    ├── dragdrop.c         # Drag-drop and folder scanning
    ├── image.c            # Image processing (load/save/resize)
    ├── processing.c       # Worker threads and processing logic
    ├── settings.c         # Settings load/save
    ├── update.c           # GitHub release update checks
    └── utils.c            # Utility functions
```

## Technical Details

### Image Processing

- **BMP loader** - Built-in Windows BMP support
- **JPEG** - Uses libjpeg-turbo for loading and saving
- **PNG** - Uses libpng with configurable compression
- **WebP** - Uses libwebp with lossless mode at 100% quality
- **TIFF** - Uses libtiff for full format support

### Threading Model

- Thread pool processes files from a shared queue
- Atomic index ensures no file is processed twice
- Critical sections protect shared statistics
- Progress updates via window messages

### Memory Management

- Images loaded and processed one at a time per thread
- Memory usage scales with thread count and image dimensions
- No memory leaks (verified with proper cleanup on all paths)

## License

[Your license here]

## Contributing

[Your contribution guidelines here]

## Changelog

### Version 1.0.0
- Initial release
- Support for JPEG, PNG, WebP, BMP, TIFF, ICO formats
- Drag-and-drop files and folders
- Multithreaded processing
- Settings persistence
- GitHub Release update checks
