#pragma once

#include "../Shared/AfxConsole.h"

#include "../Deps/release/prop/cs2/sdk_src/public/tier1/convar.h"
#include "../Deps/release/prop/cs2/sdk_src/public/icvar.h"

class CWrpConCommandCallback
	: public SOURCESDK::CS2::ICommandCallback {
public:
    CWrpConCommandCallback(advancedfx::CommandCallback_t callback)
        : m_Callback(callback)
    {

    }

	virtual void CommandCallback(void * _unknown1_rdx_ptr, SOURCESDK::CS2::CCommand * pArgs) override {			
		ArgsFromCCommand args(*pArgs);
		m_Callback(&args);
	}

private:
	class ArgsFromCCommand :
		public advancedfx::ICommandArgs
	{
	public:
		ArgsFromCCommand(const SOURCESDK::CS2::CCommand &command)
			: m_Command(command)
		{

		}

		/// <comments> implements advancedfx::ICommandArgs </comments>
		virtual int ArgC()
		{
			return m_Command.ArgC();
		}

		/// <comments> implements advancedfx::ICommandArgs </comments>
		virtual char const * ArgV(int i)
		{
			return m_Command.Arg(i);
		}
	private:
		const SOURCESDK::CS2::CCommand & m_Command;
	};

    advancedfx::CommandCallback_t m_Callback;
};

class CWrpAddCommand {
public:
	CWrpAddCommand(class SOURCESDK::CS2::CCmd * command, char const * name,
	               advancedfx::CommandCallback_t callback);
};

// EVERY COMMAND THIS DLL DECLARES, BY NAME, WITHOUT ASKING THE ENGINE.
//
// The engine's console is not the only caller. Deadlock drives the tool over netcon and from
// Panorama buttons, and both dispatch a line themselves (Commands_DispatchLine) -- on that port
// they have to, because its ICvar is not CS2's and registration does not take. A host that knows
// the name can run the command without the engine ever having heard of it.
//
// Returns null when nothing by that name was declared.
advancedfx::CommandCallback_t WrpFindCommandCallback(char const * name);

void WrpRegisterCommands();

// Re-push every declared command if the engine no longer has `mirv_filmmaker`. RenderDoc frame
// captures have left AfxHookSource2 mapped with ConCommands gone from ICvar; without this the
// DLL stays loaded and mirv_* answers "Unknown command" until a full relaunch.
void WrpEnsureCommandsRegistered();

// Unconditional unregister+register. Map loads can wipe the console hash after LevelInitPreEntity
// sees the commands still present, so the ensure-by-probe path alone is not enough.
void WrpForceReregisterCommands();

// Block ensure/force until GetTickCount passes this mark. LevelInit must not touch ICvar while
// the engine is still tearing down/rebuilding the console tables.
void WrpDeferCommandEnsure(unsigned milliseconds);

// Optional registrar for an engine whose ICvar vtable is not CS2's. Null (the default, and what CS2
// uses) means "call the virtual declared in the shared SDK header". See WrpConsole.cpp.
extern void (*g_pfnRegisterConCommandOverride)(class SOURCESDK::CS2::CCmd *);

#define CON_COMMAND( name, description ) \
   static void name(advancedfx::ICommandArgs * args); \
   static CWrpConCommandCallback name##_callback( name ); \
   static SOURCESDK::CS2::CCmd name##_command(#name, description, 0, &name##_callback); \
   static CWrpAddCommand name##_command_add(&name##_command, #name, name); \
   static void name(advancedfx::ICommandArgs * args)