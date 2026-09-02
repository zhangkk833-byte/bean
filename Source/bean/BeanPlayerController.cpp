#include "BeanPlayerController.h"

#include "BeanCharacter.h"
#include "BeanGrid.h"
#include "BeanHUDWidget.h"
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
#include "Kismet/GameplayStatics.h"
#include "Framework/Application/SlateUser.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Widgets/SOverlay.h"
#include "Styling/CoreStyle.h"

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
	MappingContext->MapKey(IA_ToggleCursor, EKeys::F1);

	IA_ToggleUI = MakeAction(TEXT("IA_ToggleUI"), EInputActionValueType::Boolean);
	MappingContext->MapKey(IA_ToggleUI, EKeys::E);

	IA_Save = MakeAction(TEXT("IA_Save"), EInputActionValueType::Boolean);
	MappingContext->MapKey(IA_Save, EKeys::F5);

	EIC->BindAction(IA_MoveForward, ETriggerEvent::Triggered, this, &ABeanPlayerController::OnMoveForward);
	EIC->BindAction(IA_MoveRight, ETriggerEvent::Triggered, this, &ABeanPlayerController::OnMoveRight);
	EIC->BindAction(IA_Look, ETriggerEvent::Triggered, this, &ABeanPlayerController::OnLook);
	EIC->BindAction(IA_Place, ETriggerEvent::Started, this, &ABeanPlayerController::OnPlace);
	EIC->BindAction(IA_Remove, ETriggerEvent::Started, this, &ABeanPlayerController::OnRemove);
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
	// In cursor mode (Shift+F1), clicking empty space exits cursor mode instead
	// of placing; clicks on UI (buttons / palette cubes) are handled by Slate.
	if (bShowMouseCursor)
	{
		if (!IsClickOverHUD())
		{
			bShowMouseCursor = false;
			ApplyCursorMode();
		}
		return;
	}
	if (!bTargetValid || !Grid) return;
	Grid->PlaceCell(TargetX, TargetY);
}

void ABeanPlayerController::OnRemove(const FInputActionValue& Value)
{
	// Same cursor-mode rule as OnPlace: empty-space clicks hide the cursor, never remove.
	if (bShowMouseCursor)
	{
		if (!IsClickOverHUD())
		{
			bShowMouseCursor = false;
			ApplyCursorMode();
		}
		return;
	}
	if (!bTargetValid || !Grid) return;
	Grid->RemoveCell(TargetX, TargetY);
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
	// Only toggle when Shift is held (Shift+F1), per spec.
	if (!FSlateApplication::Get().GetModifierKeys().IsShiftDown()) return;
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
	if (!Grid) return;

#if PLATFORM_WINDOWS
	// DesktopPlatform is a non-redistributable engine module and cannot be referenced
	// from packaged game targets, so use the Windows native file dialog instead.
	WCHAR FileBuffer[MAX_PATH] = {};
	OPENFILENAMEW Ofn;
	FMemory::Memzero(Ofn);
	Ofn.lStructSize = sizeof(OPENFILENAMEW);
	Ofn.hwndOwner = nullptr;
	Ofn.lpstrFilter = TEXT("图片文件 (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0所有文件 (*.*)\0*.*\0\0");
	Ofn.lpstrFile = FileBuffer;
	Ofn.nMaxFile = MAX_PATH;
	Ofn.lpstrTitle = TEXT("选择要导入的图片");
	Ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
	if (!GetOpenFileNameW(&Ofn))
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

	FImage Resized;
	SrcBGRA.ResizeTo(Resized, Granularity, Granularity, ERawImageFormat::BGRA8, EGammaSpace::sRGB);

	const int32 Count = Granularity * Granularity;
	const FColor* Data = reinterpret_cast<const FColor*>(Resized.RawData.GetData());

	TArray<uint8> Quantized;
	Quantized.SetNum(Count);
	for (int32 i = 0; i < Count; ++i)
	{
		const FColor Pixel = Data[i];
		if (Pixel.A < 128)
		{
			Quantized[i] = ABeanGrid::EmptyCell; // transparent: skip
		}
		else
		{
			const FLinearColor Linear = FLinearColor::FromSRGBColor(Pixel);
			Quantized[i] = static_cast<uint8>(Grid->NearestColorIndex(Linear));
		}
	}

	Grid->BeginImagePreview(Quantized, Granularity);
	UE_LOG(LogTemp, Log, TEXT("[Bean] Imported image '%s' at %dx%d, %d non-empty cells."), *FilePath, Granularity, Granularity, Count);
	if (HUDWidget)
	{
		HUDWidget->SetImportButtonsEnabled(true);
	}
#else
	// Image import currently uses a Windows-only native file dialog.
	UE_LOG(LogTemp, Warning, TEXT("[Bean] ImportImage is only supported on Windows."));
#endif
}

int32 ABeanPlayerController::PromptGranularity()
{
	const int32 Options[] = { 32, 64, 128, 256 };
	int32 Result = 0; // 0 = cancelled

	// Store the modal result in the game viewport client's parent window reference.
	const TSharedPtr<SWindow> ParentWindow = GetWorld() && GetWorld()->GetGameViewport()
		? GetWorld()->GetGameViewport()->GetWindow()
		: nullptr;

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
			.Padding(6.0f)
			[
				SNew(SButton)
				.ButtonStyle(&FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("Button"))
				.OnClicked_Lambda([&Result, &Window, Opt]()
				{
					Result = Opt;
					Window->RequestDestroyWindow();
					return FReply::Handled();
				})
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("%d × %d"), Opt, Opt)))
					.Justification(ETextJustify::Center)
				]
			];
	}
	MainBox->AddSlot().AutoHeight().Padding(12.0f, 12.0f, 12.0f, 0.0f)[SizeGrid];

	MainBox->AddSlot().AutoHeight().Padding(12.0f, 8.0f, 12.0f, 12.0f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[
			SNew(SButton)
			.ButtonStyle(&FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("Button"))
			.OnClicked_Lambda([&Result, &Window]()
			{
				Result = 0;
				Window->RequestDestroyWindow();
				return FReply::Handled();
			})
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("取消")))
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
