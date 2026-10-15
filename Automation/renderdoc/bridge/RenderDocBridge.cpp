#include <Windows.h>
#include <sddl.h>

#include "renderdoc_app.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

RENDERDOC_API_1_6_0* GetRenderDocApi() {
    HMODULE module = GetModuleHandleW(L"renderdoc.dll");
    if (!module) return nullptr;
    auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
    if (!getApi) return nullptr;
    RENDERDOC_API_1_6_0* api = nullptr;
    return getApi(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&api)) == 1 ? api : nullptr;
}

std::string Escape(const char* text) {
    std::string result;
    if (!text) return result;
    for (const unsigned char c : std::string(text)) {
        if (c == '\\' || c == '"') { result.push_back('\\'); result.push_back(char(c)); }
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else if (c == '\t') result += "\\t";
        else if (c >= 0x20) result.push_back(char(c));
    }
    return result;
}

std::string Status(RENDERDOC_API_1_6_0* api) {
    if (!api) return "{\"ok\":false,\"error\":\"RenderDoc API unavailable\"}\n";
    int major = 0, minor = 0, patch = 0;
    api->GetAPIVersion(&major, &minor, &patch);
    char buffer[1024];
    const std::string path = Escape(api->GetCaptureFilePathTemplate());
    std::snprintf(buffer, sizeof(buffer),
        "{\"ok\":true,\"version\":\"%d.%d.%d\",\"captures\":%u,\"pathTemplate\":\"%s\"}\n",
        major, minor, patch, api->GetNumCaptures(), path.c_str());
    return buffer;
}

std::string Execute(std::string command) {
    while (!command.empty() && (command.back() == '\r' || command.back() == '\n' || command.back() == '\0'))
        command.pop_back();
    auto* api = GetRenderDocApi();
    if (command == "status") return Status(api);
    if (!api) return "{\"ok\":false,\"error\":\"RenderDoc API unavailable\"}\n";
    if (command.rfind("path ", 0) == 0) {
        api->SetCaptureFilePathTemplate(command.c_str() + 5);
        return Status(api);
    }
    if (command == "capture" || command.rfind("capture ", 0) == 0) {
        if (command.size() > 8) api->SetCaptureTitle(command.c_str() + 8);
        api->TriggerCapture();
        return "{\"ok\":true,\"queued\":true}\n";
    }
    return "{\"ok\":false,\"error\":\"expected status, capture [title], or path <template>\"}\n";
}

DWORD WINAPI BridgeThread(void*) {
    wchar_t pipeName[128];
    std::swprintf(pipeName, sizeof(pipeName) / sizeof(pipeName[0]),
        L"\\\\.\\pipe\\SourceMvmRenderDocBridge_%lu", GetCurrentProcessId());
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;WD)S:(ML;;NW;;;LW)", SDDL_REVISION_1,
            &descriptor, nullptr))
        security.lpSecurityDescriptor = descriptor;
    for (;;) {
        HANDLE pipe = CreateNamedPipeW(pipeName, PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 4096, 4096, 0, security.lpSecurityDescriptor ? &security : nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            if (descriptor) LocalFree(descriptor);
            return 1;
        }
        const BOOL connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (connected) {
            char request[4096]{};
            DWORD read = 0;
            if (ReadFile(pipe, request, sizeof(request) - 1, &read, nullptr)) {
                const std::string response = Execute(std::string(request, read));
                DWORD written = 0;
                WriteFile(pipe, response.data(), DWORD(response.size()), &written, nullptr);
                FlushFileBuffers(pipe);
            }
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
    }
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (HANDLE thread = CreateThread(nullptr, 0, BridgeThread, nullptr, 0, nullptr)) CloseHandle(thread);
    }
    return TRUE;
}
