#include "BeanPlayerController.h"

#include "BeanAssets.h"
#include "BeanCharacter.h"
#include "BeanGrid.h"
#include "BeanHUDWidget.h"
#include "BeanImageConverter.h"
#include "BeanPalette.h"

#include "Blueprint/UserWidget.h"
#include "Components/EditableTextBox.h"
#include "Components/TextBlock.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameUserSettings.h"
#include "GameFramework/PlayerInput.h"
#include "GenericPlatform/GenericWindow.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "ImageUtils.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Styling/CoreStyle.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#if PLATFORM_WINDOWS
	#include "Windows/AllowWindowsPlatformTypes.h"
	#include <windows.h>
	#include <commdlg.h>
	#include "Windows/HideWindowsPlatformTypes.h"
	// winuser.h defines LoadImage as LoadImageA/LoadImageW, which would rename
	// FImageUtils::LoadImage into something that does not compile.
	#ifdef LoadImage
		#undef LoadImage
	#endif
#endif

namespace
{
	/** How far the placement ray reaches, in cells. */
	constexpr float BeanMaxTraceDistanceInCells = 30.0f;
	/** Comfort bound for the export preview: keep it under this many pixels on the long edge. */
	constexpr int32 BeanExportPreviewComfort = 1024;
	/** Formats the import file picker accepts. */
	const TCHAR* BeanImageFileFilter = TEXT("图片文件 (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0所有文件 (*.*)\0*.*\0\0");
	/** Formats the picture save dialog accepts. */
	const TCHAR* BeanPngFileFilter = TEXT("PNG 图片 (*.png)\0*.png\0所有文件 (*.*)\0*.*\0\0");
	/** Thumbnail long edge, in pixels, for gallery entries. */
	constexpr int32 BeanThumbnailMaxEdge = 128;

#if PLATFORM_WINDOWS
	/**
	 * The game window's native handle.
	 *
	 * GetOpenFileNameW / GetSaveFileNameW are modal, blocking Win32 dialogs: they must be owned by
	 * the game window, or they can end up behind it (or steal focus from the editor). Slate's own
	 * window accessors are the only portable way to reach it, so the lookup walks
	 * viewport -> SWindow -> native window and falls back to whatever window is active.
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

		if (FSlateApplication::IsInitialized())
		{
			if (TSharedPtr<SWindow> Active = FSlateApplication::Get().GetActiveTopLevelWindow())
			{
				if (TSharedPtr<FGenericWindow> NativeWindow = Active->GetNativeWindow())
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

	/** Bring the game window forward so a native dialog opened for it is actually visible. */
	void FocusBeanGameWindow(UWorld* World)
	{
		if (HWND Handle = GetBeanGameWindowHandle(World))
		{
			::SetForegroundWindow(Handle);
		}
	}
#endif

	/** Turn one saved work's name into a file name that is safe on every file system. */
	FString MakeWorkFileName(const FString& WorkName, const FString& Suffix)
	{
		const FString Base = ABeanGrid::SanitizeFileName(WorkName.IsEmpty() ? TEXT("BeanWork") : WorkName);
		return FString::Printf(TEXT("%s%s.png"), *Base, *Suffix);
	}

	/**
	 * File name of a work's gallery thumbnail.
	 *
	 * Keyed on the SAVE SLOT, never on the display name: two works may legitimately share a name
	 * (the slot is what is unique), and renaming a work must not orphan its thumbnail or leave the
	 * gallery reading a path nothing writes. One naming rule shared by the writer and every reader
	 * is the only way those two can never drift apart.
	 */
	FString MakeThumbnailFileName(const FString& SlotName)
	{
		const FString Base = ABeanGrid::SanitizeFileName(SlotName.IsEmpty() ? TEXT("BeanGridSlot") : SlotName);
		return FString::Printf(TEXT("%s%s.png"), *Base, TEXT("_thumb"));
	}

	/** Copies the crop rectangle of Source into a square-ish thumbnail-sized image. */
	void BuildCroppedSourcePreview(const FImage& Source, const FBeanConvertOptions& Options, FImage& OutImage)
	{
		if (Source.SizeX <= 0 || Source.SizeY <= 0) return;

		int32 MinX = Options.bHasCrop ? Options.CropMinX : 0;
		int32 MinY = Options.bHasCrop ? Options.CropMinY : 0;
		int32 MaxX = Options.bHasCrop ? Options.CropMaxX : Source.SizeX - 1;
		int32 MaxY = Options.bHasCrop ? Options.CropMaxY : Source.SizeY - 1;
		MinX = FMath::Clamp(MinX, 0, Source.SizeX - 1);
		MinY = FMath::Clamp(MinY, 0, Source.SizeY - 1);
		MaxX = FMath::Clamp(MaxX, MinX, Source.SizeX - 1);
		MaxY = FMath::Clamp(MaxY, MinY, Source.SizeY - 1);

		const int32 CropW = MaxX - MinX + 1;
		const int32 CropH = MaxY - MinY + 1;

		FImage Crop = FImage(MaxX - MinX + 1, MaxY - MinY + 1, Source.Format, Source.GammaSpace);
		// FImage::CopyTo returns void, so there is no failure code to test here.
		Source.CopyTo(Crop, ERawImageFormat::BGRA8, EGammaSpace::sRGB);

		// Show it at a readable, non-square-preserving scale: judging the crop is the point, and the
		// panel letterboxes whatever aspect it gets.
		const int32 LongEdge = FMath::Max(CropW, CropH);
		const float Fit = FMath::Min(1.0f, static_cast<float>(BeanExportPreviewComfort) / static_cast<float>(FMath::Max(1, LongEdge)));
		const int32 DestW = FMath::Max(1, FMath::RoundToInt(CropW * Fit));
		const int32 DestH = FMath::Max(1, FMath::RoundToInt(CropH * Fit));
		FImageCore::ResizeImageAllocDest(FImageView(Crop), OutImage, DestW, DestH, ERawImageFormat::BGRA8, EGammaSpace::sRGB, FImageCore::EResizeImageFilter::MitchellOneQuarter);
	}
}

ABeanPlayerController::ABeanPlayerController()
{
	PrimaryActorTick.bCanEverTick = true;
}

void ABeanPlayerController::BeginPlay()
{
	Super::BeginPlay();

	bShowMouseCursor = false;
	SetInputMode(FInputModeGameOnly());
	// Look slightly down at the board: the placed beads are easier to read from above.
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
		HUDWidget->OnBatchOperation = [this](EBeanBatchOp Op)
		{
			ApplyBatchOperation(Op);
		};
		HUDWidget->OnGalleryExport = [this](const FString& SlotName)
		{
			ExportWorkFromGallery(SlotName);
		};
		HUDWidget->OnColorSelected(HUDWidget ? CurrentColor : CurrentColor);
	}

	SetPreviewInputLock(false);
}

void ABeanPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();
	BindEnhancedInput();
}

void ABeanPlayerController::SetGrid(ABeanGrid* InGrid)
{
	if (Grid == InGrid)
	{
		return;
	}

	if (Grid)
	{
		// Drop the previous grid's subscription before letting it go.
		Grid->GridChangedDelegate.RemoveAll(this);
	}

	Grid = InGrid;

	if (Grid)
	{
		Grid->GridChangedDelegate.AddUObject(this, &ABeanPlayerController::HandleGridChanged);
		if (HUDWidget)
		{
			HUDWidget->SetImportButtonsEnabled(Grid->HasOverlay());
		}
		RefreshProgress();
		if (Grid->GetSaveStatusMessage().Len() > 0)
		{
			// Surface what the load actually did (restored from backup, version mismatch, ...).
			HandleGridChanged();
		}
	}
}

void ABeanPlayerController::HandleGridChanged()
{
	RefreshHUDState();
}

// ---------------------------------------------------------------------------
// Input setup
// ---------------------------------------------------------------------------

void ABeanPlayerController::AddMappingContext()
{
	if (!MappingContext)
	{
		return;
	}

	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		Subsystem->AddMappingContext(MappingContext, 0);
	}
}

void ABeanPlayerController::BindEnhancedInput()
{
	UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(InputComponent);
	if (!EIC)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Bean] InputComponent is not an EnhancedInputComponent; input will not work."));
		return;
	}

	MappingContext = NewObject<UInputMappingContext>(this, TEXT("BeanMappingContext"));
	if (!MappingContext)
	{
		return;
	}

	// Every action is created at runtime: the project deliberately ships no input assets, so the
	// bindings live in code and cannot drift away from the handlers below.
	auto MakeAction = [this](const TCHAR* Name, EInputActionValueType Type)
	{
		UInputAction* Action = NewObject<UInputAction>(this, Name);
		Action->ValueType = Type;
		return Action;
	};

	IA_MoveForward = MakeAction(TEXT("IA_MoveForward"), EInputActionValueType::Axis1D);
	IA_MoveRight = MakeAction(TEXT("IA_MoveRight"), EInputActionValueType::Axis1D);
	IA_Look = MakeAction(TEXT("IA_Look"), EInputActionValueType::Axis2D);
	IA_Place = MakeAction(TEXT("IA_Place"), EInputActionValueType::Boolean);
	IA_Remove = MakeAction(TEXT("IA_Remove"), EInputActionValueType::Boolean);
	IA_CycleColor = MakeAction(TEXT("IA_CycleColor"), EInputActionValueType::Axis1D);
	IA_ToggleFly = MakeAction(TEXT("IA_ToggleFly"), EInputActionValueType::Boolean);
	IA_FlyUp = MakeAction(TEXT("IA_FlyUp"), EInputActionValueType::Boolean);
	IA_FlyDown = MakeAction(TEXT("IA_FlyDown"), EInputActionValueType::Boolean);
	IA_ToggleCursor = MakeAction(TEXT("IA_ToggleCursor"), EInputActionValueType::Boolean);
	IA_ToggleUI = MakeAction(TEXT("IA_ToggleUI"), EInputActionValueType::Boolean);
	IA_Save = MakeAction(TEXT("IA_Save"), EInputActionValueType::Boolean);
	IA_Escape = MakeAction(TEXT("IA_Escape"), EInputActionValueType::Boolean);

	// ---- movement ----
	MappingContext->MapKey(IA_MoveForward, EKeys::W);
	MappingContext->MapKey(IA_MoveRight, EKeys::D);

	// S and A are the same two actions with a negating modifier, so one handler per axis covers
	// both directions. MapKey hands back a REFERENCE into the context's mapping array; copying it
	// into a local (as this used to) puts the modifier on a throwaway copy and silently leaves S
	// and A acting like W and D.
	{
		FEnhancedActionKeyMapping& Backward = MappingContext->MapKey(IA_MoveForward, EKeys::S);
		Backward.Modifiers.Add(NewObject<UInputModifierNegate>(this));

		FEnhancedActionKeyMapping& Left = MappingContext->MapKey(IA_MoveRight, EKeys::A);
		Left.Modifiers.Add(NewObject<UInputModifierNegate>(this));
	}

	MappingContext->MapKey(IA_Look, EKeys::Mouse2D);

	// ---- editing ----
	MappingContext->MapKey(IA_Place, EKeys::LeftMouseButton);
	MappingContext->MapKey(IA_Remove, EKeys::RightMouseButton);
	MappingContext->MapKey(IA_CycleColor, EKeys::MouseWheelAxis);

	// ---- camera / display ----
	MappingContext->MapKey(IA_ToggleFly, EKeys::F);
	MappingContext->MapKey(IA_FlyUp, EKeys::SpaceBar);
	MappingContext->MapKey(IA_FlyDown, EKeys::LeftControl);
	MappingContext->MapKey(IA_ToggleCursor, EKeys::Q);
	MappingContext->MapKey(IA_ToggleUI, EKeys::E);
	MappingContext->MapKey(IA_Save, EKeys::F5);
	MappingContext->MapKey(IA_Escape, EKeys::Escape);

	// ---- undo / redo ----
	// Deliberately NOT mapped here: Enhanced Input matches a mapping against the key alone and has
	// no way to require the Ctrl flag, so a mapping on Z would also fire on a bare Z. Ctrl+Z /
	// Ctrl+Y are handled in InputKey below, where the modifier flags are visible.

	// ---- bindings ----
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

	EIC->BindAction(IA_Escape, ETriggerEvent::Started, this, &ABeanPlayerController::OnEscape);

	// ---- undo / redo shortcuts -------------------------------------------
	// Bound through the legacy action route on purpose: an Enhanced Input mapping is matched against
	// the key alone and cannot require a modifier, so a UInputAction on Z would also fire on a bare
	// Z. The legacy binding cannot express Ctrl either (its FInputChord is resolved by Slate, which
	// clears modifier bits while Ctrl is held), so the two action names below are mapped to Ctrl+Z /
	// Ctrl+Y and the handlers additionally check the live Ctrl state, which is what actually keeps a
	// bare Z from undoing.
	{
		// The mapping itself is deliberately unmodified (Ctrl flag false): the engine's chord
		// resolution can drop modifier bits while Ctrl is held, so requiring Ctrl in the mapping
		// risks the event never being delivered at all. The action name is what is unique here; the
		// handler is what enforces Ctrl.
		UPlayerInput::AddEngineDefinedActionMapping(FInputActionKeyMapping(TEXT("BeanUndoAction"), EKeys::Z));
		UPlayerInput::AddEngineDefinedActionMapping(FInputActionKeyMapping(TEXT("BeanRedoAction"), EKeys::Y));
		UPlayerInput::AddEngineDefinedActionMapping(FInputActionKeyMapping(TEXT("BeanSaveAction"), EKeys::S));
	}

	// Bound through the legacy UInputComponent overload explicitly: the Enhanced Input overload
	// set only accepts handlers that take an action value, and these two take no arguments.
	UInputComponent* LegacyInput = InputComponent;
	LegacyInput->BindAction(TEXT("BeanUndoAction"), IE_Pressed, this, &ABeanPlayerController::HandleUndoKey);
	LegacyInput->BindAction(TEXT("BeanRedoAction"), IE_Pressed, this, &ABeanPlayerController::HandleRedoKey);

	// S is also strafe left, so this binding must not swallow the key: with input consumed, the
	// Enhanced Input mapping for IA_MoveForward would never see the press and the player could no
	// longer walk left. Ctrl+S is what saves; the handler checks the modifier itself.
	FInputActionBinding& SaveBinding = LegacyInput->BindAction(
		TEXT("BeanSaveAction"), IE_Pressed, this, &ABeanPlayerController::HandleSaveKey);
	SaveBinding.bConsumeInput = false;
}

void ABeanPlayerController::HandleUndoKey()
{
	// The legacy action binding cannot express the Ctrl requirement, so a bare Z reaches this
	// handler too and is filtered out here by asking the keyboard what is actually held.
	if (!IsInputKeyDown(EKeys::LeftControl) && !IsInputKeyDown(EKeys::RightControl))
	{
		return;
	}

	// Ctrl+Shift+Z is the other common redo chord; treat it as redo.
	if (IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift))
	{
		Redo();
		return;
	}

	Undo();
}

void ABeanPlayerController::HandleRedoKey()
{
	if (!IsInputKeyDown(EKeys::LeftControl) && !IsInputKeyDown(EKeys::RightControl))
	{
		return;
	}
	Redo();
}

// ---------------------------------------------------------------------------
// Frame work
// ---------------------------------------------------------------------------

void ABeanPlayerController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!PawnChar)
	{
		PawnChar = Cast<ABeanCharacter>(GetPawn());
	}

	EnsureGrid();

	if (Grid && Grid->IsReady() && !IsAnyPanelOpen())
	{
		UpdateTarget();
	}
	else if (Grid)
	{
		Grid->SetGhostVisible(false);
		bTargetValid = false;
	}

	// Enter / Esc are handled by the HUD; keep the selection panel in step with the grid anyway so
	// a selection that was dropped from the grid side cannot leave a stale panel on screen.
	if (HUDWidget && Grid)
	{
		const bool bHasSelection = Grid->HasSelection();
		if (bSelectionMode != bHasSelection)
		{
			bSelectionMode = bSelectionMode || bHasSelection;
		}
	}
}

void ABeanPlayerController::EnsureGrid()
{
	if (Grid && Grid->IsReady())
	{
		return;
	}

	if (ABeanGrid* Found = Cast<ABeanGrid>(UGameplayStatics::GetActorOfClass(GetWorld(), ABeanGrid::StaticClass())))
	{
		SetGrid(Found);
	}
}

void ABeanPlayerController::ApplyCursorMode()
{
	// Pointer mode: the mouse drives the HUD (palette, buttons) and cannot look around.
	if (bShowMouseCursor)
	{
		FInputModeGameAndUI Mode;
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Mode.SetHideCursorDuringCapture(false);
		SetInputMode(Mode);
	}
	else
	{
		SetInputMode(FInputModeGameOnly());
	}
}

bool ABeanPlayerController::IsPointerOverHUDControl() const
{
	if (!HUDWidget || !HUDWidget->IsInViewport())
	{
		return false;
	}

	UGameViewportClient* ViewportClient = GetWorld() ? GetWorld()->GetGameViewport() : nullptr;
	const TSharedPtr<SWindow> Window = ViewportClient ? ViewportClient->GetWindow() : nullptr;
	if (!Window.IsValid())
	{
		return false;
	}

	// Ask Slate what is under the pointer inside the game window. The public query is
	// LocateWindowUnderMouse (its per-window twin, LocateWidgetInWindow, is protected).
	const TArray<TSharedRef<SWindow>> Windows{ Window.ToSharedRef() };
	const FWidgetPath Path = FSlateApplication::Get().LocateWindowUnderMouse(
		FSlateApplication::Get().GetCursorPos(), Windows, /*bIgnoreEnabledStatus=*/false, 0);

	// Walk outwards from the deepest widget under the cursor and stop at the HUD root, so a hit on
	// the viewport itself does not count as "on the interface". A control is recognised by the Slate
	// type that consumes a click, which covers everything this HUD builds.
	const TSharedPtr<SWidget> HUD = HUDWidget->TakeWidget();
	for (int32 Index = Path.Widgets.Num() - 1; Index >= 0; --Index)
	{
		const TSharedPtr<SWidget> Widget = Path.Widgets[Index].Widget;
		if (!Widget.IsValid() || Widget == HUD)
		{
			break;
		}
		if (!Widget->IsEnabled() || !Widget->GetVisibility().IsHitTestVisible())
		{
			continue;
		}

		const FName Type = Widget->GetType();
		if (Type == TEXT("SButton") || Type == TEXT("SCheckBox") || Type == TEXT("SScrollBar")
			|| Type == TEXT("SEditableText") || Type == TEXT("SEditableTextBox")
			|| Type == TEXT("SComboBox") || Type == TEXT("SSlider"))
		{
			return true;
		}
	}

	return false;
}

void ABeanPlayerController::UpdateTarget()
{
	if (!Grid)
	{
		return;
	}

	const FVector CameraStart = PlayerCameraManager ? PlayerCameraManager->GetCameraLocation() : GetPawnOrSpectator() ? GetPawnOrSpectator()->GetActorLocation() : FVector::ZeroVector;
	const FVector CameraDirection = PlayerCameraManager ? PlayerCameraManager->GetCameraRotation().Vector() : FVector::ForwardVector;

	// Original behaviour, kept deliberately (m04494): outside selection mode the target comes from
	// the camera centre, so the placement hint always sits on the crosshair and putting the pointer
	// out (Q) does not move it. Only the selection marquee is driven by the pointer - it has to be,
	// or the rectangle would be pinned to whatever cell the crosshair happens to sit on, which was
	// exactly "无法重新选取范围" (m03538). The ghost is hidden while selecting, so this does not make
	// the hint jump when selection mode opens.
	FVector Start = CameraStart;
	FVector Direction = CameraDirection;
	if (bShowMouseCursor && bSelectionMode)
	{
		FVector CursorStart;
		FVector CursorDirection;
		if (DeprojectMousePositionToWorld(CursorStart, CursorDirection))
		{
			Start = CursorStart;
			Direction = CursorDirection;
		}
	}

	const FVector End = Start + Direction * (ABeanGrid::CellSize * BeanMaxTraceDistanceInCells);

	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(BeanPlaceTrace), false, GetPawn());

	FVector Point = End;
	bool bHasPoint = false;

	if (GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params))
	{
		// Step back along the surface normal: without it, a ray that grazes a cube corner flickers
		// between two cells, which makes placement feel random.
		Point = Hit.ImpactPoint - Hit.ImpactNormal * (ABeanGrid::CellSize * 0.5f);
		bHasPoint = true;
	}
	else if (GetWorld()->LineTraceSingleByChannel(Hit, Start, End + Direction * (ABeanGrid::CellSize * 60.0f), ECC_Visibility, Params))
	{
		Point = Hit.ImpactPoint - Hit.ImpactNormal * (ABeanGrid::CellSize * 0.5f);
		bHasPoint = true;
	}
	else
	{
		// Nothing hit: intersect the ground plane so the ghost still tracks the cursor out on the
		// empty board instead of disappearing.
		const float Denom = Direction.Z;
		if (FMath::Abs(Denom) > KINDA_SMALL_NUMBER)
		{
			const float T = -Start.Z / Denom;
			if (T > 0.0f)
			{
				Point = Start + Direction * T;
				bHasPoint = true;
			}
		}
	}

	int32 CellX = 0;
	int32 CellY = 0;
	bTargetValid = bHasPoint && Grid->WorldToCell(Point, CellX, CellY) && Grid->IsValidCell(CellX, CellY);

	if (bTargetValid)
	{
		TargetX = CellX;
		TargetY = CellY;
		Grid->UpdateGhost(TargetX, TargetY, true);

		if (Grid->IsCellOccupied(TargetX, TargetY))
		{
			Grid->SetGhostColor(FLinearColor(1.0f, 0.15f, 0.15f, 1.0f));
		}
		else
		{
			Grid->SetGhostColor(Grid->Palette.IsValidIndex(Grid->CurrentColorIndex) ? Grid->Palette[Grid->CurrentColorIndex].Color : FLinearColor::White);
		}
	}
	else
	{
		Grid->SetGhostVisible(false);
	}
}

// ---------------------------------------------------------------------------
// Placement / removal
// ---------------------------------------------------------------------------

void ABeanPlayerController::OnPlace(const FInputActionValue& Value)
{
	if (IsAnyPanelOpen())
	{
		return;
	}

	// Pointer mode: a click on empty ground puts the pointer away again and goes back to aiming, so
	// the player never has to reach for Q to resume placing. A click that lands on a control is the
	// control's business and leaves the pointer alone. The press itself is consumed either way: the
	// hold that follows must not start placing at the crosshair (m04820).
	if (bShowMouseCursor && !bSelectionMode)
	{
		bPlacePressConsumed = true;
		LastPlaceCellX = -1;
		LastPlaceCellY = -1;
		if (!IsPointerOverHUDControl())
		{
			bShowMouseCursor = false;
			ApplyCursorMode();
		}
		return;
	}

	if (bSelectionMode)
	{
		// The selection rectangle is driven from the cursor rather than the ghost target. The target
		// is refreshed first because the panel that selection mode opens stops Tick from updating it,
		// and a press in that first frame would otherwise use a stale (or uninitialised) cell.
		UpdateTarget();
		BeginSelectionDrag();
		return;
	}

	if (!Grid || !bTargetValid)
	{
		return;
	}

	// Normal (game) mode: this press places, and the hold that follows keeps sweeping.
	bPlacePressConsumed = false;
	BeginEditStroke(TEXT("放置方块"));
	Grid->PlaceCell(TargetX, TargetY);
	LastPlaceCellX = TargetX;
	LastPlaceCellY = TargetY;
}

void ABeanPlayerController::OnPlaceHeld(const FInputActionValue& Value)
{
	if (IsAnyPanelOpen() && !bSelectionMode)
	{
		return;
	}

	if (bSelectionMode)
	{
		// Refresh first: the drag reads TargetX/TargetY, and the press can arrive before this frame's
		// Tick updated them from the pointer.
		UpdateTarget();
		UpdateSelectionDrag();
		return;
	}

	if (bShowMouseCursor || !Grid || !bTargetValid)
	{
		return;
	}

	if (bPlacePressConsumed)
	{
		return;
	}

	SweepPlace(TargetX, TargetY);
}

void ABeanPlayerController::OnPlaceReleased(const FInputActionValue& Value)
{
	if (bSelectionMode)
	{
		EndSelectionDrag();
		return;
	}

	EndEditStroke();
	bPlacePressConsumed = false;
	LastPlaceCellX = -1;
	LastPlaceCellY = -1;
}

void ABeanPlayerController::OnRemove(const FInputActionValue& Value)
{
	if (bSelectionMode)
	{
		// Right-click cancels a selection, and a second right-click leaves selection mode. The target
		// is refreshed first: entering selection mode opens a panel, and a panel keeps Tick from
		// updating the target, so clicking in the same frame would otherwise use a stale cell.
		UpdateTarget();
		if (Grid && Grid->HasSelection())
		{
			ClearSelection();
		}
		else
		{
			ToggleSelectionMode();
		}
		return;
	}

	if (IsAnyPanelOpen())
	{
		return;
	}

	// Right button works like the left one: on empty ground it puts the pointer away, on a control it
	// leaves the pointer to the interface.
	if (bShowMouseCursor)
	{
		bRemovePressConsumed = true;
		LastRemoveCellX = -1;
		LastRemoveCellY = -1;
		if (!IsPointerOverHUDControl())
		{
			bShowMouseCursor = false;
			ApplyCursorMode();
		}
		return;
	}

	if (!Grid || !bTargetValid)
	{
		return;
	}

	// One cell on the press itself. Holding on from here is a drag (see OnRemoveHeld), never a way for
	// a plain click to wipe several beads (m05220).
	bRemovePressConsumed = false;
	RemovePressTime = GetWorld()->GetTimeSeconds();
	LastRemoveCellX = -1;
	LastRemoveCellY = -1;
	FString Error;
	BeginEditStroke(TEXT("移除方块"));
	Grid->RemoveCell(TargetX, TargetY);
}

void ABeanPlayerController::OnRemoveHeld(const FInputActionValue& Value)
{
	if (bSelectionMode)
	{
		return;
	}

	if (IsAnyPanelOpen() || bShowMouseCursor || !Grid || !bTargetValid)
	{
		return;
	}

	if (bRemovePressConsumed)
	{
		return;
	}

	// Erasing continuously is a drag, and a drag is only meant once the button has clearly stayed
	// down. With the view nearly parallel to the ground the aim ray drifts across cells all by itself
	// (hand tremor of one pixel is worth a dozen cells out there), so erasing from the very first
	// frame turned a single click into a string of deletions. Waiting here means a click removes the
	// one cell pressed and nothing else, while holding still sweeps (m05220/m05308).
	constexpr float HoldToEraseDelay = 0.3f;
	if (GetWorld()->GetTimeSeconds() - RemovePressTime < HoldToEraseDelay)
	{
		return;
	}

	if (LastRemoveCellX < 0)
	{
		// First sweep after the delay only anchors: the crosshair may have travelled a long way while
		// the delay ran, and drawing that line is the very thing to avoid.
		LastRemoveCellX = TargetX;
		LastRemoveCellY = TargetY;
		return;
	}

	SweepRemove(TargetX, TargetY);
}

void ABeanPlayerController::OnRemoveReleased(const FInputActionValue& Value)
{
	EndEditStroke();
	bRemovePressConsumed = false;
	LastRemoveCellX = -1;
	LastRemoveCellY = -1;
}

void ABeanPlayerController::SweepPlace(int32 ToX, int32 ToY)
{
	if (!Grid || !bTargetValid)
	{
		return;
	}

	if (LastPlaceCellX < 0)
	{
		LastPlaceCellX = ToX;
		LastPlaceCellY = ToY;
		return;
	}

	// Bresenham between the last acted cell and this frame's target, so a fast drag lays a
	// continuous line instead of dots wherever the cursor happened to be sampled.
	int32 X0 = LastPlaceCellX;
	int32 Y0 = LastPlaceCellY;
	const int32 X1 = ToX;
	const int32 Y1 = ToY;
	const int32 DX = FMath::Abs(X1 - X0);
	const int32 DY = -FMath::Abs(Y1 - Y0);
	const int32 SX = X0 < X1 ? 1 : -1;
	const int32 SY = Y0 < Y1 ? 1 : -1;
	int32 Err = DX + DY;

	while (true)
	{
		if (Grid->IsValidCell(X0, Y0))
		{
			Grid->PlaceCell(X0, Y0);
		}
		if (X0 == X1 && Y0 == Y1)
		{
			break;
		}
		const int32 E2 = 2 * Err;
		if (E2 >= DY)
		{
			Err += DY;
			X0 += SX;
		}
		if (E2 <= DX)
		{
			Err += DX;
			Y0 += SY;
		}
	}

	LastPlaceCellX = ToX;
	LastPlaceCellY = ToY;
}

void ABeanPlayerController::SweepRemove(int32 ToX, int32 ToY)
{
	if (!Grid || !bTargetValid)
	{
		return;
	}

	if (LastRemoveCellX == ToX && LastRemoveCellY == ToY)
	{
		return;
	}

	// Bresenham from the last erased cell, so a held drag erases a clean line instead of dots. Only
	// reached once the button has been down past HoldToEraseDelay, so this never runs for a click.
	int32 X0 = LastRemoveCellX;
	int32 Y0 = LastRemoveCellY;
	const int32 X1 = ToX;
	const int32 Y1 = ToY;
	const int32 DX = FMath::Abs(X1 - X0);
	const int32 DY = -FMath::Abs(Y1 - Y0);
	const int32 SX = X0 < X1 ? 1 : -1;
	const int32 SY = Y0 < Y1 ? 1 : -1;
	int32 Err = DX + DY;

	while (true)
	{
		if (Grid->IsValidCell(X0, Y0))
		{
			Grid->RemoveCell(X0, Y0);
		}
		if (X0 == X1 && Y0 == Y1)
		{
			break;
		}
		const int32 E2 = 2 * Err;
		if (E2 >= DY)
		{
			Err += DY;
			X0 += SX;
		}
		if (E2 <= DX)
		{
			Err += DX;
			Y0 += SY;
		}
	}

	LastRemoveCellX = ToX;
	LastRemoveCellY = ToY;
}

void ABeanPlayerController::BeginEditStroke(const FString& Label)
{
	if (!Grid || bStrokeOpen)
	{
		return;
	}
	bStrokeOpen = true;
	Grid->BeginStroke(Label);
}

void ABeanPlayerController::EndEditStroke()
{
	if (!Grid || !bStrokeOpen)
	{
		return;
	}
	bStrokeOpen = false;
	Grid->EndStroke();
}

// ---------------------------------------------------------------------------
// Selection + batch operations
// ---------------------------------------------------------------------------

void ABeanPlayerController::ToggleSelectionMode()
{
	bSelectionMode = !bSelectionMode;

	if (bSelectionMode)
	{
		// The selection panel is a pointer UI: without the mouse there is no way to press 退出框选,
		// 批量放置 or Esc's on-screen equivalents, which made the mode feel like a trap. Mouse and
		// keyboard both stay live (GameAndUI), so dragging a marquee on the board still works while
		// the panel's buttons remain clickable.
		if (!bShowMouseCursor)
		{
			bShowMouseCursor = true;
			ApplyCursorMode();
		}
		if (Grid)
		{
			Grid->SetSelection(TargetX, TargetY, TargetX, TargetY);
			Grid->SetGhostVisible(false);
		}
	}
	else
	{
		bSelectionDragging = false;
		if (Grid)
		{
			Grid->ClearSelection();
		}
	}

	RefreshSelectionPanel();
}

void ABeanPlayerController::BeginSelectionDrag()
{
	if (!Grid)
	{
		return;
	}
	bSelectionDragging = true;
	SelectionAnchorX = TargetX;
	SelectionAnchorY = TargetY;
	Grid->SetSelection(TargetX, TargetY, TargetX, TargetY);
}

void ABeanPlayerController::UpdateSelectionDrag()
{
	if (!Grid || !bSelectionDragging)
	{
		return;
	}
	Grid->SetSelection(SelectionAnchorX, SelectionAnchorY, TargetX, TargetY);
	RefreshSelectionPanel();
}

void ABeanPlayerController::EndSelectionDrag()
{
	if (!Grid)
	{
		return;
	}
	bSelectionDragging = false;
	RefreshSelectionPanel();
}

void ABeanPlayerController::ClearSelection()
{
	if (Grid)
	{
		Grid->ClearSelection();
	}
	RefreshSelectionPanel();
}

void ABeanPlayerController::SelectAll()
{
	if (!Grid)
	{
		return;
	}
	Grid->SetSelection(0, 0, Grid->GetWidth() - 1, Grid->GetHeight() - 1);
	RefreshSelectionPanel();
}

void ABeanPlayerController::RefreshSelectionPanel()
{
	if (!HUDWidget)
	{
		return;
	}

	const bool bHasSelection = Grid && Grid->HasSelection();
	FString Summary;
	if (bHasSelection)
	{
		int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
		Grid->GetSelection(MinX, MinY, MaxX, MaxY);
		const int32 Cells = (MaxX - MinX + 1) * (MaxY - MinY + 1);
		Summary = FString::Printf(TEXT("选区 %d × %d（%d 格）"), MaxX - MinX + 1, MaxY - MinY + 1, Cells);
	}
	else
	{
		Summary = TEXT("框选：按住左键拖出一个矩形；右键或 Esc 退出");
	}

	HUDWidget->SetSelectionPanelVisible(bSelectionMode, bHasSelection, Summary);
}

void ABeanPlayerController::ApplyBatchOperation(EBeanBatchOp Op)
{
	if (!Grid)
	{
		return;
	}

	if (!Grid->HasSelection())
	{
		if (HUDWidget)
		{
			HUDWidget->ShowStatus(TEXT("先框选一块区域，再执行批量操作。"));
		}
		return;
	}

	int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
	Grid->GetSelection(MinX, MinY, MaxX, MaxY);

	const int32 Changed = Grid->ApplyBatch(Op, MinX, MinY, MaxX, MaxY);
	if (Changed <= 0)
	{
		if (HUDWidget)
		{
			HUDWidget->ShowStatus(TEXT("选区里没有需要修改的格子。"));
		}
		return;
	}

	RefreshProgress();
	RefreshSelectionPanel();
	MarkDirtyAndAutoSave();
}

// ---------------------------------------------------------------------------
// Undo / redo
// ---------------------------------------------------------------------------

void ABeanPlayerController::Undo()
{
	if (!Grid)
	{
		return;
	}

	// An undo in the middle of a drag would close the open group first; keep the two apart.
	EndEditStroke();
	Grid->Undo();
	RefreshHUDState();
	MarkDirtyAndAutoSave();
}

void ABeanPlayerController::Redo()
{
	if (!Grid)
	{
		return;
	}

	EndEditStroke();
	Grid->Redo();
	RefreshHUDState();
	MarkDirtyAndAutoSave();
}

void ABeanPlayerController::MarkDirtyAndAutoSave()
{
	if (Grid)
	{
		// Autosave is debounced inside the grid: it only writes after the editing settles, and it
		// only reports a message when something actually happened (or failed).
		if (Grid->AutoSaveIfDirty() && HUDWidget)
		{
			HUDWidget->ShowStatus(Grid->GetSaveStatusMessage().IsEmpty() ? TEXT("已自动保存") : Grid->GetSaveStatusMessage());
		}
	}
}

// ---------------------------------------------------------------------------
// Completion statistics
// ---------------------------------------------------------------------------

void ABeanPlayerController::RefreshProgress()
{
	if (HUDWidget && Grid)
	{
		HUDWidget->SetCompletionStats(Grid->GetCachedCompletionStats());
	}
}

void ABeanPlayerController::RefreshHUDState()
{
	if (!HUDWidget || !Grid)
	{
		return;
	}

	HUDWidget->SetHistoryState(Grid->CanUndo(), Grid->CanRedo(), Grid->GetUndoLabel(), Grid->GetRedoLabel());
	HUDWidget->SetCompletionStats(Grid->GetCachedCompletionStats());
	HUDWidget->SetImportButtonsEnabled(Grid->HasOverlay());
	RefreshSelectionPanel();
}

// ---------------------------------------------------------------------------
// Color selection
// ---------------------------------------------------------------------------

void ABeanPlayerController::CycleColor(int32 Delta)
{
	if (!Grid)
	{
		return;
	}
	Grid->CycleColor(Delta);
	CurrentColor = Grid->CurrentColorIndex;
	if (HUDWidget)
	{
		HUDWidget->SetSelectedColor(CurrentColor);
	}
}

void ABeanPlayerController::SetCurrentColor(int32 Index)
{
	if (!Grid)
	{
		return;
	}
	Grid->SetCurrentColor(Index);
	CurrentColor = Grid->CurrentColorIndex;
	if (HUDWidget)
	{
		HUDWidget->SetSelectedColor(CurrentColor);
	}
}

// ---------------------------------------------------------------------------
// Import: picker -> conversion preview -> apply
// ---------------------------------------------------------------------------

void ABeanPlayerController::ImportImage()
{
	if (bImportPickerOpen)
	{
		return;
	}

	bImportPickerOpen = true;

	// The picker is a blocking Win32 dialog, so it must not run inside a Slate button callback:
	// defer it to the next tick, after the click has been fully handled.
	GetWorld()->GetTimerManager().SetTimerForNextTick(this, &ABeanPlayerController::OpenImportPicker);
}

void ABeanPlayerController::OpenImportPicker()
{
#if PLATFORM_WINDOWS
	FocusBeanGameWindow(GetWorld());

	TCHAR FileNameBuffer[MAX_PATH] = { 0 };

	OPENFILENAMEW Dialog = {};
	Dialog.lStructSize = sizeof(Dialog);
	Dialog.hwndOwner = GetBeanGameWindowHandle(GetWorld());
	Dialog.lpstrFilter = BeanImageFileFilter;
	Dialog.lpstrFile = FileNameBuffer;
	Dialog.nMaxFile = MAX_PATH;
	Dialog.lpstrTitle = TEXT("选择要导入的图片");
	Dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER | OFN_NOCHANGEDIR;

	const BOOL bPicked = GetOpenFileNameW(&Dialog);

	// The dialog hands focus around; put the game's input mode back before anything else.
	ApplyCursorMode();
	bImportPickerOpen = false;

	if (!bPicked)
	{
		return;
	}
#else
	const FString PickedPath;
	bImportPickerOpen = false;
	if (HUDWidget)
	{
		HUDWidget->ShowStatus(TEXT("当前平台没有内置文件选择器，无法导入图片。"));
	}
	return;
#endif

#if PLATFORM_WINDOWS
	const FString PickedPath = FString(FileNameBuffer);
#endif

	FString Error;
	FImage Source;
	if (!FBeanImageConverter::LoadSourceImage(PickedPath, Source, Error))
	{
		// A bad or unsupported file must say so instead of disappearing into the log.
		if (HUDWidget)
		{
			HUDWidget->ShowStatus(Error, 8.0f);
		}
		return;
	}

	ImportSource = MoveTemp(Source);

	// Clear anything left over from a previous session, then take the pointer for the preview
	// panel - exactly like the export preview does. Without handing the mouse over, the panel is on
	// screen while the game still owns the cursor and every button in it looks dead.
	CloseImportSession();
	bImportPanelOpen = true;
	bShowCursorBeforeImport = bShowMouseCursor;
	bShowMouseCursor = true;
	ApplyCursorMode();
	SetPreviewInputLock(true);

	// 不裁切: the WHOLE picture is the subject, so no crop box is set at all. The converter fits it
	// into a canvas-aspect working image (the source keeps its proportions and the leftover band
	// stays transparent), which is what makes "keep all of it" expressible.
	ImportOptions = FBeanConvertOptions();
	ImportOptions.EmptyValue = ABeanGrid::EmptyCell;
	ImportOptions.bHasCrop = false;

	if (Grid)
	{
		ImportOptions.CanvasWidth = Grid->GetWidth();
		ImportOptions.CanvasHeight = Grid->GetHeight();
	}

	bImportSourceTransparent = false;
	for (const FColor& Pixel : ImportSource.AsBGRA8())
	{
		if (Pixel.A < 255)
		{
			bImportSourceTransparent = true;
			break;
		}
	}

	// Recompute the canvas size from the currently selected preset at the default resolution.
	SetImportResolution(ABeanGrid::GetResolutionOptions().Num() > 0 ? ABeanGrid::GetResolutionOptions().Last() : 256);
	RefreshImportPreview();
}

void ABeanPlayerController::CloseImportSession()
{
	bImportPanelOpen = false;
	// ImportResult deliberately survives: the right-hand 应用 / 应用并铺满 buttons act on the last
	// conversion after the panel is gone, so clearing it here would leave them reporting "还没有可
	// 应用的转化结果" the moment the player closes the import panel. CancelImage() is what discards it.
	ImportPreviewTexture = nullptr;
	ImportSourceTexture = nullptr;
	if (HUDWidget)
	{
		HUDWidget->HideImportPreview();
	}

	// Leaving the panel must always give the game back: the import preview borrows the mouse and
	// locks movement while it is open, and a panel that closes without undoing that leaves the
	// player unable to look around or place a single bead.
	SetPreviewInputLock(false);
	bShowMouseCursor = bShowCursorBeforeImport;
	ApplyCursorMode();
}

void ABeanPlayerController::RefreshImportPreview()
{
	if (!bImportPanelOpen || !HUDWidget)
	{
		return;
	}

	if (ImportSource.SizeX <= 0 || ImportSource.SizeY <= 0)
	{
		HUDWidget->SetImportPreviewError(TEXT("这张图片没有可用的像素数据。"));
		return;
	}

	const TArray<FBeanPaletteColor> Palette = FBeanPalette::LoadOrCreateDefault();

	FBeanConvertResult Result;
	if (!FBeanImageConverter::Convert(ImportSource, Palette, ImportOptions, Result))
	{
		ImportResult = FBeanConvertResult();
		ImportPreviewTexture = nullptr;
		HUDWidget->SetImportPreviewError(Result.Error.IsEmpty() ? TEXT("图片转化失败。") : Result.Error);
		return;
	}

	ImportResult = MoveTemp(Result);

	// Right-hand side: the converted pattern (already canvas-sized and on a light board).
	ImportPreviewTexture = FBeanAssets::CreateImageTexture(ImportResult.Preview);

	// Left-hand side: the cropped source region, so panning/zooming the crop is visible immediately.
	FImage SourcePreview;
	BuildCroppedSourcePreview(ImportSource, ImportOptions, SourcePreview);
	ImportSourceTexture = SourcePreview.SizeX > 0 ? FBeanAssets::CreateImageTexture(SourcePreview) : nullptr;

	const FString PresetLabel = ABeanGrid::GetCanvasPresetLabel(ImportPresetShown);
	const FString SourceLabel = ImportOptions.bHasCrop
		? FString::Printf(TEXT("裁剪 %d × %d"), ImportOptions.CropMaxX - ImportOptions.CropMinX + 1, ImportOptions.CropMaxY - ImportOptions.CropMinY + 1)
		: FString(TEXT("整张图片（缩放适配）"));

	const FString Info = FString::Printf(
		TEXT("画布 %d × %d（%s）· 原图 %d × %d · %s · 实际用色 %d 种 · 已填 %d / %d"),
		ImportOptions.CanvasWidth, ImportOptions.CanvasHeight, *PresetLabel,
		ImportSource.SizeX, ImportSource.SizeY,
		*SourceLabel,
		ImportResult.ColorCount, ImportResult.PlacedCells, ImportOptions.CanvasWidth * ImportOptions.CanvasHeight);

	FString Message;
	if (ImportResult.bHasTransparency)
	{
		Message = TEXT("原图有透明区域，透明处不会放置豆子（不参与完成度统计）。");
	}

	HUDWidget->ShowImportPreview(ImportSourceTexture, ImportPreviewTexture);
	HUDWidget->SetImportPreviewInfo(Info, Message, true, ImportOptions.bDither,
		ImportOptions.MaxColors, ImportOptions.CanvasWidth, ImportPresetShown);
}

void ABeanPlayerController::SetImportMaxColors(int32 MaxColors)
{
	ImportOptions.MaxColors = FMath::Max(0, MaxColors);
	RefreshImportPreview();
}

void ABeanPlayerController::ToggleImportDither()
{
	ImportOptions.bDither = !ImportOptions.bDither;
	RefreshImportPreview();
}

void ABeanPlayerController::SetImportCanvasPreset(EBeanCanvasPreset Preset)
{
	ImportPresetShown = Preset;
	ApplyCanvasSize();
	RefreshImportPreview();
}

void ABeanPlayerController::ApplyCanvasSize()
{
	const int32 Resolution = FMath::Max(1, ImportResolution);
	ABeanGrid::GetPresetDimensions(ImportPresetShown, Resolution, ImportOptions.CanvasWidth, ImportOptions.CanvasHeight);

	// There is no crop box to keep in step any more: the whole picture is fitted into whatever
	// canvas the player picks, so changing the shape or the resolution is all there is to do here.
}

void ABeanPlayerController::SetImportResolution(int32 Resolution)
{
	if (Resolution <= 0)
	{
		// 0 means "pick something sensible": keep the current one when it is already valid, else use
		// the largest offered option.
		const TArray<int32>& Options = ABeanGrid::GetResolutionOptions();
		if (Options.Num() == 0)
		{
			return;
		}

		// Ask the player which resolution to use; a cancel leaves everything as it was.
		TArray<FString> Labels;
		for (int32 Option : Options)
		{
			Labels.Add(FString::Printf(TEXT("%d × %d"), Option, Option));
		}
		const int32 Chosen = ShowOptionDialog(TEXT("选择画布分辨率"), Labels, Options, TArray<bool>());
		if (Chosen <= 0)
		{
			return;
		}
		Resolution = Chosen;
	}

	ImportResolution = Resolution;
	ApplyCanvasSize();
	RefreshImportPreview();
}

void ABeanPlayerController::ApplyImage()
{
	if (!Grid)
	{
		return;
	}

	// 应用 means "make this the target pattern": the player then fills it bead by bead, and the
	// completion statistics track the work against it. Nothing about the beads already placed is
	// destroyed, which is what makes cancel-safe import possible.
	if (ImportResult.Cells.Num() != ImportResult.Width * ImportResult.Height || ImportResult.Width <= 0)
	{
		if (HUDWidget)
		{
			HUDWidget->SetImportPreviewError(TEXT("还没有可应用的转化结果。"));
		}
		return;
	}

	// 应用 replaces the board: whatever beads were there are cleared first (m03538). ClearAllCells
	// pushes its own undo step, so Ctrl+Z still brings the previous work back bead for bead.
	Grid->ClearAllCells();

	Grid->SetTargetPattern(ImportResult.Width, ImportResult.Height, 0, 0, ImportResult.Cells, TArray<bool>());

	// Show it as the hint layer too, so the player can see the pattern on the board right away.
	Grid->BeginImagePreview(ImportResult.Cells, ImportResult.Width, ImportResult.Height);

	if (HUDWidget)
	{
		HUDWidget->ShowStatus(FString::Printf(TEXT("已应用图案：%d × %d，%d 种颜色，%d 个豆位（点「应用并铺满」可一键铺满画布）"),
			ImportResult.Width, ImportResult.Height, ImportResult.ColorCount, ImportResult.PlacedCells), 8.0f);
		HUDWidget->SetImportButtonsEnabled(true);
	}

	CloseImportSession();
	RefreshHUDState();
	MarkDirtyAndAutoSave();
}

void ABeanPlayerController::ApplyImageAndFill()
{
	// Same as ApplyImage, then the whole pattern is placed in one go. Filling is a batch operation,
	// so the entire placement is a single undo step and the renderer only refreshes the colours that
	// actually changed (never one rebuild per bead).
	ApplyImage();

	if (!Grid || !Grid->HasTargetPattern())
	{
		return;
	}

	const int32 Filled = Grid->ApplyBatch(EBeanBatchOp::FillFromPattern,
		0, 0, Grid->GetWidth() - 1, Grid->GetHeight() - 1, ABeanGrid::EmptyCell, ABeanGrid::EmptyCell);

	if (HUDWidget)
	{
		HUDWidget->ShowStatus(Filled > 0
			? FString::Printf(TEXT("已按图案铺满画布：放置 %d 个方块（可用 Ctrl+Z 撤销）。"), Filled)
			: TEXT("图案已经是当前的样子，没有方块需要放置。"), 8.0f);
	}

	RefreshHUDState();
	MarkDirtyAndAutoSave();
}

void ABeanPlayerController::CancelImage()
{
	if (Grid)
	{
		Grid->CancelImage();
	}
	CloseImportSession();
	// 取消 really means discard: unlike applying, cancelling throws the conversion away so the
	// right-hand buttons cannot put it back on the board afterwards.
	ImportResult = FBeanConvertResult();
	if (HUDWidget)
	{
		HUDWidget->SetImportButtonsEnabled(Grid && Grid->HasOverlay());
		HUDWidget->ShowStatus(TEXT("已取消导入，当前作品没有被修改。"));
	}
	RefreshHUDState();
}

void ABeanPlayerController::CommitImport()
{
	ApplyImage();
}

void ABeanPlayerController::CommitPattern()
{
	if (!Grid)
	{
		return;
	}

	if (ImportResult.Cells.Num() == ImportResult.Width * ImportResult.Height && ImportResult.Width > 0)
	{
		Grid->SetTargetPattern(ImportResult.Width, ImportResult.Height, 0, 0, ImportResult.Cells, TArray<bool>());
	}
	CloseImportSession();
	RefreshHUDState();
}

// ---------------------------------------------------------------------------
// Hint layer
// ---------------------------------------------------------------------------

void ABeanPlayerController::ChangeOverlayOpacity(float Delta)
{
	if (!Grid)
	{
		return;
	}
	Grid->SetOverlayOpacity(FMath::Clamp(Grid->GetOverlayOpacity() + Delta, 0.0f, 1.0f));
}

void ABeanPlayerController::ToggleOverlayVisible()
{
	if (Grid)
	{
		Grid->SetOverlayVisible(!Grid->IsOverlayVisible());
	}
}

// ---------------------------------------------------------------------------
// Export preview + saving
// ---------------------------------------------------------------------------

int32 ABeanPlayerController::ChooseDefaultExportScale(int32 CellsW, int32 CellsH) const
{
	const TArray<int32>& Options = ABeanGrid::GetExportScaleOptions();
	if (Options.Num() == 0)
	{
		return 1;
	}

	// Largest offered scale whose preview stays comfortable on screen.
	for (int32 Index = Options.Num() - 1; Index >= 0; --Index)
	{
		const int32 Option = Options[Index];
		if (CellsW * Option <= BeanExportPreviewComfort && CellsH * Option <= BeanExportPreviewComfort)
		{
			return Option;
		}
	}
	return Options[0];
}

bool ABeanPlayerController::RefreshExportPreview(int32 Scale)
{
	if (!Grid || !HUDWidget)
	{
		return false;
	}

	int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
	if (!Grid->GetContentBounds(MinX, MinY, MaxX, MaxY))
	{
		HUDWidget->SetExportPreviewMessage(TEXT("画布上还没有豆子，先拼一点再导出。"));
		return false;
	}

	const int32 CellsW = MaxX - MinX + 1;
	const int32 CellsH = MaxY - MinY + 1;
	if (CellsW * Scale > ABeanGrid::MaxExportSize || CellsH * Scale > ABeanGrid::MaxExportSize)
	{
		HUDWidget->SetExportPreviewMessage(FString::Printf(
			TEXT("这个比例会得到 %d × %d 像素，超过 %d 的限制，请换一个小一点的比例。"),
			CellsW * Scale, CellsH * Scale, ABeanGrid::MaxExportSize));
		return false;
	}

	FImage Image;
	FString Error;
	if (!Grid->BuildExportImage(Scale, Image, Error, true))
	{
		HUDWidget->SetExportPreviewMessage(Error.IsEmpty() ? TEXT("生成预览失败。") : Error);
		return false;
	}

	ExportPreviewTexture = FBeanAssets::CreateImageTexture(Image);
	if (!ExportPreviewTexture)
	{
		HUDWidget->SetExportPreviewMessage(TEXT("无法创建预览纹理。"));
		return false;
	}

	ExportPreviewScale = Scale;
	HUDWidget->ShowExportPreview(ExportPreviewTexture, Scale, CellsW, CellsH);
	return true;
}

void ABeanPlayerController::ExportImage()
{
	if (!Grid || !HUDWidget)
	{
		return;
	}

	int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
	if (!Grid->GetContentBounds(MinX, MinY, MaxX, MaxY))
	{
		HUDWidget->ShowStatus(TEXT("画布上还没有豆子，先拼一点再导出。"));
		return;
	}

	const int32 DefaultScale = ChooseDefaultExportScale(MaxX - MinX + 1, MaxY - MinY + 1);
	if (!RefreshExportPreview(DefaultScale))
	{
		return;
	}

	bExportPreviewOpen = true;
	bShowCursorBeforeExport = bShowMouseCursor;
	bShowMouseCursor = true;
	ApplyCursorMode();
	SetPreviewInputLock(true);
}

void ABeanPlayerController::SetExportPreviewScale(int32 PixelsPerCell)
{
	if (!bExportPreviewOpen)
	{
		return;
	}
	RefreshExportPreview(PixelsPerCell);
}

void ABeanPlayerController::SaveExportImage()
{
	if (!Grid || !HUDWidget || bExportDialogOpen)
	{
		return;
	}

	bExportDialogOpen = true;
	// The save dialog is blocking, so it has to run outside the button callback.
	GetWorld()->GetTimerManager().SetTimerForNextTick(this, &ABeanPlayerController::OpenExportSaveDialog);
}

void ABeanPlayerController::OpenExportSaveDialog()
{
#if PLATFORM_WINDOWS
	FocusBeanGameWindow(GetWorld());

	// Default to the user's Pictures folder, which is where a picture is actually findable.
	FString DefaultDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("BeanExports"));
	const FString PicturesDir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformProcess::UserHomeDir(), TEXT("Pictures")));
	if (FPaths::DirectoryExists(PicturesDir))
	{
		DefaultDir = PicturesDir;
	}

	const FString DefaultName = FString::Printf(TEXT("bean_%s.png"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	const FString DefaultFile = FPaths::Combine(DefaultDir, DefaultName);

	TCHAR FileNameBuffer[MAX_PATH] = { 0 };
	FCString::Strncpy(FileNameBuffer, *DefaultFile, MAX_PATH);

	OPENFILENAMEW Dialog = {};
	Dialog.lStructSize = sizeof(Dialog);
	Dialog.hwndOwner = GetBeanGameWindowHandle(GetWorld());
	Dialog.lpstrFilter = BeanPngFileFilter;
	Dialog.lpstrFile = FileNameBuffer;
	Dialog.nMaxFile = MAX_PATH;
	Dialog.lpstrTitle = TEXT("保存作品图片");
	Dialog.lpstrDefExt = TEXT("png");
	Dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;

	const BOOL bPicked = GetSaveFileNameW(&Dialog);
	ApplyCursorMode();
	bExportDialogOpen = false;

	if (!bPicked)
	{
		if (HUDWidget)
		{
			HUDWidget->SetExportPreviewMessage(TEXT("已取消保存，可以换个比例再看，或者返回继续拼豆。"));
		}
		return;
	}

	const FString FilePath = FString(FileNameBuffer);
#else
	bExportDialogOpen = false;
	if (HUDWidget)
	{
		HUDWidget->SetExportPreviewMessage(TEXT("当前平台没有内置保存对话框。"));
	}
	return;
#endif

#if PLATFORM_WINDOWS
	FString Error;
	if (!Grid->SaveImageToPng(ExportPreviewScale, FilePath, Error, true))
	{
		// A failed write must be reported, never silently swallowed or shown as success.
		if (HUDWidget)
		{
			HUDWidget->SetExportPreviewMessage(Error.IsEmpty() ? TEXT("保存失败。") : Error);
		}
		return;
	}

	CloseExportPreview();
	if (HUDWidget)
	{
		HUDWidget->ShowStatus(FString::Printf(TEXT("已导出 %s"), *FPaths::GetCleanFilename(FilePath)));
	}
#endif
}

void ABeanPlayerController::CloseExportPreview()
{
	if (HUDWidget)
	{
		HUDWidget->HideExportPreview();
	}

	bExportPreviewOpen = false;
	SetPreviewInputLock(false);
	bShowMouseCursor = bShowCursorBeforeExport;
	ApplyCursorMode();
}

// ---------------------------------------------------------------------------
// Panels: lock / unlock game input
// ---------------------------------------------------------------------------

bool ABeanPlayerController::IsAnyPanelOpen() const
{
	return bImportPanelOpen
		|| bExportPreviewOpen
		|| bGalleryOpen
		|| bScreenshotModeOpen
		|| bSaveWorkDialogOpen
		|| bOptionDialogOpen
		|| bExportDialogOpen
		|| bScreenshotDialogOpen
		|| bImportPickerOpen;
}

void ABeanPlayerController::SetPreviewInputLock(bool bLocked)
{
	if (bLocked)
	{
		SetIgnoreMoveInput(true);
		SetIgnoreLookInput(true);
		if (PawnChar)
		{
			PawnChar->GetCharacterMovement()->StopMovementImmediately();
		}
	}
	else
	{
		ResetIgnoreMoveInput();
		ResetIgnoreLookInput();
	}
}

// ---------------------------------------------------------------------------
// Work management: save as / gallery / screenshot
// ---------------------------------------------------------------------------

void ABeanPlayerController::RequestNewCanvas()
{
	// Deferred one tick, exactly like SaveWorkAs: the resolution dialog is a modal Slate window and
	// running it from inside a button callback would re-enter the HUD's own click handling.
	GetWorld()->GetTimerManager().SetTimerForNextTick(this, &ABeanPlayerController::NewCanvasFromMenu);
}

void ABeanPlayerController::SaveWork()
{
	if (!Grid || !Grid->IsReady())
	{
		if (HUDWidget)
		{
			HUDWidget->ShowStatus(TEXT("画布还没准备好，暂时不能保存。"), 5.0f);
		}
		return;
	}

	// The quiet save. It writes to the slot the work already lives in, so nothing has to be typed and
	// nothing can be overwritten by accident; "另存作品" stays the one that asks for a name.
	Grid->SaveGame();

	const bool bFailed = Grid->DidLastSaveFail();
	if (!bFailed)
	{
		WriteWorkThumbnail(Grid->GetWorkName(), Grid->GetWorkName());
		// A save changes the row's time, size and thumbnail, so a gallery that is already on screen
		// would otherwise keep showing the state from before the save.
		if (bGalleryOpen)
		{
			RefreshGalleryRows();
		}
	}

	if (HUDWidget)
	{
		const FString Message = Grid->GetSaveStatusMessage();
		HUDWidget->ShowStatus(!Message.IsEmpty()
			? Message
			: (bFailed
				? FString(TEXT("保存失败：存档写不进去，请查看 Output Log。"))
				: FString::Printf(TEXT("已保存作品「%s」。"), *Grid->GetWorkName())), 8.0f);
	}
}

void ABeanPlayerController::HandleSaveKey()
{
	// Bound to S, which is also strafe left; the live Ctrl state decides what the press means, so a
	// bare S keeps walking (same trick as the undo/redo bindings on Z and Y).
	if (!IsInputKeyDown(EKeys::LeftControl) && !IsInputKeyDown(EKeys::RightControl))
	{
		return;
	}
	SaveWork();
}

void ABeanPlayerController::SaveWorkWithNamePrompt()
{
	// "保存作品" asks for the name before it writes (m05089): the prompt is pre-filled with the name
	// the work already has, so pressing Enter saves it in place while typing another name saves it
	// there. Ctrl+S keeps skipping the prompt for a quick save.
	if (!Grid || bSaveWorkDialogOpen)
	{
		return;
	}

	bSaveWorkDialogSaveCopy = false;
	bSaveWorkDialogOpen = true;
	GetWorld()->GetTimerManager().SetTimerForNextTick(this, &ABeanPlayerController::OpenSaveWorkDialog);
}

void ABeanPlayerController::SaveWorkAs()
{
	if (!Grid || bSaveWorkDialogOpen)
	{
		return;
	}

	bSaveWorkDialogSaveCopy = true;
	bSaveWorkDialogOpen = true;
	GetWorld()->GetTimerManager().SetTimerForNextTick(this, &ABeanPlayerController::OpenSaveWorkDialog);
}

void ABeanPlayerController::OpenSaveWorkDialog()
{
	bSaveWorkDialogOpen = false;
	if (!Grid)
	{
		return;
	}

	// Every Slate dialog borrows the pointer for as long as it is up. Without this, the window is
	// drawn on top of a game that still captures the mouse, so its buttons never see a click
	// (m03538: 保存作品/选择分辨率 dialogs were unusable).
	const bool bCursorWasShown = bShowMouseCursor;
	bShowMouseCursor = true;
	ApplyCursorMode();
	auto RestoreCursor = [this, bCursorWasShown]()
	{
		bShowMouseCursor = bCursorWasShown;
		ApplyCursorMode();
	};

	FString Name = Grid->GetWorkName();
	if (Name.IsEmpty())
	{
		Name = FString::Printf(TEXT("拼豆作品_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	}

	// "另存作品" starts from a copy name so that accepting the default cannot overwrite the work
	// being edited; "保存作品" starts from the current name so Enter simply saves it in place (m05089).
	if (bSaveWorkDialogSaveCopy)
	{
		Name = FString::Printf(TEXT("%s 副本"), *Name);
	}
	bSaveWorkDialogSaveCopy = false;

	const FString DefaultName = Name;
	if (!ShowTextPrompt(TEXT("保存作品"), DefaultName, Name))
	{
		RestoreCursor();
		return;
	}

	const FString SlotName = ABeanGrid::SanitizeSlotName(Name);
	if (SlotName.IsEmpty())
	{
		if (HUDWidget)
		{
			HUDWidget->ShowStatus(TEXT("作品名不能为空。"));
		}
		RestoreCursor();
		return;
	}

	if (ABeanGrid::DoesSlotExist(SlotName) && SlotName != ABeanGrid::SanitizeSlotName(Grid->GetWorkName()))
	{
		const TArray<int32> Values{ 1 };
		const int32 Chosen = ShowOptionDialog(
			FString::Printf(TEXT("已存在同名作品「%s」"), *Name),
			TArray<FString>{ TEXT("覆盖保存"), TEXT("取消") },
			TArray<int32>{ 1, 0 },
			TArray<bool>());
		if (Chosen != 1)
		{
			RestoreCursor();
			return;
		}
	}

	FString Error;
	const bool bSaved = Grid->SaveGameAs(SlotName, Error);
	RestoreCursor();
	if (HUDWidget)
	{
		HUDWidget->ShowStatus(bSaved
			? FString::Printf(TEXT("已保存作品「%s」"), *Name)
			: (Error.IsEmpty() ? TEXT("保存失败。") : Error), 8.0f);
	}

	if (bSaved)
	{
		// A thumbnail makes the gallery usable without loading every work. It is keyed on the slot
		// the save actually went to, and on the name stored inside that save, so a later rename can
		// always find and move this exact file.
		WriteWorkThumbnail(SlotName, Name);

		// Saving from inside the gallery must not leave the list looking like it did before the save.
		if (bGalleryOpen)
		{
			RefreshGalleryRows();
		}
	}
}

void ABeanPlayerController::NewCanvasFromMenu()
{
	if (!Grid)
	{
		return;
	}

	// The options dialog doubles as the confirmation: picking a size means "start over", cancelling
	// keeps the current work. Nothing can be lost here, because the work being replaced was already
	// autosaved and stays in the gallery under its own name.
	const TArray<int32>& Options = ABeanGrid::GetResolutionOptions();
	if (Options.Num() == 0)
	{
		return;
	}

	TArray<FString> Labels;
	for (int32 Option : Options)
	{
		Labels.Add(FString::Printf(TEXT("%d × %d"), Option, Option));
	}

	const int32 Chosen = ShowOptionDialog(TEXT("新建画布（当前作品已自动保存）"), Labels, Options, TArray<bool>());
	if (Chosen <= 0)
	{
		return;
	}

	FString Error;
	if (!Grid->NewCanvas(Chosen, Chosen, Error))
	{
		if (HUDWidget)
		{
			HUDWidget->ShowStatus(Error.IsEmpty() ? TEXT("新建画布失败。") : Error, 8.0f);
		}
		return;
	}

	// The marquee, the hint layer and the previous history are all gone with the old work, so the
	// HUD has to be rebuilt from scratch rather than patched.
	if (HUDWidget)
	{
		HUDWidget->SetSelectionPanelVisible(false, false, FString());
		HUDWidget->ShowStatus(FString::Printf(TEXT("已新建 %d × %d 画布「%s」"), Chosen, Chosen, *Grid->GetWorkName()), 8.0f);
	}
	RefreshHUDState();
	RefreshGalleryRows();
}

void ABeanPlayerController::ExportWorkFromGallery(const FString& SlotName)
{
	if (!HUDWidget)
	{
		return;
	}

	UBeanGridSaveData* Save = Cast<UBeanGridSaveData>(UGameplayStatics::LoadGameFromSlot(SlotName, 0));
	if (!Save || Save->Cells.Num() == 0)
	{
		HUDWidget->SetGalleryMessage(TEXT("这个存档读不出来，无法导出图片。"));
		return;
	}

	// Export through a scratch grid so the saved work never touches the grid the player is editing,
	// and so the picture comes out of exactly the same code path the in-game export preview uses.
	ABeanGrid* Scratch = GetWorld() ? GetWorld()->SpawnActor<ABeanGrid>() : nullptr;
	if (!Scratch)
	{
		HUDWidget->SetGalleryMessage(TEXT("导出失败：无法创建临时画布。"));
		return;
	}

	// One exit point, so the temporary actor can never be leaked by an early return.
	auto CleanUp = [Scratch]()
	{
		if (IsValid(Scratch))
		{
			Scratch->Destroy();
		}
	};

	Scratch->ApplySaveData(*Save);
	if (!Scratch->IsReady())
	{
		HUDWidget->SetGalleryMessage(TEXT("导出失败：存档数据不合法。"));
		CleanUp();
		return;
	}

	// Pictures next to the user's other images when there is a Pictures folder, else the project's
	// own Saved folder. A unique suffix means an export can never overwrite an earlier one.
	FString Directory = FPaths::Combine(FPlatformProcess::UserHomeDir(), TEXT("Pictures"));
	if (!FPaths::DirectoryExists(Directory))
	{
		Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BeanExports"));
	}
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*Directory))
	{
		PlatformFile.CreateDirectoryTree(*Directory);
	}

	const FString WorkName = Save->WorkName.IsEmpty() ? SlotName : Save->WorkName;
	const FString FileName = FString::Printf(TEXT("%s_%s.png"),
		*ABeanGrid::SanitizeFileName(WorkName), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	const FString FullPath = FPaths::Combine(Directory, FileName);

	FString Error;
	if (Scratch->SaveImageToPng(8, FullPath, Error, false))
	{
		HUDWidget->SetGalleryMessage(FString::Printf(TEXT("已导出作品「%s」到 %s"), *WorkName, *FullPath));
	}
	else
	{
		HUDWidget->SetGalleryMessage(Error.IsEmpty() ? TEXT("导出失败。") : Error);
	}

	CleanUp();
}

void ABeanPlayerController::WriteWorkThumbnail(const FString& SlotName, const FString& WorkName)
{
	if (!Grid)
	{
		return;
	}

	FImage Thumb;
	if (!Grid->BuildThumbnailImage(BeanThumbnailMaxEdge, Thumb))
	{
		return;
	}

	// The path is derived from the slot, but the picture written is the work the controller has in
	// front of it, which is exactly the one that was just saved into that slot.
	const FString ThumbPath = FPaths::Combine(ABeanGrid::GetSaveDirectory(), MakeThumbnailFileName(SlotName));
	const FString Directory = FPaths::GetPath(ThumbPath);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*Directory))
	{
		PlatformFile.CreateDirectoryTree(*Directory);
	}

	if (!FImageUtils::SaveImageByExtension(*ThumbPath, Thumb))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Bean] Could not write the thumbnail of work '%s' to '%s'"), *WorkName, *ThumbPath);
	}
}

void ABeanPlayerController::OpenGallery()
{
	if (!HUDWidget)
	{
		return;
	}

	if (bGalleryOpen)
	{
		// Closing: hand the game its input back.
		bGalleryOpen = false;
		HUDWidget->HideGallery();
		SetPreviewInputLock(false);
		bShowMouseCursor = bShowCursorBeforeGallery;
		ApplyCursorMode();
		return;
	}

	RefreshGalleryRows();

	bGalleryOpen = true;
	bShowCursorBeforeGallery = bShowMouseCursor;
	bShowMouseCursor = true;
	ApplyCursorMode();
	SetPreviewInputLock(true);
	HUDWidget->ShowGallery();
}

void ABeanPlayerController::RefreshGalleryRows()
{
	if (!HUDWidget)
	{
		return;
	}

	const FString SaveDir = ABeanGrid::GetSaveDirectory();
	TArray<FString> FileNames;
	TArray<FString> SlotNames;
	TArray<FString> WorkNames;
	TArray<UTexture2D*> Thumbnails;
	TArray<FString> MetaLines;
	TArray<bool> Readable;
	TArray<FString> Problems;

	// Slot names are collected separately from the file names being iterated below, and the save time
	// is kept alongside them so the list can be shown newest first.
	TArray<FString> RowSlotNames;
	TArray<int64> SortTicks;

	// The work the player is editing right now, so the list can say which row that is.
	const FString CurrentSlot = Grid ? ABeanGrid::SanitizeSlotName(Grid->GetWorkName()) : FString();

	IFileManager::Get().FindFiles(FileNames, *(SaveDir / TEXT("*.sav")), true, false);
	FileNames.Sort();

	for (const FString& FileName : FileNames)
	{
		const FString SlotName = FPaths::GetBaseFilename(FileName, false);

		// Internal slots (the performance/automation harnesses write "__BeanPerf_*" and
		// "__BeanPerfTest_*") are tooling, not the player's works. Hide them: a run of the harness
		// used to leave rows in the gallery that the player then had to delete by hand.
		if (SlotName.StartsWith(TEXT("__")))
		{
			continue;
		}

		UBeanGridSaveData* Save = Cast<UBeanGridSaveData>(UGameplayStatics::LoadGameFromSlot(SlotName, 0));

		const FString WorkName = (Save && !Save->WorkName.IsEmpty()) ? Save->WorkName : SlotName;

		// A work that cannot be read stays visible and says why: silently hiding it would look like
		// the save was deleted.
		FString Problem;
		bool bRowReadable = Save != nullptr;
		if (!Save)
		{
			Problem = TEXT("存档损坏或无法读取，已跳过。");
		}
		else if (Save->Version > UBeanGridSaveData::CurrentVersion)
		{
			Problem = FString::Printf(TEXT("存档版本 %d 高于当前支持的 %d，可能无法打开。"), Save->Version, UBeanGridSaveData::CurrentVersion);
			bRowReadable = false;
		}
		else if (Save->Cells.Num() != Save->Width * Save->Height)
		{
			Problem = TEXT("存档尺寸与数据不一致，已跳过。");
			bRowReadable = false;
		}
		else if (Save->bWasRecoveredFromBackup)
		{
			Problem = TEXT("上次保存中断，已从备份恢复。");
		}

		const FString Meta = Save
			? FString::Printf(TEXT("%d × %d · %s%s"), Save->Width, Save->Height,
				Save->SavedUtcTicks > 0 ? *FDateTime(Save->SavedUtcTicks).ToString(TEXT("%Y-%m-%d %H:%M")) : TEXT("未知时间"),
				SlotName == CurrentSlot ? TEXT(" · 正在编辑") : TEXT(""))
			: FString::Printf(TEXT("文件：%s"), *FileName);

		UTexture2D* Thumbnail = nullptr;
		if (Save)
		{
			const FString ThumbPath = FPaths::Combine(SaveDir, MakeThumbnailFileName(SlotName));
			FImage ThumbImage;
			if (FPaths::FileExists(ThumbPath) && FImageUtils::LoadImage(*ThumbPath, ThumbImage))
			{
				Thumbnail = FBeanAssets::CreateImageTexture(ThumbImage);
			}
			else if (Problem.IsEmpty())
			{
				Problem = TEXT("没有缩略图。");
			}
		}

		// Row names go into their own array: the loop above iterates the file-name array, so writing
		// the sanitized slot name back into it would hand a later iteration a slot name where it
		// expects a file name.
		RowSlotNames.Add(SlotName);
		SortTicks.Add(Save ? Save->SavedUtcTicks : 0);
		WorkNames.Add(WorkName);
		Thumbnails.Add(Thumbnail);
		MetaLines.Add(Meta);
		Readable.Add(bRowReadable);
		Problems.Add(Problem);
	}

	// Row actions: opening / renaming / deleting a work all go through this controller.
	HUDWidget->GalleryActions.OnOpen = [this](const FString& SlotName)
	{
		LoadWorkFromGallery(SlotName);
	};
	HUDWidget->GalleryActions.OnRename = [this](const FString& SlotName)
	{
		RenameWorkFromGallery(SlotName);
	};
	HUDWidget->GalleryActions.OnDelete = [this](const FString& SlotName)
	{
		DeleteWorkFromGallery(SlotName);
	};
	HUDWidget->GalleryActions.OnExport = [this](const FString& SlotName)
	{
		ExportWorkFromGallery(SlotName);
	};

	// Newest first: the work that was just saved is the one the player is most likely to want next.
	// Sorting the file names (what this used to do) left it wherever the alphabet put it.
	TArray<int32> Order;
	Order.Reserve(RowSlotNames.Num());
	for (int32 Index = 0; Index < RowSlotNames.Num(); ++Index)
	{
		Order.Add(Index);
	}
	Order.Sort([&SortTicks](const int32 A, const int32 B)
	{
		return SortTicks[A] > SortTicks[B];
	});

	TArray<FString> SortedSlotNames;
	TArray<FString> SortedWorkNames;
	TArray<UTexture2D*> SortedThumbnails;
	TArray<FString> SortedMetaLines;
	TArray<bool> SortedReadable;
	TArray<FString> SortedProblems;
	for (const int32 Index : Order)
	{
		SortedSlotNames.Add(RowSlotNames[Index]);
		SortedWorkNames.Add(WorkNames[Index]);
		SortedThumbnails.Add(Thumbnails[Index]);
		SortedMetaLines.Add(MetaLines[Index]);
		SortedReadable.Add(Readable[Index]);
		SortedProblems.Add(Problems[Index]);
	}

	HUDWidget->SetGalleryRows(SortedSlotNames, SortedWorkNames, SortedThumbnails, SortedMetaLines, SortedReadable, SortedProblems);
}

void ABeanPlayerController::LoadWorkFromGallery(const FString& SlotName)
{
	if (!Grid || !HUDWidget)
	{
		return;
	}

	// A work with unsaved edits may not be silently thrown away.
	if (Grid->IsDirty())
	{
		const int32 Chosen = ShowOptionDialog(
			TEXT("当前作品还没保存"),
			TArray<FString>{ TEXT("先保存再打开"), TEXT("放弃修改并打开"), TEXT("取消") },
			TArray<int32>{ 1, 2, 0 },
			TArray<bool>());
		if (Chosen == 0)
		{
			return;
		}
		if (Chosen == 1)
		{
			Grid->SaveGame();
			// The thumbnail belongs to the work being saved, which is still the current one: it is
			// written before the load below replaces it.
			WriteWorkThumbnail(Grid->GetWorkName(), Grid->GetWorkName());
		}
	}

	FString Error;
	if (!Grid->LoadGameFrom(SlotName, Error))
	{
		HUDWidget->SetGalleryMessage(Error.IsEmpty() ? TEXT("加载失败。") : Error);
		return;
	}

	// Refresh the newly opened work's thumbnail from what was actually loaded, so the gallery row
	// always shows the state that is stored (including a save that was recovered from its backup).
	WriteWorkThumbnail(SlotName, Grid->GetWorkName());

	RefreshGalleryRows();
	RefreshHUDState();
	HUDWidget->SetGalleryMessage(FString::Printf(TEXT("已打开作品「%s」"), *Grid->GetWorkName()));
	HUDWidget->ShowStatus(FString::Printf(TEXT("已打开作品「%s」"), *Grid->GetWorkName()));
}

void ABeanPlayerController::RenameWorkFromGallery(const FString& SlotName)
{
	if (!Grid || !HUDWidget)
	{
		return;
	}

	UBeanGridSaveData* Save = Cast<UBeanGridSaveData>(UGameplayStatics::LoadGameFromSlot(SlotName, 0));
	if (!Save)
	{
		HUDWidget->SetGalleryMessage(TEXT("这个存档读不出来，无法重命名。"));
		return;
	}

	FString NewName = Save->WorkName.IsEmpty() ? SlotName : Save->WorkName;
	if (!ShowTextPrompt(TEXT("重命名作品"), NewName, NewName))
	{
		return;
	}

	const FString NewSlot = ABeanGrid::SanitizeSlotName(NewName);
	if (NewSlot.IsEmpty())
	{
		HUDWidget->SetGalleryMessage(TEXT("作品名不能为空。"));
		return;
	}

	// Renaming keeps the same slot: the work keeps its data, its thumbnail and its identity, and
	// nothing is copied or deleted. It also goes through the same atomic writer as a normal save,
	// so an interrupted rename cannot destroy the file it is renaming.
	FString RenameError;
	if (!ABeanGrid::RenameStoredWork(SlotName, NewName, RenameError))
	{
		HUDWidget->SetGalleryMessage(RenameError);
		return;
	}

	RefreshGalleryRows();
	HUDWidget->SetGalleryMessage(FString::Printf(TEXT("已重命名为「%s」"), *NewName));
}

void ABeanPlayerController::DeleteWorkFromGallery(const FString& SlotName)
{
	if (!HUDWidget)
	{
		return;
	}

	if (SlotName == ABeanGrid::SanitizeSlotName(Grid ? Grid->GetWorkName() : FString()))
	{
		HUDWidget->SetGalleryMessage(TEXT("这是当前正在编辑的作品，请先另存为别的名字再删除。"));
		return;
	}

	// Deleting is irreversible from here, so it always asks first.
	const int32 Chosen = ShowOptionDialog(
		FString::Printf(TEXT("删除作品「%s」？"), *SlotName),
		TArray<FString>{ TEXT("删除"), TEXT("取消") },
		TArray<int32>{ 1, 0 },
		TArray<bool>());
	if (Chosen != 1)
	{
		HUDWidget->SetGalleryMessage(TEXT("已取消删除。"));
		return;
	}

	// The slot file, its backup and its temp file are all deleted together, so a work deleted here
	// cannot reappear as a \"recovered\" row if the same slot name is ever used again.
	FString DeleteError;
	const bool bRemoved = ABeanGrid::DeleteStoredWork(SlotName, DeleteError);

	if (bRemoved)
	{
		// Delete the thumbnail too, so a later work with the same name cannot inherit a stale picture.
		const FString ThumbPath = FPaths::Combine(ABeanGrid::GetSaveDirectory(), MakeWorkFileName(SlotName, TEXT("_thumb")));
		if (FPaths::FileExists(ThumbPath))
		{
			IFileManager::Get().Delete(*ThumbPath, false, true, true);
		}
	}

	RefreshGalleryRows();
	HUDWidget->SetGalleryMessage(bRemoved
		? FString::Printf(TEXT("已删除作品「%s」"), *SlotName)
		: DeleteError);
}

void ABeanPlayerController::OpenScreenshotMode()
{
	if (!HUDWidget)
	{
		return;
	}

	if (bScreenshotModeOpen)
	{
		bScreenshotModeOpen = false;
		bScreenshotUIHidden = false;
		HUDWidget->HideScreenshotMode();
		HUDWidget->SetHUDVisible(true);
		SetPreviewInputLock(false);
		bShowMouseCursor = bShowCursorBeforeGallery;
		ApplyCursorMode();
		return;
	}

	bScreenshotModeOpen = true;
	bScreenshotUIHidden = false;
	bShowCursorBeforeGallery = bShowMouseCursor;
	bShowMouseCursor = true;
	ApplyCursorMode();
	SetPreviewInputLock(true);
	HUDWidget->ShowScreenshotMode();
	RefreshScreenshotStatus();
}

void ABeanPlayerController::SetScreenshotStyle(int32 StyleIndex)
{
	if (StyleIndex < 0)
	{
		ScreenshotStyle = (ScreenshotStyle + 1) % 3;
	}
	else
	{
		ScreenshotStyle = StyleIndex % 3;
	}

	RefreshScreenshotStatus();
}

void ABeanPlayerController::RefreshScreenshotStatus()
{
	if (!HUDWidget || !Grid)
	{
		return;
	}

	FString StyleText;
	switch (ScreenshotStyle)
	{
	case 0: StyleText = TEXT("风格：纯作品（隐藏提示层）"); break;
	case 1: StyleText = TEXT("风格：带提示层（对照图案）"); break;
	default: StyleText = TEXT("风格：当前游戏画面"); break;
	}

	// Style 0 / 1 are about the hint layer, which the grid already owns: the display mode never
	// writes work data, it only changes what is shown.
	if (ScreenshotStyle == 0)
	{
		Grid->SetOverlayVisible(false);
	}
	else if (ScreenshotStyle == 1)
	{
		Grid->SetOverlayVisible(true);
	}

	HUDWidget->SetScreenshotStatus(StyleText, bScreenshotUIHidden
		? TEXT("已隐藏界面：按 F9 或点「隐藏界面」可以恢复。截图会保存到本地。")
		: TEXT("调整好角度后按「截图保存」。截图会保存到本地。"));
}

void ABeanPlayerController::ToggleScreenshotUI()
{
	if (!HUDWidget)
	{
		return;
	}

	bScreenshotUIHidden = !bScreenshotUIHidden;
	HUDWidget->SetScreenshotUIHidden(bScreenshotUIHidden);
	RefreshScreenshotStatus();
}

void ABeanPlayerController::CaptureScreenshot()
{
	if (!HUDWidget || bScreenshotDialogOpen)
	{
		return;
	}

	// Capture what is on screen right now (which is what the display mode is for), write it to the
	// screenshots folder through the engine's own screenshot path, and report the file name.
	FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Screenshots"));
	const FString PicturesDir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformProcess::UserHomeDir(), TEXT("Pictures")));
	if (FPaths::DirectoryExists(PicturesDir))
	{
		Directory = FPaths::Combine(PicturesDir, TEXT("BeanScreenshots"));
	}

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*Directory))
	{
		PlatformFile.CreateDirectoryTree(*Directory);
	}

	const FString BaseName = FString::Printf(TEXT("bean_%s.png"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	const FString FilePath = FPaths::Combine(Directory, BaseName);

	// The engine's screenshot path is asynchronous and never overwrites: it is the same mechanism
	// the editor's own screenshot key uses, so nothing here can leave a half-written file behind.
	// bShowUI = false keeps the HUD out of the frame.
	FScreenshotRequest::RequestScreenshot(FilePath, false, true);

	HUDWidget->SetScreenshotStatus(
		ScreenshotStyle == 0 ? TEXT("风格：纯作品（隐藏提示层）") : (ScreenshotStyle == 1 ? TEXT("风格：带提示层（对照图案）") : TEXT("风格：当前游戏画面")),
		FString::Printf(TEXT("已截图：%s（保存在 %s）"), *BaseName, *Directory));

	HUDWidget->ShowStatus(FString::Printf(TEXT("已截图 %s"), *BaseName));
}

// ---------------------------------------------------------------------------
// Screenshot save dialog (unused hook kept for a future fixed-resolution capture)
// ---------------------------------------------------------------------------

void ABeanPlayerController::OpenScreenshotSaveDialog()
{
	bScreenshotDialogOpen = false;
}

// ---------------------------------------------------------------------------
// Input handlers: camera / display / save
// ---------------------------------------------------------------------------

void ABeanPlayerController::OnMoveForward(const FInputActionValue& Value)
{
	// The pawn is looked up here instead of relying on a Tick-cached pointer: movement input must
	// not stop working just because Tick has not run yet (which is exactly what a panel that was
	// left open does, since Tick skips UpdateTarget while one is up).
	ABeanCharacter* Mover = PawnChar ? PawnChar.Get() : Cast<ABeanCharacter>(GetPawn());
	const float Axis = Value.Get<float>();
	if (Mover && FMath::Abs(Axis) > KINDA_SMALL_NUMBER && !IsAnyPanelOpen())
	{
		Mover->AddMovementInput(GetControlRotation().Vector(), Axis);
	}
}

void ABeanPlayerController::OnMoveRight(const FInputActionValue& Value)
{
	ABeanCharacter* Mover = PawnChar ? PawnChar.Get() : Cast<ABeanCharacter>(GetPawn());
	const float Axis = Value.Get<float>();
	if (Mover && FMath::Abs(Axis) > KINDA_SMALL_NUMBER && !IsAnyPanelOpen())
	{
		Mover->AddMovementInput(FRotationMatrix(GetControlRotation()).GetUnitAxis(EAxis::Y), Axis);
	}
}

void ABeanPlayerController::OnLook(const FInputActionValue& Value)
{
	if (IsAnyPanelOpen() || bShowMouseCursor)
	{
		return;
	}

	// Mouse up must look up (the axis reports positive downward), so the pitch is negated; yaw
	// keeps the axis' own direction. LookSensitivity is unchanged from the original build.
	const FVector2D Axis = Value.Get<FVector2D>();
	AddYawInput(Axis.X * LookSensitivity);
	AddPitchInput(-Axis.Y * LookSensitivity);
}

void ABeanPlayerController::OnCycleColor(const FInputActionValue& Value)
{
	if (IsAnyPanelOpen() || bShowMouseCursor)
	{
		return;
	}

	// The wheel reports a signed delta; only its direction is used so one notch is one colour.
	const float Axis = Value.Get<float>();
	if (FMath::Abs(Axis) < KINDA_SMALL_NUMBER)
	{
		return;
	}
	CycleColor(Axis > 0.0f ? -1 : 1);
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
	// Started and Completed are both bound: the same handler reads the action's live bool.
	const bool bHeld = Value.Get<bool>();
	bFlyUpHeld = bHeld;
	if (PawnChar)
	{
		PawnChar->SetFlyVerticalInput(bFlyUpHeld, bFlyDownHeld);
	}
}

void ABeanPlayerController::OnFlyDown(const FInputActionValue& Value)
{
	const bool bHeld = Value.Get<bool>();
	bFlyDownHeld = bHeld;
	if (PawnChar)
	{
		PawnChar->SetFlyVerticalInput(bFlyUpHeld, bFlyDownHeld);
	}
}

void ABeanPlayerController::OnToggleCursor(const FInputActionValue& Value)
{
	if (bShowMouseCursor)
	{
		bShowMouseCursor = false;
	}
	else
	{
		bShowMouseCursor = true;
	}
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
	// F5 and Ctrl+S both end up here, so both get the thumbnail refresh and the gallery refresh.
	SaveWork();
}

void ABeanPlayerController::OnEscape(const FInputActionValue& Value)
{
	// Esc backs out of whatever is on top, without ever touching the work itself.
	if (bExportPreviewOpen)
	{
		CloseExportPreview();
		return;
	}
	if (bImportPanelOpen)
	{
		CancelImage();
		return;
	}
	if (bGalleryOpen)
	{
		OpenGallery();
		return;
	}
	if (bScreenshotModeOpen)
	{
		OpenScreenshotMode();
		return;
	}
	if (Grid && Grid->HasSelection())
	{
		ClearSelection();
		return;
	}
	if (bSelectionMode)
	{
		ToggleSelectionMode();
	}
}

// ---------------------------------------------------------------------------
// Modal dialogs (Slate, scaled to the window's DPI)
// ---------------------------------------------------------------------------

int32 ABeanPlayerController::ShowOptionDialog(const FString& Title, const TArray<FString>& Labels, const TArray<int32>& Values, const TArray<bool>& bEnabled)
{
	if (Labels.Num() == 0)
	{
		return 0;
	}

	// Scale the whole dialog to the window height so the same layout reads identically in a small
	// PIE window and in a full-height packaged window.
	float UIScale = 1.0f;
	if (GEngine && GEngine->GameViewport)
	{
		FVector2D ViewportSize(0.0f, 0.0f);
		GEngine->GameViewport->GetViewportSize(ViewportSize);
		const float DpiScale = FSlateApplication::Get().GetApplicationScale();
		if (ViewportSize.Y > 0.0f && DpiScale > 0.0f)
		{
			UIScale = FMath::Clamp(ViewportSize.Y / DpiScale / 720.0f, 1.0f, 2.5f);
		}
	}

	int32 Chosen = 0;
	TSharedPtr<SWindow> DialogWindow;

	// Two columns so even a 4-option list stays a compact grid.
	const int32 Columns = 2;
	const int32 Rows = FMath::DivideAndRoundUp(Labels.Num(), Columns);

	TSharedRef<SVerticalBox> ButtonGrid = SNew(SVerticalBox);
	TArray<TSharedPtr<SButton>> Buttons;

	for (int32 Row = 0; Row < Rows; ++Row)
	{
		TSharedRef<SHorizontalBox> RowBox = SNew(SHorizontalBox);
		for (int32 Col = 0; Col < Columns; ++Col)
		{
			const int32 Index = Row * Columns + Col;
			if (!Labels.IsValidIndex(Index))
			{
				RowBox->AddSlot().FillWidth(1.0f);
				continue;
			}

			const int32 Value = Values.IsValidIndex(Index) ? Values[Index] : (Index + 1);
			const bool bIsEnabled = !bEnabled.IsValidIndex(Index) || bEnabled[Index];

			TSharedPtr<SButton> Button;
			RowBox->AddSlot()
				.FillWidth(1.0f)
				.Padding(6.0f)
				[
					SAssignNew(Button, SButton)
					.IsEnabled(bIsEnabled)
					.OnClicked_Lambda([&Chosen, Value, &DialogWindow]()
					{
						Chosen = Value;
						if (DialogWindow.IsValid())
						{
							DialogWindow->RequestDestroyWindow();
						}
						return FReply::Handled();
					})
					[
						SNew(STextBlock)
						.Text(FText::FromString(Labels[Index]))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", FMath::RoundToInt(16.0f * UIScale)))
						.Justification(ETextJustify::Center)
						.AutoWrapText(true)
					]
				];
			Buttons.Add(Button);
		}
		ButtonGrid->AddSlot().AutoHeight()[RowBox];
	}

	ButtonGrid->AddSlot()
		.AutoHeight()
		.Padding(6.0f, 12.0f, 6.0f, 0.0f)
		[
			SNew(SButton)
			.HAlign(HAlign_Center)
			.OnClicked_Lambda([&Chosen, &DialogWindow]()
			{
				Chosen = 0;
				if (DialogWindow.IsValid())
				{
					DialogWindow->RequestDestroyWindow();
				}
				return FReply::Handled();
			})
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("取消")))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", FMath::RoundToInt(15.0f * UIScale)))
			]
		];

	DialogWindow = SNew(SWindow)
		.Title(FText::FromString(Title))
		// Autosized, so the window measures its own content. This used to be
		// .ClientSize(FVector2D(430 * UIScale, 1)): a client area ONE PIXEL tall, which drew every
		// option dialog as a bare title bar with the buttons clipped out of existence - the
		// resolution picker and the delete confirmation were unusable because of it.
		.SizingRule(ESizingRule::Autosized)
		.SupportsMinimize(false)
		.SupportsMaximize(false)
		[
			SNew(SBox)
			.WidthOverride(430.0f * UIScale)
			[
				SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush("ToolPanel.GroupBorder"))
				.Padding(16.0f * UIScale)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.HAlign(HAlign_Center)
					.Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Title))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", FMath::RoundToInt(18.0f * UIScale)))
						.Justification(ETextJustify::Center)
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						ButtonGrid
					]
				]
			]
		];

	// A modal window is a real Slate window with real buttons, so it needs a pointer: while the
	// player is looking around the cursor is hidden and the game owns the mouse, which leaves the
	// dialog visible but unclickable. Borrow the cursor for the lifetime of the dialog and hand it
	// back afterwards (m03538: "无法正常选择分辨率").
	const bool bCursorWasShown = bShowMouseCursor;
	bShowMouseCursor = true;
	ApplyCursorMode();

	FSlateApplication::Get().AddModalWindow(DialogWindow.ToSharedRef(), GetWorld() && GEngine && GEngine->GameViewport ? GEngine->GameViewport->GetWindow() : nullptr);

	bShowMouseCursor = bCursorWasShown;
	ApplyCursorMode();
	return Chosen;
}

bool ABeanPlayerController::ShowTextPrompt(const FString& Title, const FString& DefaultText, FString& OutText)
{
	bool bAccepted = false;
	TSharedPtr<SWindow> DialogWindow;
	TSharedPtr<SEditableTextBox> TextBox;

	float UIScale = 1.0f;
	if (GEngine && GEngine->GameViewport)
	{
		FVector2D ViewportSize(0.0f, 0.0f);
		GEngine->GameViewport->GetViewportSize(ViewportSize);
		const float DpiScale = FSlateApplication::Get().GetApplicationScale();
		if (ViewportSize.Y > 0.0f && DpiScale > 0.0f)
		{
			UIScale = FMath::Clamp(ViewportSize.Y / DpiScale / 720.0f, 1.0f, 2.5f);
		}
	}

	DialogWindow = SNew(SWindow)
		.Title(FText::FromString(Title))
		// Autosized for the same reason as ShowOptionDialog: a one-pixel client height drew this
		// prompt as a title bar with no text box and no buttons.
		.SizingRule(ESizingRule::Autosized)
		.SupportsMinimize(false)
		.SupportsMaximize(false)
		[
			SNew(SBox)
			.WidthOverride(460.0f * UIScale)
			[
				SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush("ToolPanel.GroupBorder"))
				.Padding(16.0f * UIScale)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 0.0f, 0.0f, 8.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Title))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", FMath::RoundToInt(16.0f * UIScale)))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 0.0f, 0.0f, 12.0f)
					[
						SAssignNew(TextBox, SEditableTextBox)
						.Text(FText::FromString(DefaultText))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", FMath::RoundToInt(15.0f * UIScale)))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.HAlign(HAlign_Right)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(6.0f)
						[
							SNew(SButton)
							.OnClicked_Lambda([&bAccepted, &DialogWindow]()
							{
								bAccepted = true;
								if (DialogWindow.IsValid())
								{
									DialogWindow->RequestDestroyWindow();
								}
								return FReply::Handled();
							})
							[
								SNew(STextBlock).Text(FText::FromString(TEXT("确定")))
							]
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(6.0f)
						[
							SNew(SButton)
							.OnClicked_Lambda([&bAccepted, &DialogWindow]()
							{
								bAccepted = false;
								if (DialogWindow.IsValid())
								{
									DialogWindow->RequestDestroyWindow();
								}
								return FReply::Handled();
							})
							[
								SNew(STextBlock).Text(FText::FromString(TEXT("取消")))
							]
						]
					]
				]
			]
		];

	// Same cursor borrow as ShowOptionDialog: an unclickable rename prompt is worse than no prompt.
	const bool bCursorWasShown = bShowMouseCursor;
	bShowMouseCursor = true;
	ApplyCursorMode();

	FSlateApplication::Get().AddModalWindow(DialogWindow.ToSharedRef(), GetWorld() && GEngine && GEngine->GameViewport ? GEngine->GameViewport->GetWindow() : nullptr);

	bShowMouseCursor = bCursorWasShown;
	ApplyCursorMode();

	OutText = TextBox.IsValid() ? TextBox->GetText().ToString() : DefaultText;
	return bAccepted;
}
