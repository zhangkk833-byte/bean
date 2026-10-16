// Copyright Epic Games, Inc. All Rights Reserved.

#include "bean.h"

#include "BeanAssets.h"
#include "BeanGrid.h"
#include "BeanSelfTest.h"

#if WITH_EDITOR
// The console commands below are editor-only, and so is this header: the game target does not link
// UnrealEd, so including it unconditionally broke the first Shipping/cook run (m05347).
#include "Editor.h"
#endif
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMemory.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "TimerManager.h"

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

		// Editor-only: run the data-layer self-test from the command line. The editor's
		// automation framework never leaves its FindWorkers state on a machine without an
		// automation worker session, so this driver is used instead:
		//   UnrealEditor-Cmd.exe D:\bean\bean.uproject -ExecCmds="BeanSelfTest; Quit" -unattended -nopause -nosplash
		IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("BeanSelfTest"),
			TEXT("Run the Bean data-layer self-test (conversion, undo/redo, batch edits, completion statistics, save/load, large-canvas cost)."),
			FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* InWorld)
			{
				// -ExecCmds can run before the editor has a world; retry on the next tick so the
				// grid half of the self-test still runs instead of being skipped.
				UWorld* World = InWorld;
				if (!World && GEngine)
				{
					for (const FWorldContext& Context : GEngine->GetWorldContexts())
					{
						if (Context.World())
						{
							World = Context.World();
							break;
						}
					}
				}

				if (!World)
				{
					UE_LOG(LogTemp, Warning, TEXT("[BeanSelfTest] no world is available yet; running the image checks only."));
					const int32 Failures = FBeanSelfTest::RunAll(nullptr, true);
					UE_LOG(LogTemp, Display, TEXT("[BeanSelfTest] RESULT: %s (%d failed checks)"),
						Failures == 0 ? TEXT("ALL CHECKS PASSED") : TEXT("FAILURES PRESENT"), Failures);
					return;
				}

				const int32 Failures = FBeanSelfTest::RunAll(World, true);
				UE_LOG(LogTemp, Display, TEXT("[BeanSelfTest] RESULT: %s (%d failed checks)"),
					Failures == 0 ? TEXT("ALL CHECKS PASSED") : TEXT("FAILURES PRESENT"), Failures);
			}),
			ECVF_Default);

		// Editor-only: bounded, instrumented timing of the large-canvas operations. The full
		// self-test runs every check; this one exists purely to produce reviewable numbers (and to
		// stay fast enough that a broken build cannot make it look like a hang):
		//   UnrealEditor-Cmd.exe D:\bean\bean.uproject -ExecCmds="BeanPerf; Quit" -unattended -nopause -nosplash -nullrhi
		IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("BeanPerf"),
			TEXT("Time the large-canvas grid operations (initial build, place, remove, batch, undo/redo, save/load) and report memory."),
			FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* InWorld)
			{
				UWorld* World = InWorld;
				if (!World && GEngine)
				{
					for (const FWorldContext& Context : GEngine->GetWorldContexts())
					{
						if (Context.World())
						{
							World = Context.World();
							break;
						}
					}
				}
				if (!World)
				{
					UE_LOG(LogTemp, Error, TEXT("[BeanPerf] no world available; run this with the editor up or in a game world."));
					return;
				}

				// 32 at the end so the largest canvas is not the one left allocated while the
				// process reports its memory.
				const int32 Sizes[] = { 64, 128, 256, 32 };

				auto Ms = [](const double Seconds) { return FString::Printf(TEXT("%8.1f ms"), Seconds * 1000.0); };
				auto MemAt = []
				{
					const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
					return FString::Printf(TEXT("used %.0f MB / peak %.0f MB"),
						static_cast<double>(Stats.UsedPhysical) / (1024.0 * 1024.0),
						static_cast<double>(Stats.PeakUsedPhysical) / (1024.0 * 1024.0));
				};

				double SpawnSum = 0.0;
				for (const int32 Size : Sizes)
				{
					UE_LOG(LogTemp, Display, TEXT("[BeanPerf] ---------- canvas %d x %d ----------"), Size, Size);
					UE_LOG(LogTemp, Display, TEXT("[BeanPerf]   memory before        : %s"), *MemAt());

					double T0 = FPlatformTime::Seconds();
					ABeanGrid* Grid = World->SpawnActor<ABeanGrid>();
					const double SpawnSeconds = FPlatformTime::Seconds() - T0;
					SpawnSum += SpawnSeconds;
					if (!Grid)
					{
						UE_LOG(LogTemp, Error, TEXT("[BeanPerf]   could not spawn the grid; stopping."));
						return;
					}

					T0 = FPlatformTime::Seconds();
					Grid->ResizeGrid(Size, Size);
					const double SetupSeconds = FPlatformTime::Seconds() - T0;
					UE_LOG(LogTemp, Display, TEXT("[BeanPerf]   setup %dx%d            : %s"), Size, Size, *Ms(SpawnSeconds + SetupSeconds));
					UE_LOG(LogTemp, Display, TEXT("[BeanPerf]   memory after setup   : %s"), *MemAt());

					Grid->ClearHistory();

					// Single placement / removal (average of 32, so one-off hitches do not dominate).
					const int32 SampleCount = 32;
					T0 = FPlatformTime::Seconds();
					for (int32 i = 0; i < SampleCount; ++i)
					{
						Grid->PlaceCell(i % Size, (i / Size) % Size);
					}
					const double PlaceSeconds = (FPlatformTime::Seconds() - T0) / SampleCount;

					T0 = FPlatformTime::Seconds();
					for (int32 i = 0; i < SampleCount; ++i)
					{
						Grid->RemoveCell(i % Size, (i / Size) % Size);
					}
					const double RemoveSeconds = (FPlatformTime::Seconds() - T0) / SampleCount;
					UE_LOG(LogTemp, Display, TEXT("[BeanPerf]   place / remove       : %s / %s"), *Ms(PlaceSeconds), *Ms(RemoveSeconds));

					Grid->ClearHistory();
					Grid->EndActiveStroke();

					// Batch over the whole canvas: the worst case for the incremental rebuild.
					T0 = FPlatformTime::Seconds();
					const int32 Filled = Grid->ApplyBatch(EBeanBatchOp::PlaceCurrentColor, 0, 0, Size - 1, Size - 1);
					const double BatchFillSeconds = FPlatformTime::Seconds() - T0;

					T0 = FPlatformTime::Seconds();
					Grid->ApplyBatch(EBeanBatchOp::Remove, 0, 0, Size - 1, Size - 1);
					const double BatchClearSeconds = FPlatformTime::Seconds() - T0;
					UE_LOG(LogTemp, Display, TEXT("[BeanPerf]   batch fill / clear   : %s / %s  (%d cells)"),
						*Ms(BatchFillSeconds), *Ms(BatchClearSeconds), Filled);

					// Undo/redo of that one full-canvas batch.
					T0 = FPlatformTime::Seconds();
					Grid->Undo();
					const double UndoSeconds = FPlatformTime::Seconds() - T0;
					T0 = FPlatformTime::Seconds();
					Grid->Redo();
					const double RedoSeconds = FPlatformTime::Seconds() - T0;
					UE_LOG(LogTemp, Display, TEXT("[BeanPerf]   undo / redo          : %s / %s"), *Ms(UndoSeconds), *Ms(RedoSeconds));

					// Save / load: the whole canvas plus the target pattern.
					{
						const TArray<uint8> PatternCells;
						const TArray<bool> PatternMask;
						Grid->SetTargetPattern(Size, Size, 0, 0, PatternCells, PatternMask);
					}
					const FString Slot = FString::Printf(TEXT("__BeanPerf_%d"), Size);
					FString SaveError;
					T0 = FPlatformTime::Seconds();
					const bool bSaved = Grid->SaveGameAs(Slot, SaveError);
					const double SaveSeconds = FPlatformTime::Seconds() - T0;

					T0 = FPlatformTime::Seconds();
					const bool bLoaded = Grid->LoadGameFrom(Slot, SaveError);
					const double LoadSeconds = FPlatformTime::Seconds() - T0;
					UE_LOG(LogTemp, Display, TEXT("[BeanPerf]   save / load          : %s / %s  (ok=%d/%d)"),
						*Ms(SaveSeconds), *Ms(LoadSeconds), bSaved ? 1 : 0, bLoaded ? 1 : 0);
					if (!bSaved || !bLoaded)
					{
						UE_LOG(LogTemp, Warning, TEXT("[BeanPerf]   save/load reported: %s"), *SaveError);
					}

					// Do not leave the measurement behind: these slots used to show up in the player's
					// gallery as unreadable-looking rows ("__BeanPerf_128").
					UGameplayStatics::DeleteGameInSlot(Slot, 0);

					UE_LOG(LogTemp, Display, TEXT("[BeanPerf]   memory after run     : %s"), *MemAt());
					Grid->Destroy();
				}

				UE_LOG(LogTemp, Display, TEXT("[BeanPerf] actor spawn total: %s"), *Ms(SpawnSum));
				UE_LOG(LogTemp, Display, TEXT("[BeanPerf] DONE"));
			}),
			ECVF_Default);

		// Editor-only: schedules "BeanPerf" far enough after map load that a world exists.
		// -ExecCmds runs before the world is up, and the console command then silently finds no
		// world, so this deferred runner is what makes the perf report reproducible:
		//   UnrealEditor-Cmd.exe D:\bean\bean.uproject -ExecCmds="BeanPerfDeferred" -unattended -nopause -nosplash -nullrhi
		IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("BeanPerfDeferred"),
			TEXT("Wait for a game world, then run BeanPerf and quit. Use this from -ExecCmds."),
			FConsoleCommandDelegate::CreateLambda([]()
			{
				FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float) -> bool
				{
					UWorld* World = nullptr;
					if (GEngine)
					{
						for (const FWorldContext& Context : GEngine->GetWorldContexts())
						{
							// Either the packaged game world or a PIE session is fine; anything
							// else (editor preview worlds) is not the world the game runs in.
							if (Context.World()
								&& (Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE))
							{
								World = Context.World();
								break;
							}
						}
					}
					if (!World)
					{
						return true; // keep waiting
					}

					UE_LOG(LogTemp, Display, TEXT("[BeanPerf] world is up; running the timed checks."));
					if (GEngine)
					{
						GEngine->Exec(World, TEXT("BeanPerf"));
					}
					// Give the log a moment to flush, then close the editor - the run is unattended.
					FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float) -> bool
					{
						FPlatformMisc::RequestExit(false);
						return false;
					}), 1.0f);
					return false;
				}), 0.5f);
			}),
			ECVF_Default);

		// Editor-only: same world-wait driver for the full self-test. Without this the command is
		// dispatched from -ExecCmds before any game world exists, so RunAll() silently falls back to
		// the image-only half and the grid checks never run:
		//   UnrealEditor-Cmd.exe D:\bean\bean.uproject -ExecCmds="BeanSelfTestDeferred" -unattended -nopause -nosplash -nullrhi
		IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("BeanSelfTestDeferred"),
			TEXT("Wait for a game world, then run the full Bean self-test, write the report and quit."),
			FConsoleCommandDelegate::CreateLambda([]()
			{
				FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float) -> bool
				{
					UWorld* World = nullptr;
					if (GEngine)
					{
						for (const FWorldContext& Context : GEngine->GetWorldContexts())
						{
							if (Context.World()
								&& (Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE))
							{
								World = Context.World();
								break;
							}
						}
					}
					if (!World)
					{
						return true; // keep waiting
					}

					UE_LOG(LogTemp, Display, TEXT("[BeanSelfTest] world is up; running the checks."));
					const int32 Failures = FBeanSelfTest::RunAll(World, true);
					UE_LOG(LogTemp, Display, TEXT("[BeanSelfTest] RESULT: %s (%d failed checks)"),
						Failures == 0 ? TEXT("ALL CHECKS PASSED") : TEXT("FAILURES PRESENT"), Failures);

					// Let the log and the report file finish flushing before the process exits.
					FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float) -> bool
					{
						FPlatformMisc::RequestExit(false);
						return false;
					}), 1.0f);
					return false;
				}), 0.5f);
			}),
			ECVF_Default);
#endif
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE(FBeanModule, bean, "bean");
