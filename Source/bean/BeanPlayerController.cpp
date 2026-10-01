#include "BeanPlayerController.h"

#include "BeanCharacter.h"
#include "BeanGrid.h"
#include "BeanHUDWidget.h"
#include "Engine/World.h"
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include <commdlg.h>
#include "Windows/HideWindowsPlatformTypes.h"
// winuser.h defines LoadImage as a macro (LoadImageW/A); undef it so it does not
// rename FImageUtils::LoadImage in engine headers parsed afterwards.
#ifdef LoadImage
#undef LoadImage
#endif
#endif
#include "Blueprint/UserWidget.h"
#include "Engine/GameViewportClient.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Framework/Application/SlateApplication.h"
#include "ImageUtils.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "Kismet/GameplayStatics.h"
#include "Framework/Application/SlateUser.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"
#include "GenericPlatform/GenericWindow.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Widgets/SOverlay.h"
#include "Styling/CoreStyle.h"

#if PLATFORM_WINDOWS
namespace
{
	/**
	 * Native HWND of the window currently hosting the game: the PIE window when playing in the
	 * editor, or the standalone/packaged game window. The Win32 common dialog must be owned by
	 * that window, otherwise it is a plain top-level window that ends up hidden behind it.
	 */
	HWND GetBeanGameWindowHandle(UWorld* World)
	{
		if (World)
		{
			if (UGameViewportClient* Viewport = World->GetGameViewport())
			{
				if (TSharedPtr<SWindow> Window = Viewport->GetWindow())
				{
					if (TSharedPtr<FGenericWindow> NativeWindow = Window->GetNativeWindow())
					{
						if (void* Handle = NativeWindow->GetOSWindowHandle())
						{
							return static_cast<HWND>(Handle);
						}
					}
				}
			}
		}

		// Fallback: whatever Slate currently considers the active window (e.g. a non-game viewport).
		if (FSlateApplication::IsInitialized())
		{
			if (TSharedPtr<SWindow> ActiveWindow = FSlateApplication::Get().GetActiveTopLevelWindow())
			{
				if (TSharedPtr<FGenericWindow> NativeWindow = ActiveWindow->GetNativeWindow())
				{
					if (void* Handle = NativeWindow->GetOSWindowHandle())
					{
						return static_cast<HWND>(Handle);
					}
				}
			}
		}

		return ::GetActiveWindow();
	}

}
#endif // PLATFORM_WINDOWS

ABeanPlayerController::ABeanPlayerController()
{
	PrimaryActorTick.bCanEverTick = true;
}

void ABeanPlayerController::BeginPlay()
{
	Super::BeginPlay();

	bShowMouseCursor = false;
	SetInputMode(FInputModeGameOnly());
	SetControlRotation(FRotator(-20.0f, 0.0f, 0.0f));

	AddMappingContext();

	HUDWidget = CreateWidget<UBeanHUDWidget>(this, UBeanHUDWidget::StaticClass());
	if (HUDWidget)
	{
		HUDWidget->AddToViewport(10);
		HUDWidget->SetController(this);
		HUDWidget->OnColorSelected = [this](int32 Index)
		{
			SetCurrentColor(Index);
		};
	}
}

void ABeanPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();
	BindEnhancedInput();
}

void ABeanPlayerController::BindEnhancedInput()
{
	UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(InputComponent);
	if (!EIC) return;

	MappingContext = NewObject<UInputMappingContext>(this, TEXT("BeanMappingContext"));

	auto MakeAction = [this](const TCHAR* Name, EInputActionValueType Type)
	{
		UInputAction* Action = NewObject<UInputAction>(this, Name);
		Action->ValueType = Type;
		return Action;
	};

	IA_MoveForward = MakeAction(TEXT("IA_MoveForward"), EInputActionValueType::Axis1D);
	MappingContext->MapKey(IA_MoveForward, EKeys::W);
	MappingContext->MapKey(IA_MoveForward, EKeys::S).Modifiers.Add(NewObject<UInputModifierNegate>(this));

	IA_MoveRight = MakeAction(TEXT("IA_MoveRight"), EInputActionValueType::Axis1D);
	MappingContext->MapKey(IA_MoveRight, EKeys::D);
	MappingContext->MapKey(IA_MoveRight, EKeys::A).Modifiers.Add(NewObject<UInputModifierNegate>(this));

	IA_Look = MakeAction(TEXT("IA_Look"), EInputActionValueType::Axis2D);
	// Mouse2D is the paired 2D mouse key (X from MouseX, Y from MouseY). Binding
	// MouseX/MouseY separately would put both values on the X channel (pitch dead).
	MappingContext->MapKey(IA_Look, EKeys::Mouse2D);

	IA_Place = MakeAction(TEXT("IA_Place"), EInputActionValueType::Boolean);
	MappingContext->MapKey(IA_Place, EKeys::LeftMouseButton);

	IA_Remove = MakeAction(TEXT("IA_Remove"), EInputActionValueType::Boolean);
	MappingContext->MapKey(IA_Remove, EKeys::RightMouseButton);

	// Mouse wheel is an axis key in UE 5.8; the Started event carries one notch (sign = direction).
	IA_CycleColor = MakeAction(TEXT("IA_CycleColor"), EInputActionValueType::Axis1D);
	MappingContext->MapKey(IA_CycleColor, EKeys::MouseWheelAxis);

	IA_ToggleFly = MakeAction(TEXT("IA_ToggleFly"), EInputActionValueType::Boolean);
	MappingContext->MapKey(IA_ToggleFly, EKeys::F);

	IA_FlyUp = MakeAction(TEXT("IA_FlyUp"), EInputActionValueType::Boolean);
	MappingContext->MapKey(IA_FlyUp, EKeys::SpaceBar);

	IA_FlyDown = MakeAction(TEXT("IA_FlyDown"), EInputActionValueType::Boolean);
	MappingContext->MapKey(IA_FlyDown, EKeys::LeftControl);

	IA_ToggleCursor = MakeAction(TEXT("IA_ToggleCursor"), EInputActionValueType::Boolean);
	MappingContext->MapKey(IA_ToggleCursor, EKeys::Q);

	IA_ToggleUI = MakeAction(TEXT("IA_ToggleUI"), EInputActionValueType::Boolean);
	MappingContext->MapKey(IA_ToggleUI, EKeys::E);

	IA_Save = MakeAction(TEXT("IA_Save"), EInputActionValueType::Boolean);
	MappingContext->MapKey(IA_Save, EKeys::F5);

	EIC->BindAction(IA_MoveForward, ETriggerEvent::Triggered, this, &ABeanPlayerController::OnMoveForward);
	EIC->BindAction(IA_MoveRight, ETriggerEvent::Triggered, this, &ABeanPlayerController::OnMoveRight);
	EIC->BindAction(IA_Look, ETriggerEvent::Triggered, this, &ABeanPlayerController::OnLook);
	EIC->BindAction(IA_Place, ETriggerEvent::Started, this, &ABeanPlayerController::OnPlace);
	EIC->BindAction(IA_Place, ETriggerEvent::Triggered, this, &ABeanPlayerController::OnPlaceHeld);
	EIC->BindAction(IA_Place, ETriggerEvent::Completed, this, &ABeanPlayerController::OnPlaceReleased);
	EIC->BindAction(IA_Remove, ETriggerEvent::Started, this, &ABeanPlayerController::OnRemove);
	EIC->BindAction(IA_Remove, ETriggerEvent::Triggered, this, &ABeanPlayerController::OnRemoveHeld);
	EIC->BindAction(IA_Remove, ETriggerEvent::Completed, this, &ABeanPlayerController::OnRemoveReleased);
	EIC->BindAction(IA_CycleColor, ETriggerEvent::Started, this, &ABeanPlayerController::OnCycleColor);
	EIC->BindAction(IA_ToggleFly, ETriggerEvent::Started, this, &ABeanPlayerController::OnToggleFly);
	EIC->BindAction(IA_FlyUp, ETriggerEvent::Started, this, &ABeanPlayerController::OnFlyUp);
	EIC->BindAction(IA_FlyUp, ETriggerEvent::Completed, this, &ABeanPlayerController::OnFlyUp);
	EIC->BindAction(IA_FlyDown, ETriggerEvent::Started, this, &ABeanPlayerController::OnFlyDown);
	EIC->BindAction(IA_FlyDown, ETriggerEvent::Completed, this, &ABeanPlayerController::OnFlyDown);
	EIC->BindAction(IA_ToggleCursor, ETriggerEvent::Started, this, &ABeanPlayerController::OnToggleCursor);
	EIC->BindAction(IA_ToggleUI, ETriggerEvent::Started, this, &ABeanPlayerController::OnToggleUI);
	EIC->BindAction(IA_Save, ETriggerEvent::Started, this, &ABeanPlayerController::OnSave);
}

void ABeanPlayerController::AddMappingContext()
{
	if (ULocalPlayer* LocalPlayer = GetLocalPlayer())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
		{
			Subsystem->AddMappingContext(MappingContext, 0);
		}
	}
}

void ABeanPlayerController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!PawnChar)
	{
		PawnChar = Cast<ABeanCharacter>(GetPawn());
	}
	EnsureGrid();

	if (Grid)
	{
		UpdateTarget();
	}
}

void ABeanPlayerController::EnsureGrid()
{
	if (!Grid)
	{
		Grid = Cast<ABeanGrid>(UGameplayStatics::GetActorOfClass(GetWorld(), ABeanGrid::StaticClass()));
	}
}

void ABeanPlayerController::ApplyCursorMode()
{
	if (bShowMouseCursor)
	{
		SetShowMouseCursor(true);
		FInputModeGameAndUI Mode;
		Mode.SetHideCursorDuringCapture(false);
		SetInputMode(Mode);
	}
	else
	{
		SetShowMouseCursor(false);
		SetInputMode(FInputModeGameOnly());
	}
}

bool ABeanPlayerController::IsClickOverHUD() const
{
	if (!HUDWidget) return false;
	const TSharedPtr<FSlateUser> User = FSlateApplication::Get().GetCursorUser();
	if (!User.IsValid()) return false;
	return User->IsWidgetUnderCursor(HUDWidget->TakeWidget());
}

void ABeanPlayerController::UpdateTarget()
{
	if (!Grid || !PlayerCameraManager) return;

	const FVector Start = PlayerCameraManager->GetCameraLocation();
	const FVector Forward = PlayerCameraManager->GetCameraRotation().Vector();
	// Max reach of the placement raycast: ~30 grid cells (CellSize = 25 units each).
	const float MaxTraceDistance = ABeanGrid::CellSize * 30.0f;
	const FVector End = Start + Forward * MaxTraceDistance;

	FHitResult Hit;
	FCollisionQueryParams Params(FName(TEXT("BeanPlacementTrace")), false);
	Params.AddIgnoredActor(this);
	if (APawn* LocalPawn = GetPawn())
	{
		Params.AddIgnoredActor(LocalPawn);
	}

	bTargetValid = false;
	FVector HitPoint;
	bool bHavePoint = false;
	if (GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params))
	{
		// Push the hit point slightly along the surface normal back into the face's
		// cell, so aiming at a shared edge between two cubes doesn't flicker the
		// targeted cell between the two neighbors.
		HitPoint = Hit.Location - Hit.ImpactNormal * 0.5f;
		bHavePoint = true;
	}
	else if (FMath::Abs(Forward.Z) > 1e-4f)
	{
		// Fall back to a mathematical intersection with the ground plane (z = 0),
		// but only within the same max reach so far cells can't be targeted.
		const float t = -Start.Z / Forward.Z;
		if (t > 0.0f && t <= MaxTraceDistance)
		{
			HitPoint = Start + Forward * t;
			bHavePoint = true;
		}
	}

	if (bHavePoint)
	{
		int32 CellX = 0, CellY = 0;
		if (Grid->WorldToCell(HitPoint, CellX, CellY))
		{
			TargetX = CellX;
			TargetY = CellY;
			bTargetValid = true;
		}
	}

	Grid->UpdateGhost(bTargetValid ? TargetX : 0, bTargetValid ? TargetY : 0, bTargetValid);
	if (bTargetValid)
	{
		const bool bOccupied = Grid->IsCellOccupied(TargetX, TargetY);
		Grid->SetGhostColor(bOccupied
			? FLinearColor(1.0f, 0.15f, 0.15f)              // occupied: red = cannot place
			: Grid->Palette[Grid->CurrentColorIndex].Color); // empty: current color
	}
}

// ---------------------------------------------------------------------------
// Input handlers
// ---------------------------------------------------------------------------

void ABeanPlayerController::OnMoveForward(const FInputActionValue& Value)
{
	if (!PawnChar) return;
	const float AxisValue = Value.Get<float>();
	if (!FMath::IsNearlyZero(AxisValue))
	{
		const FRotator YawRot(0.0f, GetControlRotation().Yaw, 0.0f);
		const FVector Forward = FRotationMatrix(YawRot).GetUnitAxis(EAxis::X);
		PawnChar->AddMovementInput(Forward, AxisValue);
	}
}

void ABeanPlayerController::OnMoveRight(const FInputActionValue& Value)
{
	if (!PawnChar) return;
	const float AxisValue = Value.Get<float>();
	if (!FMath::IsNearlyZero(AxisValue))
	{
		const FRotator YawRot(0.0f, GetControlRotation().Yaw, 0.0f);
		const FVector Right = FRotationMatrix(YawRot).GetUnitAxis(EAxis::Y);
		PawnChar->AddMovementInput(Right, AxisValue);
	}
}

void ABeanPlayerController::OnLook(const FInputActionValue& Value)
{
	if (bShowMouseCursor) return; // cursor mode: no mouse-look
	const FVector2D Input = Value.Get<FVector2D>();
	if (FMath::IsNearlyZero(Input.X) && FMath::IsNearlyZero(Input.Y)) return;

	FRotator ControlRot = GetControlRotation();
	ControlRot.Yaw += Input.X * LookSensitivity;
	ControlRot.Pitch = FMath::Clamp(ControlRot.Pitch + Input.Y * LookSensitivity, -85.0f, 85.0f);
	SetControlRotation(ControlRot);
}

void ABeanPlayerController::OnPlace(const FInputActionValue& Value)
{
	if (!Grid) return;

	if (bShowMouseCursor)
	{
		// Pointer mode: this press is consumed (exit cursor on empty space), so the
		// remaining hold cannot start placing. The next press after release will.
		bPlacePressConsumed = true;
		LastPlaceCellX = -1;
		LastPlaceCellY = -1;
		if (!IsClickOverHUD())
		{
			bShowMouseCursor = false;
			ApplyCursorMode();
		}
		return;
	}

	// Normal (game) mode: place on the current cell, then keep sweeping while held.
	bPlacePressConsumed = false;
	LastPlaceCellX = -1;
	LastPlaceCellY = -1;
	if (bTargetValid)
	{
		SweepPlace(TargetX, TargetY);
	}
}

void ABeanPlayerController::OnPlaceHeld(const FInputActionValue& Value)
{
	// Continuous placement while the left button stays down.
	if (bShowMouseCursor || bPlacePressConsumed) return;
	if (!bTargetValid)
	{
		LastPlaceCellX = -1;
		LastPlaceCellY = -1;
		return;
	}
	if (LastPlaceCellX == TargetX && LastPlaceCellY == TargetY) return;
	SweepPlace(TargetX, TargetY);
}

void ABeanPlayerController::OnPlaceReleased(const FInputActionValue& Value)
{
	bPlacePressConsumed = false;
	LastPlaceCellX = -1;
	LastPlaceCellY = -1;
}

void ABeanPlayerController::SweepPlace(int32 ToX, int32 ToY)
{
	if (!Grid || !Grid->IsValidCell(ToX, ToY))
	{
		LastPlaceCellX = -1;
		LastPlaceCellY = -1;
		return;
	}

	if (LastPlaceCellX < 0 || LastPlaceCellY < 0)
	{
		Grid->PlaceCell(ToX, ToY);
	}
	else
	{
		// Step through every cell between the last acted cell and the current target so a
		// fast drag paints a solid line instead of leaving gaps.
		const int32 Dx = ToX - LastPlaceCellX;
		const int32 Dy = ToY - LastPlaceCellY;
		const int32 Steps = FMath::Max(FMath::Abs(Dx), FMath::Abs(Dy));
		for (int32 i = 1; i <= Steps; ++i)
		{
			const float T = static_cast<float>(i) / static_cast<float>(Steps);
			const int32 X = LastPlaceCellX + FMath::RoundToInt(Dx * T);
			const int32 Y = LastPlaceCellY + FMath::RoundToInt(Dy * T);
			if (Grid->IsValidCell(X, Y))
			{
				Grid->PlaceCell(X, Y);
			}
		}
	}

	LastPlaceCellX = ToX;
	LastPlaceCellY = ToY;
}

void ABeanPlayerController::OnRemove(const FInputActionValue& Value)
{
	if (!Grid) return;

	if (bShowMouseCursor)
	{
		bRemovePressConsumed = true;
		LastRemoveCellX = -1;
		LastRemoveCellY = -1;
		if (!IsClickOverHUD())
		{
			bShowMouseCursor = false;
			ApplyCursorMode();
		}
		return;
	}

	bRemovePressConsumed = false;
	LastRemoveCellX = -1;
	LastRemoveCellY = -1;
	if (bTargetValid)
	{
		SweepRemove(TargetX, TargetY);
	}
}

void ABeanPlayerController::OnRemoveHeld(const FInputActionValue& Value)
{
	if (bShowMouseCursor || bRemovePressConsumed) return;
	if (!bTargetValid)
	{
		LastRemoveCellX = -1;
		LastRemoveCellY = -1;
		return;
	}
	if (LastRemoveCellX == TargetX && LastRemoveCellY == TargetY) return;
	SweepRemove(TargetX, TargetY);
}

void ABeanPlayerController::OnRemoveReleased(const FInputActionValue& Value)
{
	bRemovePressConsumed = false;
	LastRemoveCellX = -1;
	LastRemoveCellY = -1;
}

void ABeanPlayerController::SweepRemove(int32 ToX, int32 ToY)
{
	if (!Grid || !Grid->IsValidCell(ToX, ToY))
	{
		LastRemoveCellX = -1;
		LastRemoveCellY = -1;
		return;
	}

	if (LastRemoveCellX < 0 || LastRemoveCellY < 0)
	{
		Grid->RemoveCell(ToX, ToY);
	}
	else
	{
		const int32 Dx = ToX - LastRemoveCellX;
		const int32 Dy = ToY - LastRemoveCellY;
		const int32 Steps = FMath::Max(FMath::Abs(Dx), FMath::Abs(Dy));
		for (int32 i = 1; i <= Steps; ++i)
		{
			const float T = static_cast<float>(i) / static_cast<float>(Steps);
			const int32 X = LastRemoveCellX + FMath::RoundToInt(Dx * T);
			const int32 Y = LastRemoveCellY + FMath::RoundToInt(Dy * T);
			if (Grid->IsValidCell(X, Y))
			{
				Grid->RemoveCell(X, Y);
			}
		}
	}

	LastRemoveCellX = ToX;
	LastRemoveCellY = ToY;
}

void ABeanPlayerController::OnCycleColor(const FInputActionValue& Value)
{
	const float AxisValue = Value.Get<float>();
	if (FMath::IsNearlyZero(AxisValue)) return;
	CycleColor(AxisValue > 0.0f ? -1 : 1);
}

void ABeanPlayerController::CycleColor(int32 Delta)
{
	if (!Grid) return;
	Grid->CycleColor(Delta);
	CurrentColor = Grid->CurrentColorIndex;
	if (HUDWidget)
	{
		HUDWidget->SetSelectedColor(CurrentColor);
	}
}

void ABeanPlayerController::SetCurrentColor(int32 Index)
{
	CurrentColor = Index;
	if (Grid)
	{
		Grid->SetCurrentColor(Index);
	}
	if (HUDWidget)
	{
		HUDWidget->SetSelectedColor(Index);
	}
}

void ABeanPlayerController::OnToggleFly(const FInputActionValue& Value)
{
	if (PawnChar)
	{
		PawnChar->ToggleFly();
	}
}

void ABeanPlayerController::OnFlyUp(const FInputActionValue& Value)
{
	bFlyUpHeld = Value.Get<bool>();
	if (PawnChar)
	{
		PawnChar->SetFlyVerticalInput(bFlyUpHeld, bFlyDownHeld);
	}
}

void ABeanPlayerController::OnFlyDown(const FInputActionValue& Value)
{
	bFlyDownHeld = Value.Get<bool>();
	if (PawnChar)
	{
		PawnChar->SetFlyVerticalInput(bFlyUpHeld, bFlyDownHeld);
	}
}

void ABeanPlayerController::OnToggleCursor(const FInputActionValue& Value)
{
	// Q toggles the mouse cursor (show/hide).
	bShowMouseCursor = !bShowMouseCursor;
	ApplyCursorMode();
}

void ABeanPlayerController::OnToggleUI(const FInputActionValue& Value)
{
	if (HUDWidget)
	{
		HUDWidget->ToggleVisibility();
	}
}

void ABeanPlayerController::OnSave(const FInputActionValue& Value)
{
	if (Grid)
	{
		Grid->SaveGame();
	}
}

// ---------------------------------------------------------------------------
// Image import flow
// ---------------------------------------------------------------------------

void ABeanPlayerController::ImportImage()
{
	if (!Grid || bImportPickerOpen) return;

#if PLATFORM_WINDOWS
	UWorld* World = GetWorld();
	if (!World) return;

	// The picker is a blocking Win32 modal loop, so it must not be opened from inside the Slate
	// button callback: Slate would be frozen mid-tick with the button still held down. Hand the
	// real work to the next tick instead.
	bImportPickerOpen = true;
	World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ABeanPlayerController::OpenImportPicker));
#else
	// Image import currently uses a Windows-only native file dialog.
	UE_LOG(LogTemp, Warning, TEXT("[Bean] ImportImage is only supported on Windows."));
#endif
}

#if PLATFORM_WINDOWS
void ABeanPlayerController::OpenImportPicker()
{
	// DesktopPlatform is a non-redistributable engine module and cannot be referenced from packaged
	// game targets, so use the Windows native file dialog instead.
	//
	// The game window is deliberately NOT restyled while the picker is up: changing the window mode
	// makes the engine recreate the swapchain and re-resolve the viewport, which throws away the
	// frame the user is looking at (the picture collapses to the bare clear colour). Owning the
	// dialog by the game window is enough to keep it on top, and it leaves the scene untouched.
	UWorld* World = GetWorld();
	const HWND OwnerHandle = GetBeanGameWindowHandle(World);

	if (OwnerHandle)
	{
		// Owned windows always sit above their owner, but a topmost owner would still win. The game
		// window is never topmost in practice, so this is a no-op restyle - it just makes the
		// guarantee unconditional without disturbing the scene.
		::SetWindowPos(OwnerHandle, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}

	WCHAR FileBuffer[MAX_PATH] = {};
	OPENFILENAMEW Ofn;
	FMemory::Memzero(Ofn);
	Ofn.lStructSize = sizeof(OPENFILENAMEW);
	// Owning the picker by the game window is what keeps it visible: an ownerless common dialog is
	// a plain top-level window and Windows is free to leave it behind the game window.
	// Do NOT set OFN_ENABLEHOOK/OFN_ENABLETEMPLATE here: requesting a hook or a custom template
	// makes Windows fall back to the legacy (pre-Vista) dialog, and the explorer-style picker is
	// exactly what we want. The dialog is a normal Windows file browser, matching the OS look.
	Ofn.hwndOwner = OwnerHandle;
	Ofn.lpstrFilter = TEXT("图片文件 (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0所有文件 (*.*)\0*.*\0\0");
	Ofn.lpstrFile = FileBuffer;
	Ofn.nMaxFile = MAX_PATH;
	Ofn.lpstrTitle = TEXT("选择要导入的图片");
	// nFilterIndex is 1-based; 0 would leave the filter combo box unset.
	Ofn.nFilterIndex = 1;
	// OFN_NOCHANGEDIR keeps the game's working directory intact while the user browses elsewhere.
	Ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER | OFN_NOCHANGEDIR;

	const BOOL bPicked = GetOpenFileNameW(&Ofn);

	// Hand the foreground back to the game before doing anything else.
	if (OwnerHandle)
	{
		::SetForegroundWindow(OwnerHandle);
	}
	// Closing the picker also drops the OS mouse capture, so re-assert the game's input mode or
	// camera look and the HUD cursor stay wedged.
	ApplyCursorMode();

	// The modal loop is behind us and nothing below can re-enter the UI, so the latch that keeps a
	// second 导入图片 click from queueing a second dialog can be released now.
	bImportPickerOpen = false;

	if (!bPicked)
	{
		return;
	}

	const FString FilePath(Ofn.lpstrFile);

	FImage Source;
	if (!FImageUtils::LoadImage(*FilePath, Source))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Bean] Failed to load image: %s"), *FilePath);
		return;
	}

	// Let the user pick the preview granularity (32/64/128/256) in a modal window.
	const int32 Granularity = PromptGranularity();
	if (Granularity <= 0)
	{
		return; // cancelled
	}

	// Normalise the image to a known 8-bit BGRA format before sampling so we can
	// safely read pixels as FColor (JPEG etc. may load as RGBA8 or other formats).
	FImage SrcBGRA;
	Source.CopyTo(SrcBGRA, ERawImageFormat::BGRA8, EGammaSpace::sRGB);

	// Precompute the palette in CIE L*a*b*: sRGB is not perceptually uniform, and
	// matching in Lab stops colors from being mispicked (dark red vs brown etc.).
	TArray<FVector3f> LabPalette;
	FBeanPalette::BuildLabPalette(Grid->Palette, LabPalette);

	// 1) Compress the whole image down to the chosen resolution (Granularity x Granularity).
	FImage Resized;
	SrcBGRA.ResizeTo(Resized, Granularity, Granularity, ERawImageFormat::BGRA8, EGammaSpace::sRGB);

	// 2) Snap every resized pixel to the nearest of the 48 palette colours (in Lab), so the
	//    hint layer shows exactly the colours the blocks will be built with. Pixels with a
	//    transparent alpha are skipped (nothing is placed there).
	const int32 Count = Granularity * Granularity;
	TArray<uint8> Quantized;
	Quantized.SetNum(Count);
	const FColor* ResizedPixels = reinterpret_cast<const FColor*>(Resized.RawData.GetData());
	for (int32 i = 0; i < Count; ++i)
	{
		const FColor& Px = ResizedPixels[i];
		if (Px.A < 128)
		{
			Quantized[i] = ABeanGrid::EmptyCell;
			continue;
		}
		Quantized[i] = static_cast<uint8>(FBeanPalette::NearestColorIndexLab(LabPalette, Px));
	}

	// 3) Each pixel will be magnified to one grid cell of the hint layer (and of Apply).
	Grid->BeginImagePreview(Quantized, Granularity);
	UE_LOG(LogTemp, Log, TEXT("[Bean] Imported image '%s' compressed to %dx%d, %d blocks."), *FilePath, Granularity, Granularity, Quantized.Num());
	if (HUDWidget)
	{
		HUDWidget->SetImportButtonsEnabled(true);
	}
}
#endif // PLATFORM_WINDOWS

int32 ABeanPlayerController::PromptGranularity()
{
	const int32 Options[] = { 32, 64, 128, 256 };
	int32 Result = 0; // 0 = cancelled

	UGameViewportClient* ViewportClient = GetWorld() ? GetWorld()->GetGameViewport() : nullptr;
	const TSharedPtr<SWindow> ParentWindow = ViewportClient ? ViewportClient->GetWindow() : nullptr;

	// The dialog is laid out in Slate units, which do not grow with the display. PIE runs in a
	// 1280x720 window, so the same 1.0x layout reads as a comfortable panel there; a packaged build
	// runs at the monitor's native height and the identical panel looks tiny. Scale the whole thing
	// by the viewport-height ratio (Slate units, i.e. the viewport's pixel height divided by the
	// window's DPI factor) against that 720-unit-tall reference so it reads at the size it did in
	// the editor. GetViewportSize() reports pixels; if it is not available yet, stay at 1.0x.
	float UIScale = 1.0f;
	{
		FVector2D ViewportSize = FVector2D::ZeroVector;
		if (ViewportClient)
		{
			ViewportClient->GetViewportSize(ViewportSize);
		}

		const float DpiScale = ParentWindow.IsValid() ? FMath::Max(ParentWindow->GetDPIScaleFactor(), 0.01f) : 1.0f;
		if (ViewportSize.Y > 0.0f)
		{
			UIScale = FMath::Clamp(static_cast<float>(ViewportSize.Y) / DpiScale / 720.0f, 1.0f, 2.5f);
		}
	}

	// Every size below is expressed for the 720-unit reference layout and then scaled up.
	const float GridPad = FMath::RoundToFloat(6.0f * UIScale);
	const float OuterPad = FMath::RoundToFloat(12.0f * UIScale);
	const float ButtonPadX = FMath::RoundToFloat(18.0f * UIScale);
	const float ButtonPadY = FMath::RoundToFloat(10.0f * UIScale);
	const float MinButtonWidth = FMath::RoundToFloat(110.0f * UIScale);
	const FSlateFontInfo ButtonFont = FCoreStyle::GetDefaultFontStyle("Bold", FMath::RoundToFloat(16.0f * UIScale));

	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(FText::FromString(TEXT("选择提示层大小")))
		.SizingRule(ESizingRule::Autosized)
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		.HasCloseButton(true);

	// Build a 2x2 grid of size buttons plus a cancel button.
	TSharedRef<SVerticalBox> MainBox = SNew(SVerticalBox);

	TSharedRef<SGridPanel> SizeGrid = SNew(SGridPanel);
	for (int32 i = 0; i < 4; ++i)
	{
		const int32 Opt = Options[i];
		SizeGrid->AddSlot(i % 2, i / 2)
			.Padding(GridPad)
			[
				SNew(SButton)
				.ButtonStyle(&FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("Button"))
				.ContentPadding(FMargin(ButtonPadX, ButtonPadY))
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				.OnClicked_Lambda([&Result, &Window, Opt]()
				{
					Result = Opt;
					Window->RequestDestroyWindow();
					return FReply::Handled();
				})
				[
					SNew(SBox)
					.MinDesiredWidth(MinButtonWidth)
					[
						SNew(STextBlock)
						.Text(FText::FromString(FString::Printf(TEXT("%d × %d"), Opt, Opt)))
						.Font(ButtonFont)
						.Justification(ETextJustify::Center)
					]
				]
			];
	}
	MainBox->AddSlot().AutoHeight().Padding(OuterPad, OuterPad, OuterPad, 0.0f)[SizeGrid];

	MainBox->AddSlot().AutoHeight().Padding(OuterPad, FMath::RoundToFloat(8.0f * UIScale), OuterPad, OuterPad)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[
			SNew(SButton)
			.ButtonStyle(&FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("Button"))
			.ContentPadding(FMargin(ButtonPadX, ButtonPadY))
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			.OnClicked_Lambda([&Result, &Window]()
			{
				Result = 0;
				Window->RequestDestroyWindow();
				return FReply::Handled();
			})
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("取消")))
				.Font(ButtonFont)
				.Justification(ETextJustify::Center)
			]
		]
	];

	Window->SetContent(MainBox);

	FSlateApplication::Get().AddModalWindow(Window, ParentWindow);
	return Result;
}

void ABeanPlayerController::ApplyImage()
{
	if (Grid)
	{
		Grid->ApplyImage();
	}
}

void ABeanPlayerController::CancelImage()
{
	if (Grid)
	{
		Grid->CancelImage();
	}
	if (HUDWidget)
	{
		HUDWidget->SetImportButtonsEnabled(false);
	}
}

void ABeanPlayerController::ChangeOverlayOpacity(float Delta)
{
	if (Grid && Grid->HasOverlay())
	{
		Grid->SetOverlayOpacity(Grid->GetOverlayOpacity() + Delta);
	}
}

void ABeanPlayerController::ToggleOverlayVisible()
{
	if (Grid && Grid->HasOverlay())
	{
		Grid->SetOverlayVisible(!Grid->IsOverlayVisible());
	}
}
