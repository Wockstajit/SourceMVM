#include "WrpConsole.h"

#include "../Shared/AfxConsole.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <list>
#include <utility>
#include <vector>
bool g_bComandsRegisterred = false;
static DWORD g_deferEnsureUntilMs = 0;

void WrpDeferCommandEnsure(unsigned milliseconds) {
    g_deferEnsureUntilMs = GetTickCount() + milliseconds;
}

// AN ENGINE WHOSE ICvar IS NOT CS2's CAN SUPPLY ITS OWN REGISTRATION.
//
// `ICvar` is declared in the shared CS2 SDK header, so a call through it uses CS2's vtable indices.
// Deadlock answers to the same interface version string with a DIFFERENT layout -- 48 slots against
// CS2's 46, everything from GetCvar on shifted by two -- so the virtual call below lands on an
// unrelated function there, returns, and registers nothing. Silently: for the whole life of that
// port, no command it declared existed in the game's own console.
//
// Rather than fork this file or bend the shared header, a target may install its own registrar.
// Null means "use the declared virtual", which is what CS2 does and always did.
void (*g_pfnRegisterConCommandOverride)(class SOURCESDK::CS2::CCmd *) = nullptr;

static std::vector<size_t> & GetCommandHandles() {
    static std::vector<size_t> handles;
    return handles;
}

static std::list<class SOURCESDK::CS2::CCmd *> & GetHeapCommands() {
    static std::list<class SOURCESDK::CS2::CCmd *> heap;
    return heap;
}

static void EnsureLog(const char *fmt, ...) {
    char path[MAX_PATH]{};
    if(!GetEnvironmentVariableA("APPDATA", path, MAX_PATH)) return;
    char file[MAX_PATH]{};
    std::snprintf(file, sizeof(file), "%s\\HLAE\\debuglogs\\wrp_console_ensure.log", path);
    FILE *f = nullptr;
    if(fopen_s(&f, file, "a") != 0 || !f) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::fprintf(f, "%02u:%02u:%02u.%03u ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fclose(f);
}

static void RegisterOne(class SOURCESDK::CS2::CCmd * command) {
    if(g_pfnRegisterConCommandOverride) {
        g_pfnRegisterConCommandOverride(command);
        return;
    }
    auto it = SOURCESDK::CS2::g_pCVar->RegisterConCommand(command);
    if(it.IsValid()) GetCommandHandles().push_back(it.GetIndex());
}



std::list<class SOURCESDK::CS2::CCmd *> & GetCommands() {
    static std::list<class SOURCESDK::CS2::CCmd *> m_Commands;
    return m_Commands;
}

// Name -> callback, so a host that is not the engine can still run the command. See
// WrpFindCommandCallback in the header for why that is not merely a convenience.
static std::list<std::pair<char const *, advancedfx::CommandCallback_t> > & GetCallbacks() {
    static std::list<std::pair<char const *, advancedfx::CommandCallback_t> > m_Callbacks;
    return m_Callbacks;
}

advancedfx::CommandCallback_t WrpFindCommandCallback(char const * name) {
    if(!name) return nullptr;
    for(auto it = GetCallbacks().begin(); it != GetCallbacks().end(); it++) {
        if(it->first && 0 == _stricmp(it->first, name)) return it->second;
    }
    return nullptr;
}

CWrpAddCommand::CWrpAddCommand(class SOURCESDK::CS2::CCmd * command, char const * name,
                               advancedfx::CommandCallback_t callback) {
    GetCallbacks().emplace_back(name, callback);
    GetCommands().emplace_back(command);
    if(SOURCESDK::CS2::g_pCVar) {
        RegisterOne(command);
    }
}

void WrpRegisterCommands() {
    if(g_bComandsRegisterred) return;

    if(g_bComandsRegisterred = (nullptr != SOURCESDK::CS2::g_pCVar)) {
        GetCommandHandles().clear();
        for(auto it =  GetCommands().begin(); it !=  GetCommands().end(); it++) {
            RegisterOne(*it);
        }
    }
}

static bool MirvFilmmakerPresent() {
    // Match the engine console default (bDisallowDeveloper=true). Find(..., false) can still
    // see FCVAR_DEVELOPMENTONLY commands that netcon/users get as "Unknown command".
    auto handle = SOURCESDK::CS2::g_pCVar->FindCommand("mirv_filmmaker", true);
    if(!handle.IsValid()) return false;
    auto *cmd = SOURCESDK::CS2::g_pCVar->GetCmd(handle.Get());
    return cmd && cmd->GetName() && 0 == _stricmp(cmd->GetName(), "mirv_filmmaker");
}

static void ClearHiddenFlags(SOURCESDK::CS2::CCmd *cmd) {
    if(!cmd) return;
    const int64_t hide = SOURCESDK_CS2_FCVAR_DEVELOPMENTONLY | SOURCESDK_CS2_FCVAR_HIDDEN;
    const int64_t flags = cmd->GetFlags();
    if(flags & hide) cmd->SetFlags(flags & ~hide);
}

static void UnregisterOwnedCommands() {
    auto &handles = GetCommandHandles();
    for(size_t index : handles) {
        SOURCESDK::CS2::g_pCVar->UnregisterConCommand(index);
    }
    handles.clear();
    for(auto *owned : GetCommands()) {
        if(!owned || !owned->GetName()) continue;
        auto handle = SOURCESDK::CS2::g_pCVar->FindCommand(owned->GetName(), false);
        if(!handle.IsValid()) continue;
        auto *cmd = SOURCESDK::CS2::g_pCVar->GetCmd(handle.Get());
        if(cmd == owned) SOURCESDK::CS2::g_pCVar->UnregisterConCommand(handle.Get());
    }
    for(auto *owned : GetHeapCommands()) {
        if(!owned || !owned->GetName()) continue;
        auto handle = SOURCESDK::CS2::g_pCVar->FindCommand(owned->GetName(), false);
        if(!handle.IsValid()) continue;
        auto *cmd = SOURCESDK::CS2::g_pCVar->GetCmd(handle.Get());
        if(cmd == owned) SOURCESDK::CS2::g_pCVar->UnregisterConCommand(handle.Get());
    }
}

void WrpForceReregisterCommands() {
    if(!SOURCESDK::CS2::g_pCVar) return;
    UnregisterOwnedCommands();
    // Static CCmd bodies can become unre-registerable after a wipe. Push fresh heap clones
    // each force so RegisterConCommand always sees a new object.
    for(auto *old : GetHeapCommands()) delete old;
    GetHeapCommands().clear();
    GetCommandHandles().clear();
    unsigned ok = 0, fail = 0;
    for(auto *templateCmd : GetCommands()) {
        if(!templateCmd || !templateCmd->GetCallback()) {
            ++fail;
            continue;
        }
        auto *neu = new SOURCESDK::CS2::CCmd(templateCmd->GetName(), templateCmd->GetHelpString(),
                                             templateCmd->GetFlags(), templateCmd->GetCallback());
        ClearHiddenFlags(neu);
        GetHeapCommands().push_back(neu);
        auto it = SOURCESDK::CS2::g_pCVar->RegisterConCommand(neu);
        if(it.IsValid()) {
            GetCommandHandles().push_back(it.GetIndex());
            ClearHiddenFlags(neu);
            ++ok;
        } else {
            ++fail;
        }
    }
    g_bComandsRegisterred = true;
    EnsureLog("force re-register ok=%u fail=%u templates=%zu presentDev=%d presentAll=%d", ok, fail,
              GetCommands().size(),
              SOURCESDK::CS2::g_pCVar->FindCommand("mirv_filmmaker", true).IsValid() ? 1 : 0,
              SOURCESDK::CS2::g_pCVar->FindCommand("mirv_filmmaker", false).IsValid() ? 1 : 0);
    advancedfx::Message("WrpConsole: force re-registered %u/%zu commands\n", ok, GetCommands().size());
}

void WrpEnsureCommandsRegistered() {
    if(g_deferEnsureUntilMs && GetTickCount() < g_deferEnsureUntilMs) return;
    if(!SOURCESDK::CS2::g_pCVar) {
        EnsureLog("ensure: no g_pCVar");
        return;
    }
    if(!g_bComandsRegisterred) {
        WrpRegisterCommands();
        EnsureLog("ensure: first register present=%d", MirvFilmmakerPresent() ? 1 : 0);
        return;
    }
    if(MirvFilmmakerPresent()) return;
    static DWORD lastForceMs = 0;
    const DWORD now = GetTickCount();
    // Cap force attempts: a hard RegisterConCommand failure would otherwise spin every frame.
    if(lastForceMs && now - lastForceMs < 500) return;
    lastForceMs = now;
    EnsureLog("ensure: mirv_filmmaker missing — forcing");
    WrpForceReregisterCommands();
}
