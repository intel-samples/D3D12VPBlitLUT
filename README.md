# D3D12 Agility SDK Lut Compound Sample Application

## Overview

This sample application demonstrates how to use the **Microsoft Direct3D 12 Agility SDK** to perform **video processing with 1D and 3D LUT (Look-Up Table) transforms**.

The sample:

* Loads raw video frames from a file (P010 format)
* Applies **CSC (Color Space Conversion)**, **1D LUT**, and **3D LUT**
* Outputs processed frames in **NV12 format**
* Uses the **D3D12 Video Processing pipeline** with Agility SDK runtime loading

This project is intended as a **reference implementation** for developers integrating advanced color processing using the D3D12 Agility SDK.

---

## Prerequisites

* Windows 10/11 with a **D3D12-compatible GPU**
* Visual Studio (2019 or later recommended)
* Graphics driver supporting **D3D12 Video Processing + LUT**
* **Microsoft Direct3D 12 Agility SDK**

### Agility SDK Dependency

This project depends on the **D3D12 Agility SDK 1.719** 

Please add the Agility SDK NuGet package and configure the dependency according to the official Microsoft guidance:

> Refer to the Microsoft NuGet integration example for Direct3D 12 Agility SDK.

*(This README does not include detailed NuGet setup steps.)*

At runtime, the application loads the required DLLs from:

```
<Executable Directory>\D3D12\
    d3d12core.dll
    d3d12sdklayers.dll
```

---

## Features

* Direct3D 12 Video Processing
* Hardware capability validation
* 1D LUT support (RGBA16)
* 3D LUT support with trilinear interpolation
* Supported 3D LUT sizes:

  * 17 × 17 × 17
  * 33 × 33 × 33
  * 45 × 45 × 45
  * 65 × 65 × 65
* Batch processing of multiple frames

---

## Input / Output

### Input

| Item         | Description                               |
| ------------ | ----------------------------------------- |
| Video format | Raw **P010** frames, user need to use it's own RAW content if want to run this sample app                       |
| Resolution   | **1920 × 1080**                           |
| Color space  | Custom / HDR (example BT.2020 processing) |
| File type    | Binary raw frame sequence                 |
| 1D LUT       | Binary file, RGBA16 format, example provides a 1DLut for H2S                |
| 3D LUT       | Binary file, RGBA16 format, example provides a 3DLut for H2S                |

Frame size calculation (P010):

```
FrameSize = Width × Height × 2 (Y)
          + Width × Height      (UV)
```

---

### Output

| Item       | Description                   |
| ---------- | ----------------------------- |
| Format     | **NV12**                      |
| Resolution | 1920 × 1080                   |
| File type  | Binary                        |
| Naming     | `<output_base>_frame_XXX.bin` |

If no output base is provided, default:

```
output_1920x1080_frame_000.bin
```

---

## Command Line Usage

```
SampleApp.exe 
    -i <input_file>
    -lut1d <1d_lut_file>
    -lut3d <3d_lut_file>
    -lut1dsize <size>
    -lut3dsize <size>
    [-o <output_base>]
```

### Example

```
SampleApp.exe ^
    -i input_1080p_p010.bin ^
    -lut1d lut1d.bin ^
    -lut3d lut3d_33.bin ^
    -lut1dsize 1024 ^
    -lut3dsize 65 ^
    -o output/result
```

---

## Processing Pipeline

1. Initialize Agility SDK runtime DLLs
2. Create D3D12 device (highest supported feature level)
3. Validate hardware video processing capabilities
4. Upload:

   * Input frame
   * 1D LUT texture
   * 3D LUT texture
5. Configure:

   * CSC matrix
   * LUT transform configuration
6. Execute:

   * Copy → Video Process → Readback
7. Save processed frame to disk

LUT and CSC are initialized on the **first frame** and reused for subsequent frames.

---

## Known Issues

* **Fixed Resolution Only**
  Currently supports only:

  ```
  1920 × 1080
  ```

  Dynamic resolution support will be added in a future release.

* **Fixed Input/Output Formats**

  * Input: P010
  * Output: NV12

  more formats support need based on real usage, here is only for example.

---

## Future Improvements

Planned for next version:

* Dynamic resolution support
* Additional input/output formats

---

## Hardware Requirements

The GPU must support:

* D3D12 Video Processing
* 1D LUT
* 3D LUT (with supported dimensions)

If unsupported, the application will exit with a capability error.

---

## Project Structure (Conceptual)

```
/SampleApp
    main.cpp
    /D3D12
        d3d12core.dll
        d3d12sdklayers.dll
```

---

## Disclaimer

This sample is provided for demonstration and reference purposes only.
It is not intended for production use and may require modification for integration into real-world applications.

---

## References

* Microsoft Direct3D 12 Documentation
* Direct3D 12 Agility SDK (NuGet)
* D3D12 Video Processing API