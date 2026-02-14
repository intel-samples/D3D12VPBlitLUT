#include "d3d12.h"
#include "d3d12video.h"
#include <dxgi1_6.h>
#include <atlbase.h>
#include <wrl/client.h>
#include <iostream>
#include <vector>
#include <comdef.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <windows.h>
#include <algorithm>
#include <cassert>

using Microsoft::WRL::ComPtr;
ComPtr<ID3D12Device> pDevice;

#pragma comment(lib, "version.lib")

#define VERIFY_SUCCEEDED(hr) \
    do { \
        HRESULT _hr = (hr); \
        if (FAILED(_hr)) { \
            _com_error err(_hr); \
            std::wcout <<  L"FAILED at line " << std::dec << __LINE__ << L": " << #hr << L" = 0x" << std::hex << _hr \
                      << L" (" << err.ErrorMessage() << L")" << std::endl; \
            if (pDevice) { \
                HRESULT removedReason = pDevice->GetDeviceRemovedReason(); \
                _com_error removedErr(removedReason); \
                std::wcout << L"Device Removed Reason: 0x" << std::hex << removedReason \
                           << L" (" << removedErr.ErrorMessage() << L")" << std::endl; \
            } \
            return _hr; \
        } \
    } while(0)

std::wstring ConvertToWString(const char* str) {
    int len = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
    if (len == 0) return L"";
    std::wstring wstr(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, str, -1, &wstr[0], len);
    return wstr;
}

bool ParseCommandLine(int argc, char* argv[],
    std::wstring& inputPath,
    std::wstring& outputPathBase,
    std::wstring& lut1dPath,
    std::wstring& lut3dPath,
    UINT& lut1dSize,
    UINT& lut3dSize)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            inputPath = ConvertToWString(argv[++i]);
        }
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outputPathBase = ConvertToWString(argv[++i]);
        }
        else if (strcmp(argv[i], "-lut1d") == 0 && i + 1 < argc) {
            lut1dPath = ConvertToWString(argv[++i]);
        }
        else if (strcmp(argv[i], "-lut3d") == 0 && i + 1 < argc) {
            lut3dPath = ConvertToWString(argv[++i]);
        }
        else if (strcmp(argv[i], "-lut1dsize") == 0 && i + 1 < argc) {
            lut1dSize = static_cast<UINT>(atoi(argv[++i]));
        }
        else if (strcmp(argv[i], "-lut3dsize") == 0 && i + 1 < argc) {
            lut3dSize = static_cast<UINT>(atoi(argv[++i]));
        }
    }

    if (inputPath.empty() || lut1dPath.empty() || lut3dPath.empty() || lut1dSize == 0 || lut3dSize == 0) {
        return false;
    }
    return true;
}

std::vector<BYTE> ReadFileToVector(const std::wstring& filename) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::wcout << L"Failed to open file for reading: " << filename << std::endl;
        return {};
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<BYTE> buffer(size);
    if (file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return buffer;
    }
    std::wcout << L"Failed to read data from file: " << filename << std::endl;
    return {};
}

bool WriteVectorToFile(const std::wstring& filename, const std::vector<BYTE>& data) {
    // Create directory if it doesn't exist
    size_t pos = filename.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        std::wstring dir = filename.substr(0, pos);
        CreateDirectoryW(dir.c_str(), NULL);
    }

    std::ofstream file(filename, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        std::wcout << L"Failed to open file for writing: " << filename << std::endl;
        return false;
    }

    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    if (!file.good()) {
        std::wcout << L"Failed to write data to file: " << filename << std::endl;
        return false;
    }

    file.close();
    return true;
}

std::vector<BYTE> ReadSingleFrameFromFile(const std::wstring& filename, int frameIndex, UINT64 frameSize) {
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::wcout << L"Failed to open file for reading: " << filename << std::endl;
        return {};
    }

    UINT64 offset = static_cast<UINT64>(frameIndex) * frameSize;
    file.seekg(offset, std::ios::beg);

    if (!file.good()) {
        std::wcout << std::dec << L"Failed to seek to frame " << frameIndex << L" at offset " << offset << std::endl;
        return {};
    }

    std::vector<BYTE> buffer(frameSize);
    if (file.read(reinterpret_cast<char*>(buffer.data()), frameSize)) {
        std::wcout << std::dec << L"Successfully read frame " << frameIndex << L" (" << frameSize << L" bytes)" << std::endl;
        return buffer;
    }

    std::wcout << L"Failed to read frame data from file: " << filename << std::endl;
    return {};
}

void CopyImagePlane(
    BYTE* pDestBase,
    UINT64 destOffset,
    UINT64 destRowPitch,
    const BYTE* pSrcData,
    UINT64 srcRowPitch,
    UINT numRows)
{
    BYTE* pDst = pDestBase + destOffset;
    const BYTE* pSrc = pSrcData;

    UINT64 bytesToCopy = (std::min)(srcRowPitch, destRowPitch);

    for (UINT y = 0; y < numRows; ++y)
    {
        memcpy(pDst, pSrc, static_cast<size_t>(bytesToCopy));
        pDst += destRowPitch;
        pSrc += srcRowPitch;
    }
}

// Calculate offsets and fill upload buffer for P010 format
void FillP010UploadBuffer(
    BYTE* pMappedUploadBuffer,
    const std::vector<BYTE>& inputFrameData,
    UINT width,
    UINT height,
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* pLayouts,
    const UINT* pNumRows)
{
    // P010 / P016 / NV12 logic:
    // Y Plane is full res. 2 bytes per pixel for P010.
    // UV Plane is half height, same pitch as Y (interleaved U and V).

    // 1. Copy Plane 0 (Luma/Y)
    const UINT64 srcPitchY = static_cast<UINT64>(width) * 2; // 2 bytes per pixel for P010
    const BYTE* pSrcY = inputFrameData.data();

    CopyImagePlane(
        pMappedUploadBuffer,
        pLayouts[0].Offset,
        pLayouts[0].Footprint.RowPitch,
        pSrcY,
        srcPitchY,
        pNumRows[0]
    );

    // 2. Copy Plane 1 (Chroma/UV)
    const UINT64 srcPitchUV = static_cast<UINT64>(width) * 2; // Same as Y for P010

    // Offset in the source file: Y plane size is Width * Height * 2 bytes
    const UINT64 yPlaneSizeInFile = static_cast<UINT64>(width) * height * 2;
    const BYTE* pSrcUV = inputFrameData.data() + yPlaneSizeInFile;

    CopyImagePlane(
        pMappedUploadBuffer,
        pLayouts[1].Offset,
        pLayouts[1].Footprint.RowPitch,
        pSrcUV,
        srcPitchUV,
        pNumRows[1]
    );
}

HRESULT WaitForFence(ID3D12Device* pDevice, ID3D12Fence* pFence, UINT64 fenceValue, HANDLE hEvent)
{
    if (pFence->GetCompletedValue() < fenceValue)
    {
        // The macro now has access to pDevice
        VERIFY_SUCCEEDED(pFence->SetEventOnCompletion(fenceValue, hEvent));
        WaitForSingleObject(hEvent, INFINITE);
    }
    return S_OK;
}

bool InitializeD3D12DLLs() {

    wchar_t exePath[MAX_PATH];
    GetModuleFileName(NULL, exePath, MAX_PATH);
    std::wstring exeDir = std::wstring(exePath);
    size_t pos = exeDir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        exeDir = exeDir.substr(0, pos + 1);
    }

    std::wcout << L"Exe directory: " << exeDir << std::endl;

    // Check all need DLLs
    std::vector<std::wstring> requiredDLLs = {
        L"d3d12core.dll",
        L"d3d12sdklayers.dll"
    };

    std::vector<std::wstring> dllPaths;
    for (const auto& dll : requiredDLLs) {
        std::wstring fullPath = exeDir + L"D3D12\\" + dll;
        if (GetFileAttributes(fullPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::wcout << L" " << dll << L" not found at: " << fullPath << std::endl;
            return false;
        }
        dllPaths.push_back(fullPath);
    }

    // set currect exe path as DLLs search path
    if (!SetDllDirectory(exeDir.c_str())) {
        DWORD error = GetLastError();
        std::wcout << L"Failed to set DLL directory to exe dir, error: " << error << std::endl;
        return false;
    }

    // pre load all DLLs
    std::vector<HMODULE> loadedModules;
    for (const auto& dllPath : dllPaths) {
        HMODULE hModule = LoadLibrary(dllPath.c_str());
        if (!hModule) {
            DWORD error = GetLastError();
            std::wcout << L"Failed to load " << dllPath << L", error: " << error << std::endl;
            return false;
        }
        loadedModules.push_back(hModule);
    }

    return true;
}

int main(int argc, char* argv[])
{
    if (!InitializeD3D12DLLs()) {
        std::wcout << L"Failed to initialize D3D12 DLLs. Exiting." << std::endl;
        return -1;
    }

    // Initialize COM
    CoInitialize(nullptr);

    // Parse command line arguments
    std::wstring inputPath, outputPathBase, lut1dPath, lut3dPath;
    UINT lut1dSize = 0, lut3dSize = 0;

    if (!ParseCommandLine(argc, argv, inputPath, outputPathBase, lut1dPath, lut3dPath, lut1dSize, lut3dSize)) {
        std::wcout << L"Usage: SampleApp.exe -i <input_frame> -lut1d <path_to_1d_lut_buff> -lut3d <path_to_3d_lut_dat> -lut1dsize <size> -lut3dsize <size> [-o <output_path_base>]" << std::endl;
        return -1;
    }

    DXGI_FORMAT inputStreamFmt = DXGI_FORMAT_P010;
    DXGI_COLOR_SPACE_TYPE inputStreamColorSpace = DXGI_COLOR_SPACE_CUSTOM;
    UINT InputStreamWidth = 1920;
    UINT InputStreamHeight = 1080;

    DXGI_FORMAT outputStreamFmt = DXGI_FORMAT_NV12;
    DXGI_COLOR_SPACE_TYPE outputStreamColorSpace = DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709;
    UINT OutputStreamWidth = 1920;
    UINT OutputStreamHeight = 1080;

    // P010: Y plane (width * height * 2) + UV plane (width * height * 2 / 2)
    const UINT64 yPlaneSize = static_cast<UINT64>(InputStreamWidth) * InputStreamHeight * 2;
    const UINT64 uvPlaneSize = static_cast<UINT64>(InputStreamWidth) * InputStreamHeight; // Half height, but 2 bytes per UV pair
    const UINT64 singleFrameSize = yPlaneSize + uvPlaneSize;

    std::ifstream inputFile(inputPath, std::ios::binary | std::ios::ate);
    if (!inputFile.is_open()) {
        std::wcout << L"Failed to open input file: " << inputPath << std::endl;
        return -1;
    }

    std::streamsize totalFileSize = inputFile.tellg();
    inputFile.close();

    int totalFrames = static_cast<int>(totalFileSize / singleFrameSize);
    if (totalFrames == 0) {
        std::wcout << L"No complete frames found in input file" << std::endl;
        return -1;
    }

    if (outputPathBase.empty()) {
        std::wstringstream ss;
        ss << L"output_" << OutputStreamWidth << L"x" << OutputStreamHeight;
        outputPathBase = ss.str();
    }

    std::vector<std::wstring> outputPaths;
    for (int i = 0; i < totalFrames; ++i) {
        std::wstringstream ss;
        size_t dotPos = outputPathBase.find_last_of(L'.');
        std::wstring base = (dotPos == std::wstring::npos) ? outputPathBase : outputPathBase.substr(0, dotPos);
        ss << base << L"_frame_" << std::setfill(L'0') << std::setw(3) << i << L".bin";
        outputPaths.push_back(ss.str());
    }

    ComPtr<IDXGIFactory4> factory;
    VERIFY_SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            break;
        }
        adapter.Reset();
    }

    if (!adapter) {
        std::wcout << L"No hardware adapter found" << std::endl;
        return -1;
    }

    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = E_FAIL;
    for (auto level : featureLevels) {
        hr = D3D12CreateDevice(adapter.Get(), level, IID_PPV_ARGS(&pDevice));
        if (SUCCEEDED(hr)) {
            std::wcout << L"Created D3D12 device with feature level: 0x" << std::hex << level << std::endl;
            break;
        }
    }
    VERIFY_SUCCEEDED(hr);

    ComPtr< ID3D12VideoDevice3DLUT> spVideoDevice3dlut;
    VERIFY_SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&spVideoDevice3dlut)));

    DXGI_FORMAT TableFormat1DLUT = DXGI_FORMAT_R16G16B16A16_UNORM;
    DXGI_FORMAT TableFormat3DLUT = DXGI_FORMAT_R16G16B16A16_UNORM;

    D3D12_FEATURE_DATA_VIDEO_PROCESS_SUPPORT1 supportData = {};
    supportData.NodeIndex = 0;
    supportData.InputSample = { InputStreamWidth, InputStreamHeight, { inputStreamFmt, inputStreamColorSpace } };
    supportData.InputFieldType = D3D12_VIDEO_FIELD_TYPE_NONE;
    supportData.InputStereoFormat = D3D12_VIDEO_FRAME_STEREO_FORMAT_NONE;
    supportData.InputFrameRate = { 30, 1 };
    supportData.OutputFormat = { outputStreamFmt, outputStreamColorSpace };
    supportData.OutputStereoFormat = D3D12_VIDEO_FRAME_STEREO_FORMAT_NONE;
    supportData.OutputFrameRate = { 30, 1 };
    supportData.SupportLUT.Format1DLUT = TableFormat1DLUT;
    supportData.SupportLUT.Format3DLUT = TableFormat3DLUT;
    supportData.SupportLUT.ColorSpace3DLUTOutput = outputStreamColorSpace;

    // Set 3D LUT dimension flags based on size
    if (lut3dSize == 17) {
        supportData.SupportLUT.Dimension3DLUTFlags = D3D12_VIDEO_PROCESS_3DLUT_TABLE_DIMENSION_SUPPORT_FLAG_17x17x17;
    }
    else if (lut3dSize == 33) {
        supportData.SupportLUT.Dimension3DLUTFlags = D3D12_VIDEO_PROCESS_3DLUT_TABLE_DIMENSION_SUPPORT_FLAG_33x33x33;
    }
    else if (lut3dSize == 45) {
        supportData.SupportLUT.Dimension3DLUTFlags = D3D12_VIDEO_PROCESS_3DLUT_TABLE_DIMENSION_SUPPORT_FLAG_45x45x45;
    }
    else if (lut3dSize == 65) {
        supportData.SupportLUT.Dimension3DLUTFlags = D3D12_VIDEO_PROCESS_3DLUT_TABLE_DIMENSION_SUPPORT_FLAG_65x65x65;
    }
    else {
        std::wcout << std::dec << L"Unsupported 3D LUT size: " << lut3dSize << std::endl;
        return -1;
    }

    supportData.SupportLUT.Interpolation3DLUTFlags = D3D12_VIDEO_PROCESS_3DLUT_INTERPOLATION_MODE_SUPPORT_FLAG_TRILINEAR;
    VERIFY_SUCCEEDED(spVideoDevice3dlut->CheckFeatureSupport(D3D12_FEATURE_VIDEO_PROCESS_SUPPORT1, &supportData, sizeof(supportData)));
    if (!(supportData.SupportFlags & D3D12_VIDEO_PROCESS_SUPPORT_FLAG_SUPPORTED)) {
        std::wcout << L"Video processing configuration not supported by hardware" << std::endl;
        return -1;
    }

    // Validate LUT sizes
    if (lut1dSize != supportData.SupportLUT.Native1DLUTSize) {
        std::wcout << std::dec << L"Error: Requested 1D LUT size " << lut1dSize
            << L" differs from native size " << supportData.SupportLUT.Native1DLUTSize << std::endl;
    }

    UINT TableSize1DLUT = lut1dSize;
    UINT TableSize3DLUT = lut3dSize;

    D3D12_VIDEO_PROCESS_LUT_TRANSFORM_CONFIGURATION ConfigurationLUT = {};
    ConfigurationLUT.ConfigFlags = D3D12_VIDEO_PROCESS_LUT_TRANSFORM_CONFIGURATION_FLAG_ENABLE;
    ConfigurationLUT.UpsampledChromaInput = DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020;
    ConfigurationLUT.Format1DLUT = TableFormat1DLUT;
    ConfigurationLUT.Format3DLUT = TableFormat3DLUT;
    ConfigurationLUT.ColorSpace3DLUTOutput = outputStreamColorSpace;
    ConfigurationLUT.Interpolation3DLUT = D3D12_VIDEO_PROCESS_3DLUT_INTERPOLATION_MODE_TRILINEAR;

    // Set 3D LUT dimension enum based on size
    if (lut3dSize == 17) {
        ConfigurationLUT.Dimension3DLUT = D3D12_VIDEO_PROCESS_3DLUT_TABLE_DIMENSION_17x17x17;
    }
    else if (lut3dSize == 33) {
        ConfigurationLUT.Dimension3DLUT = D3D12_VIDEO_PROCESS_3DLUT_TABLE_DIMENSION_33x33x33;
    }
    else if (lut3dSize == 45) {
        ConfigurationLUT.Dimension3DLUT = D3D12_VIDEO_PROCESS_3DLUT_TABLE_DIMENSION_45x45x45;
    }
    else if (lut3dSize == 65) {
        ConfigurationLUT.Dimension3DLUT = D3D12_VIDEO_PROCESS_3DLUT_TABLE_DIMENSION_65x65x65;
    }

    D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC1 inputStreamDesc = {};
    inputStreamDesc.Format = inputStreamFmt;
    inputStreamDesc.ColorSpace = inputStreamColorSpace;
    inputStreamDesc.SourceSizeRange = { InputStreamWidth, InputStreamHeight, InputStreamWidth, InputStreamHeight };
    inputStreamDesc.DestinationSizeRange = { OutputStreamWidth, OutputStreamHeight, OutputStreamWidth, OutputStreamHeight };
    inputStreamDesc.ConfigurationLUT = ConfigurationLUT;
    inputStreamDesc.SourceAspectRatio = { 1, 1 };
    inputStreamDesc.DestinationAspectRatio = { 1, 1 };
    inputStreamDesc.FrameRate = { 30, 1 };
    inputStreamDesc.EnableOrientation = FALSE;
    inputStreamDesc.FilterFlags = D3D12_VIDEO_PROCESS_FILTER_FLAG_NONE;
    inputStreamDesc.StereoFormat = D3D12_VIDEO_FRAME_STEREO_FORMAT_NONE;
    inputStreamDesc.FieldType = D3D12_VIDEO_FIELD_TYPE_NONE;
    inputStreamDesc.DeinterlaceMode = D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_NONE;
    inputStreamDesc.EnableAlphaBlending = FALSE;
    inputStreamDesc.NumPastFrames = 0;
    inputStreamDesc.NumFutureFrames = 0;
    inputStreamDesc.EnableAutoProcessing = FALSE;

    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC  outputStreamDesc = {};
    outputStreamDesc.Format = outputStreamFmt;
    outputStreamDesc.ColorSpace = outputStreamColorSpace;
    outputStreamDesc.FrameRate = { 30, 1 };

    D3D12_FEATURE_DATA_VIDEO_PROCESSOR_SIZE2 ProcessorSize2 = {};
    ProcessorSize2.NodeMask = 0u;
    ProcessorSize2.pOutputStreamDesc = &outputStreamDesc;
    ProcessorSize2.NumInputStreamDescs = 1;
    ProcessorSize2.pInputStreamDescs = &inputStreamDesc;
    VERIFY_SUCCEEDED(spVideoDevice3dlut->CheckFeatureSupport(D3D12_FEATURE_VIDEO_PROCESSOR_SIZE2, &ProcessorSize2, sizeof(ProcessorSize2)));

    ComPtr<ID3D12VideoProcessor3DLUT> spVideoProcessor;
    VERIFY_SUCCEEDED(spVideoDevice3dlut->CreateVideoProcessor2(0, &outputStreamDesc, 1, &inputStreamDesc, nullptr, IID_PPV_ARGS(&spVideoProcessor)));

    // ***Command Queues, Allocators, Lists, Fences***
    ComPtr<ID3D12CommandQueue> spCommandQueue; // Video Process queue
    D3D12_COMMAND_QUEUE_DESC commandQueueDesc = {};
    commandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS;
    VERIFY_SUCCEEDED(pDevice->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(&spCommandQueue)));

    ComPtr<ID3D12CommandAllocator> spCommandAllocator;
    VERIFY_SUCCEEDED(pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS, IID_PPV_ARGS(&spCommandAllocator)));

    ComPtr<ID3D12VideoProcessCommandList3 > spCommandList;
    VERIFY_SUCCEEDED(pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS, spCommandAllocator.Get(), nullptr, IID_PPV_ARGS(&spCommandList)));
    CComPtr< ID3D12VideoProcessCommandListInternal > spCommandListInternal;
    VERIFY_SUCCEEDED(spCommandList->QueryInterface(IID_PPV_ARGS(&spCommandListInternal)));
    spCommandList->Close(); // Close initially, will be reset in the loop

    ComPtr<ID3D12CommandQueue> spCopyQueue;
    D3D12_COMMAND_QUEUE_DESC copyQueueDesc = { D3D12_COMMAND_LIST_TYPE_COPY };
    VERIFY_SUCCEEDED(pDevice->CreateCommandQueue(&copyQueueDesc, IID_PPV_ARGS(&spCopyQueue)));

    ComPtr<ID3D12CommandAllocator> spCopyAllocator;
    VERIFY_SUCCEEDED(pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&spCopyAllocator)));

    ComPtr<ID3D12GraphicsCommandList> spCopyCommandList;
    VERIFY_SUCCEEDED(pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, spCopyAllocator.Get(), nullptr, IID_PPV_ARGS(&spCopyCommandList)));
    spCopyCommandList->Close(); // Close initially

    UINT64 fenceValue = 0;
    ComPtr<ID3D12Fence> spCopyFence;
    VERIFY_SUCCEEDED(pDevice->CreateFence(fenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&spCopyFence)));
    ComPtr<ID3D12Fence> spProcessFence;
    VERIFY_SUCCEEDED(pDevice->CreateFence(fenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&spProcessFence)));
    HANDLE eventHandle = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!eventHandle) return E_FAIL;

    // ***Resource Creation (Input, Output, LUTs, Upload/Readback Buffers)***
    D3D12_HEAP_PROPERTIES Properties = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_HEAP_PROPERTIES uploadProps = { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_HEAP_PROPERTIES readbackProps = { D3D12_HEAP_TYPE_READBACK };

    // Input Resource
    ComPtr<ID3D12Resource> pInputResource;
    D3D12_RESOURCE_DESC inputDesc = {};
    inputDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    inputDesc.Width = InputStreamWidth;
    inputDesc.Height = InputStreamHeight;
    inputDesc.DepthOrArraySize = 1;
    inputDesc.MipLevels = 1;
    inputDesc.Format = inputStreamFmt;
    inputDesc.SampleDesc.Count = 1;
    inputDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    VERIFY_SUCCEEDED(pDevice->CreateCommittedResource(&Properties, D3D12_HEAP_FLAG_NONE, &inputDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&pInputResource)));

    // Output Resource
    ComPtr<ID3D12Resource> pOutputResource;
    D3D12_RESOURCE_DESC outputDesc = inputDesc;
    outputDesc.Format = outputStreamFmt;
    outputDesc.Width = OutputStreamWidth;
    outputDesc.Height = OutputStreamHeight;
    VERIFY_SUCCEEDED(pDevice->CreateCommittedResource(&Properties, D3D12_HEAP_FLAG_NONE, &outputDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&pOutputResource)));

    // Input Upload Buffer
    ComPtr<ID3D12Resource> pInputUploadBuffer;
    UINT64 inputUploadBufferSize = 0;
    pDevice->GetCopyableFootprints(&inputDesc, 0, 2, 0, nullptr, nullptr, nullptr, &inputUploadBufferSize);
    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = inputUploadBufferSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    VERIFY_SUCCEEDED(pDevice->CreateCommittedResource(&uploadProps, D3D12_HEAP_FLAG_NONE, &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&pInputUploadBuffer)));

    // Output Readback Buffer
    ComPtr<ID3D12Resource> pReadbackBuffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT outputFootprints[2];;
    UINT64 totalBytes;
    pDevice->GetCopyableFootprints(&outputDesc, 0, 2, 0, outputFootprints, nullptr, nullptr, &totalBytes); // NV12 has 2 planes
    D3D12_RESOURCE_DESC readbackDesc = {};
    readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDesc.Width = totalBytes;
    readbackDesc.Height = 1;
    readbackDesc.DepthOrArraySize = 1;
    readbackDesc.MipLevels = 1;
    readbackDesc.Format = DXGI_FORMAT_UNKNOWN;
    readbackDesc.SampleDesc.Count = 1;
    readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    VERIFY_SUCCEEDED(pDevice->CreateCommittedResource(&readbackProps, D3D12_HEAP_FLAG_NONE, &readbackDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&pReadbackBuffer)));

    std::vector<BYTE> lut1DData = ReadFileToVector(lut1dPath);
    if (lut1DData.empty()) {
        std::wcout << L"Failed to load 1D LUT file: " << lut1dPath << std::endl;
        return -1;
    }

    size_t expected1DBytes = static_cast<size_t>(TableSize1DLUT) * 8; // 8 bytes per entry (RGBA16)
    if (lut1DData.size() != expected1DBytes) {
        std::wcout << std::dec << L"Error: 1D LUT file size mismatch. Expected " << expected1DBytes
            << L" bytes, got " << lut1DData.size() << L" bytes" << std::endl;
        return -1;
    }

    ComPtr<ID3D12Resource> sp1DLUTtransform;
    D3D12_RESOURCE_DESC Desc1DLUTTable = {};
    Desc1DLUTTable.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    Desc1DLUTTable.Width = TableSize1DLUT;
    Desc1DLUTTable.Height = 1;
    Desc1DLUTTable.DepthOrArraySize = 1;
    Desc1DLUTTable.MipLevels = 1;
    Desc1DLUTTable.Format = TableFormat1DLUT;
    Desc1DLUTTable.SampleDesc.Count = 1;
    Desc1DLUTTable.Flags = D3D12_RESOURCE_FLAG_VIDEO_PROCESS_1DLUT_ONLY;
    VERIFY_SUCCEEDED(pDevice->CreateCommittedResource(&Properties, D3D12_HEAP_FLAG_NONE, &Desc1DLUTTable, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&sp1DLUTtransform)));

    ComPtr<ID3D12Resource> p1DLUTUploadBuffer;
    D3D12_RESOURCE_DESC lut1DUploadDesc = {};
    lut1DUploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    lut1DUploadDesc.Width = lut1DData.size();
    lut1DUploadDesc.Height = 1;
    lut1DUploadDesc.DepthOrArraySize = 1;
    lut1DUploadDesc.MipLevels = 1;
    lut1DUploadDesc.Format = DXGI_FORMAT_UNKNOWN;
    lut1DUploadDesc.SampleDesc.Count = 1;
    lut1DUploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    VERIFY_SUCCEEDED(pDevice->CreateCommittedResource(&uploadProps, D3D12_HEAP_FLAG_NONE, &lut1DUploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&p1DLUTUploadBuffer)));
    void* pMapped1DLUT;
    VERIFY_SUCCEEDED(p1DLUTUploadBuffer->Map(0, nullptr, &pMapped1DLUT));
    memcpy(pMapped1DLUT, lut1DData.data(), lut1DData.size());
    p1DLUTUploadBuffer->Unmap(0, nullptr);

    std::vector<BYTE> lut3DData = ReadFileToVector(lut3dPath);
    if (lut3DData.empty()) {
        std::wcout << L"Failed to load 3D LUT file: " << lut3dPath << std::endl;
        return -1;
    }

    size_t expected3DBytes = static_cast<size_t>(TableSize3DLUT) * TableSize3DLUT * TableSize3DLUT * 8; // 8 bytes per pixel (RGBA16)
    if (lut3DData.size() != expected3DBytes) {
        std::wcout << std::dec << L"Error: 3D LUT file size mismatch. Expected " << expected3DBytes
            << L" bytes, got " << lut3DData.size() << L" bytes" << std::endl;
        return -1;
    }

    ComPtr<ID3D12Resource> sp3DLUTtransform;
    D3D12_RESOURCE_DESC Desc3DLUTTable = {};
    Desc3DLUTTable.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    Desc3DLUTTable.Width = TableSize3DLUT;
    Desc3DLUTTable.Height = TableSize3DLUT;
    Desc3DLUTTable.DepthOrArraySize = TableSize3DLUT;
    Desc3DLUTTable.MipLevels = 1;
    Desc3DLUTTable.Format = TableFormat3DLUT;
    Desc3DLUTTable.SampleDesc.Count = 1;
    Desc3DLUTTable.Flags = D3D12_RESOURCE_FLAG_VIDEO_PROCESS_3DLUT_ONLY;
    VERIFY_SUCCEEDED(pDevice->CreateCommittedResource(&Properties, D3D12_HEAP_FLAG_NONE, &Desc3DLUTTable, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&sp3DLUTtransform)));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT placedFootprint3D;
    UINT64 totalBytes3D;
    pDevice->GetCopyableFootprints(&Desc3DLUTTable, 0, 1, 0, &placedFootprint3D, nullptr, nullptr, &totalBytes3D);

    ComPtr<ID3D12Resource> p3DLUTUploadBuffer;
    D3D12_RESOURCE_DESC lut3DUploadDesc = {};
    lut3DUploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    lut3DUploadDesc.Width = totalBytes3D;
    lut3DUploadDesc.Height = 1;
    lut3DUploadDesc.DepthOrArraySize = 1;
    lut3DUploadDesc.MipLevels = 1;
    lut3DUploadDesc.Format = DXGI_FORMAT_UNKNOWN;
    lut3DUploadDesc.SampleDesc.Count = 1;
    lut3DUploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    VERIFY_SUCCEEDED(pDevice->CreateCommittedResource(&uploadProps, D3D12_HEAP_FLAG_NONE, &lut3DUploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&p3DLUTUploadBuffer)));

    void* pMapped3DLUT;
    VERIFY_SUCCEEDED(p3DLUTUploadBuffer->Map(0, nullptr, &pMapped3DLUT));

    const UINT bytesPerPixel = 8;
    const UINT unpaddedRowSize = static_cast<UINT>(TableSize3DLUT) * bytesPerPixel;
    const UINT paddedRowSize = static_cast<UINT>(placedFootprint3D.Footprint.RowPitch);
    BYTE* pDest = reinterpret_cast<BYTE*>(pMapped3DLUT) + placedFootprint3D.Offset;
    const BYTE* pSrc = lut3DData.data();

    for (UINT z = 0; z < placedFootprint3D.Footprint.Depth; ++z) {
        BYTE* pDestSlice = pDest + (z * placedFootprint3D.Footprint.Height * paddedRowSize);
        const BYTE* pSrcSlice = pSrc + (z * placedFootprint3D.Footprint.Height * unpaddedRowSize);
        for (UINT y = 0; y < placedFootprint3D.Footprint.Height; ++y) {
            memcpy(pDestSlice + (y * paddedRowSize), pSrcSlice + (y * unpaddedRowSize), unpaddedRowSize);
        }
    }
    p3DLUTUploadBuffer->Unmap(0, nullptr);

    // Record and execute LUT copy commands
    VERIFY_SUCCEEDED(spCopyAllocator->Reset());
    VERIFY_SUCCEEDED(spCopyCommandList->Reset(spCopyAllocator.Get(), nullptr));

    D3D12_TEXTURE_COPY_LOCATION lut1DSrcLocation = {};
    lut1DSrcLocation.pResource = p1DLUTUploadBuffer.Get();
    lut1DSrcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    pDevice->GetCopyableFootprints(&Desc1DLUTTable, 0, 1, 0, &lut1DSrcLocation.PlacedFootprint, nullptr, nullptr, nullptr);
    D3D12_TEXTURE_COPY_LOCATION lut1DDstLocation = { sp1DLUTtransform.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, 0 };
    spCopyCommandList->CopyTextureRegion(&lut1DDstLocation, 0, 0, 0, &lut1DSrcLocation, nullptr);

    D3D12_TEXTURE_COPY_LOCATION lut3DSrcLocation = {};
    lut3DSrcLocation.pResource = p3DLUTUploadBuffer.Get();
    lut3DSrcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    lut3DSrcLocation.PlacedFootprint = placedFootprint3D;
    D3D12_TEXTURE_COPY_LOCATION lut3DDstLocation = { sp3DLUTtransform.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, 0 };
    spCopyCommandList->CopyTextureRegion(&lut3DDstLocation, 0, 0, 0, &lut3DSrcLocation, nullptr);

    VERIFY_SUCCEEDED(spCopyCommandList->Close());
    ID3D12CommandList* ppLUTCopyCommandLists[1] = { spCopyCommandList.Get() };
    spCopyQueue->ExecuteCommandLists(1, ppLUTCopyCommandLists);
    fenceValue++;
    VERIFY_SUCCEEDED(spCopyQueue->Signal(spCopyFence.Get(), fenceValue));

    WaitForFence(pDevice.Get(), spCopyFence.Get(), fenceValue, eventHandle);

    //                              MAIN PROCESSING LOOP                                    
    for (int frameIndex = 0; frameIndex < totalFrames; ++frameIndex)
    {
        std::wcout << std::dec << L"\n--- Processing Frame " << frameIndex << L" ---" << std::endl;

        // ***1. UPLOAD INPUT FRAME (CORRECTED FOR P010)***
        std::vector<BYTE> inputImageData = ReadSingleFrameFromFile(inputPath, frameIndex, singleFrameSize);
        if (inputImageData.empty()) {
            std::wcout << std::dec << L"Failed to read frame " << frameIndex << L" data" << std::endl;
            return -1;
        }
        assert(inputImageData.size() <= inputUploadBufferSize);

        void* pMappedData;
        VERIFY_SUCCEEDED(pInputUploadBuffer->Map(0, nullptr, &pMappedData));

        // Get the layout information for proper plane copying
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT pLayouts[2];
        UINT pNumRows[2];
        UINT64 pRowSizesInBytes[2];
        UINT64 totalBytesInput;
        pDevice->GetCopyableFootprints(&inputDesc, 0, 2, 0, pLayouts, pNumRows, pRowSizesInBytes, &totalBytesInput);

        FillP010UploadBuffer(
            reinterpret_cast<BYTE*>(pMappedData),
            inputImageData,
            InputStreamWidth,
            InputStreamHeight,
            pLayouts,
            pNumRows
        );
        pInputUploadBuffer->Unmap(0, nullptr);

        VERIFY_SUCCEEDED(spCopyAllocator->Reset());
        VERIFY_SUCCEEDED(spCopyCommandList->Reset(spCopyAllocator.Get(), nullptr));

        D3D12_TEXTURE_COPY_LOCATION dstLocationLuma = { pInputResource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, 0 };
        D3D12_TEXTURE_COPY_LOCATION srcLocationLuma = { pInputUploadBuffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, pLayouts[0] };
        spCopyCommandList->CopyTextureRegion(&dstLocationLuma, 0, 0, 0, &srcLocationLuma, nullptr);
        D3D12_TEXTURE_COPY_LOCATION dstLocationChroma = { pInputResource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, 1 };
        D3D12_TEXTURE_COPY_LOCATION srcLocationChroma = { pInputUploadBuffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, pLayouts[1] };
        spCopyCommandList->CopyTextureRegion(&dstLocationChroma, 0, 0, 0, &srcLocationChroma, nullptr);
        VERIFY_SUCCEEDED(spCopyCommandList->Close());

        ID3D12CommandList* ppCopyCommandLists[] = { spCopyCommandList.Get() };
        spCopyQueue->ExecuteCommandLists(1, ppCopyCommandLists);
        fenceValue++;
        VERIFY_SUCCEEDED(spCopyQueue->Signal(spCopyFence.Get(), fenceValue));
        WaitForFence(pDevice.Get(), spCopyFence.Get(), fenceValue, eventHandle);

        // ***2. PROCESS FRAME***
        VERIFY_SUCCEEDED(spCommandAllocator->Reset());
        VERIFY_SUCCEEDED(spCommandList->Reset(spCommandAllocator.Get()));

        D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS OutputArguments = {};
        OutputArguments.OutputStream[0].pTexture2D = pOutputResource.Get();
        OutputArguments.TargetRectangle = { 0, 0, static_cast<LONG>(OutputStreamWidth), static_cast<LONG>(OutputStreamHeight) };

        D3D12_VIDEO_PROCESS_LUT_UPDATE_FLAGS UpdateFlags;
        if (frameIndex == 0) {
            // First frame: set LUT update flags
            UpdateFlags = D3D12_VIDEO_PROCESS_LUT_UPDATE_FLAG_CSC |
                D3D12_VIDEO_PROCESS_LUT_UPDATE_FLAG_1DLUT |
                D3D12_VIDEO_PROCESS_LUT_UPDATE_FLAG_3DLUT;
            std::wcout << L"Frame " << frameIndex << L": Initializing LUTs and CSC matrix" << std::endl;
        }
        else {
            // Subsequent frames: no LUT updates needed
            UpdateFlags = D3D12_VIDEO_PROCESS_LUT_UPDATE_FLAG_NONE;
        }

        D3D12_VIDEO_PROCESS_TRANSFORM_LUT TransformLUTParams = {};
        TransformLUTParams.Configuration = ConfigurationLUT;
        TransformLUTParams.UpdateFlags = UpdateFlags;

        // Example of BT2020 YUV to RGB CSC Matrix
        TransformLUTParams.pCSCMatrix[0][0] = 1.16893005371093750f;
        TransformLUTParams.pCSCMatrix[0][1] = 0.0f;
        TransformLUTParams.pCSCMatrix[0][2] = 1.68522644042968750f;

        TransformLUTParams.pCSCMatrix[1][0] = 1.16893005371093750f;
        TransformLUTParams.pCSCMatrix[1][1] = -0.188064575195312f;
        TransformLUTParams.pCSCMatrix[1][2] = -0.652969360351562f;

        TransformLUTParams.pCSCMatrix[2][0] = 1.16893005371093750f;
        TransformLUTParams.pCSCMatrix[2][1] = 2.15013122559f;
        TransformLUTParams.pCSCMatrix[2][2] = 0.0f;

        TransformLUTParams.pCSCMatrix[3][0] = -16.0f;
        TransformLUTParams.pCSCMatrix[3][1] = -128.0f;
        TransformLUTParams.pCSCMatrix[3][2] = -128.0f;

        TransformLUTParams.p1DLUTtransform = sp1DLUTtransform.Get();
        TransformLUTParams.p3DLUTTransform = sp3DLUTtransform.Get();

        D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS2 InputArguments = {};
        InputArguments.InputStream[0].pTexture2D = pInputResource.Get();
        InputArguments.Transform.SourceRectangle = { 0, 0, static_cast<LONG>(InputStreamWidth), static_cast<LONG>(InputStreamHeight) };
        InputArguments.Transform.DestinationRectangle = { 0, 0, static_cast<LONG>(OutputStreamWidth), static_cast<LONG>(OutputStreamHeight) };
        InputArguments.LUTMode = TransformLUTParams;

        D3D12_RESOURCE_BARRIER barriers[4];
        barriers[0] = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE, { pInputResource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ} };
        barriers[1] = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE, { pOutputResource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE} };
        barriers[2] = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE, { sp1DLUTtransform.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ} };
        barriers[3] = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE, { sp3DLUTtransform.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ} };
        spCommandList->ResourceBarrier(4, barriers);

        // VPBLT PROCESS
        spCommandListInternal->ProcessFrames2(spVideoProcessor.Get(), &OutputArguments, 1, &InputArguments);
        D3D12_RESOURCE_BARRIER outputToCopySourceBarrier = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE, { pOutputResource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE, D3D12_RESOURCE_STATE_COMMON} };
        spCommandList->ResourceBarrier(1, &outputToCopySourceBarrier);
        spCommandList->Close();
        ID3D12CommandList* ppProcessCommandLists[] = { spCommandList.Get() };
        spCommandQueue->ExecuteCommandLists(1, ppProcessCommandLists);
        fenceValue++;
        VERIFY_SUCCEEDED(spCommandQueue->Signal(spProcessFence.Get(), fenceValue));
        WaitForFence(pDevice.Get(), spProcessFence.Get(), fenceValue, eventHandle);

        // ***3. READBACK OUTPUT***
        VERIFY_SUCCEEDED(spCopyAllocator->Reset());
        VERIFY_SUCCEEDED(spCopyCommandList->Reset(spCopyAllocator.Get(), nullptr));

        // Handle NV12 format with 2 planes (Y and UV)
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT outputFootprints[2];
        pDevice->GetCopyableFootprints(&outputDesc, 0, 2, 0, outputFootprints, nullptr, nullptr, nullptr);

        // Copy Y plane
        D3D12_TEXTURE_COPY_LOCATION outputSrcLocationY = { pOutputResource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, 0 };
        D3D12_TEXTURE_COPY_LOCATION outputDstLocationY = { pReadbackBuffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, outputFootprints[0] };
        spCopyCommandList->CopyTextureRegion(&outputDstLocationY, 0, 0, 0, &outputSrcLocationY, nullptr);

        // Copy UV plane
        D3D12_TEXTURE_COPY_LOCATION outputSrcLocationUV = { pOutputResource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, 1 };
        D3D12_TEXTURE_COPY_LOCATION outputDstLocationUV = { pReadbackBuffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, outputFootprints[1] };
        spCopyCommandList->CopyTextureRegion(&outputDstLocationUV, 0, 0, 0, &outputSrcLocationUV, nullptr);

        // Transition output resource back to common for the next loop iteration
        D3D12_RESOURCE_BARRIER outputToCommonBarrier = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE, {pOutputResource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON } };
        spCommandList->ResourceBarrier(1, &outputToCommonBarrier);

        VERIFY_SUCCEEDED(spCopyCommandList->Close());

        spCopyQueue->ExecuteCommandLists(1, ppCopyCommandLists);
        fenceValue++;
        VERIFY_SUCCEEDED(spCopyQueue->Signal(spCopyFence.Get(), fenceValue));
        WaitForFence(pDevice.Get(), spCopyFence.Get(), fenceValue, eventHandle);

        // ***4. SAVE TO FILE***
        void* pOutputMappedData;
        VERIFY_SUCCEEDED(pReadbackBuffer->Map(0, nullptr, &pOutputMappedData));

        // Calculate NV12 output size (Y plane + UV plane)
        const UINT yPlaneSize = OutputStreamWidth * OutputStreamHeight;
        const UINT uvPlaneSize = OutputStreamWidth * OutputStreamHeight / 2;
        std::vector<BYTE> outputData(yPlaneSize + uvPlaneSize);

        // Copy Y plane
        const BYTE* pSrcDataY = reinterpret_cast<const BYTE*>(pOutputMappedData) + outputFootprints[0].Offset;
        BYTE* pDstDataY = outputData.data();
        for (UINT y = 0; y < OutputStreamHeight; ++y) {
            memcpy(pDstDataY, pSrcDataY, OutputStreamWidth);
            pDstDataY += OutputStreamWidth;
            pSrcDataY += outputFootprints[0].Footprint.RowPitch;
        }

        // Copy UV plane
        const BYTE* pSrcDataUV = reinterpret_cast<const BYTE*>(pOutputMappedData) + outputFootprints[1].Offset;
        BYTE* pDstDataUV = outputData.data() + yPlaneSize;
        for (UINT y = 0; y < OutputStreamHeight / 2; ++y) {
            memcpy(pDstDataUV, pSrcDataUV, OutputStreamWidth);
            pDstDataUV += OutputStreamWidth;
            pSrcDataUV += outputFootprints[1].Footprint.RowPitch;
        }

        pReadbackBuffer->Unmap(0, nullptr);

        if (WriteVectorToFile(outputPaths[frameIndex], outputData)) {
            std::wcout << std::dec << L"Frame " << frameIndex << L" saved successfully to: " << outputPaths[frameIndex] << std::endl;
        }
        else {
            std::wcout << std::dec << L"Failed to save frame " << frameIndex << L" to: " << outputPaths[frameIndex] << std::endl;
        }
    } // End of main processing loop

    std::wcout << L"\n=== All " << totalFrames << L" frames completed=== " << std::endl;

    CloseHandle(eventHandle);
    CoUninitialize();
    return 0;
}
