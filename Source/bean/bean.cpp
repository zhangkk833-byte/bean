// Copyright Epic Games, Inc. All Rights Reserved.

#include "bean.h"
#include "BeanAssets.h"
#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"

class FBeanModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
#if WITH_EDITOR
		// Editor-only: bake the runtime materials into /Game/Bean assets so a later
		// packaging run can load them instead of creating materials at runtime.
		// Registered at module startup so it also works headlessly before packaging:
		//   UnrealEditor.exe bean.uproject -ExecCmds="BeanBakeAssets" -unattended
		IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("BeanBakeAssets"),
			TEXT("Bake runtime-generated materials into /Game/Bean asset files (run once in the editor before packaging)."),
			FConsoleCommandDelegate::CreateLambda([]()
			{
				const bool bOk = FBeanAssets::BakeAssets();
				UE_LOG(LogTemp, Log, TEXT("[Bean] BeanBakeAssets %s"), bOk ? TEXT("OK") : TEXT("FAILED"));
			}),
			ECVF_Default);
#endif
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE(FBeanModule, bean, "bean");
