#include "BeanHUDWidget.h"

#include "BeanAssets.h"
#include "BeanPalette.h"
#include "BeanPlayerController.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/GameUserSettings.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "Styling/CoreStyle.h"
#include "UObject/ConstructorHelpers.h"

// ===========================================================================
// UBeanPaletteItemWidget
// ===========================================================================

void UBeanPaletteItemWidget::Setup(int32 InColorIndex, const FLinearColor& InColor, const FString& InName, UTexture2D* InIcon, float InItemWidth, float InItemHeight)
{
	ColorIndex = InColorIndex;
	Color = InColor;
	ColorName = InName;
	IconTexture = InIcon;
	ItemWidth = InItemWidth;
	ItemHeight = InItemHeight;
}

TSharedRef<SWidget> UBeanPaletteItemWidget::RebuildWidget()
{
	if (!Button)
	{
		BuildContent();
	}
	return Super::RebuildWidget();
}

void UBeanPaletteItemWidget::BuildContent()
{
	USizeBox* SizeBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("ItemSizeBox"));
	SizeBox->SetWidthOverride(ItemWidth);
	SizeBox->SetHeightOverride(ItemHeight);

	Frame = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("ItemFrame"));
	Frame->SetPadding(FMargin(2.0f));
	Frame->SetBrushColor(FLinearColor(1.0f, 1.0f, 1.0f, 0.0f));

	Button = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("ItemButton"));
	Button->WidgetStyle.Normal.TintColor = FSlateColor(FLinearColor(1.0f, 1.0f, 1.0f, 0.0f));
	Button->WidgetStyle.Hovered.TintColor = FSlateColor(FLinearColor(1.0f, 1.0f, 1.0f, 0.0f));
	Button->WidgetStyle.Pressed.TintColor = FSlateColor(FLinearColor(1.0f, 1.0f, 1.0f, 0.0f));
	Button->OnClicked.AddDynamic(this, &UBeanPaletteItemWidget::HandleButtonClicked);

	// VerticalBox: the cube icon row on top, its label below. The icon row is
	// pinned with VAlign_Top + a fixed SizeBox so it is NOT Fill-stretched taller
	// than it is wide (that previously squashed the cube into a tall column).
	UVerticalBox* Content = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("ItemContent"));
	Button->AddChild(Content);

	// Cube drawn twice as wide as tall: square texture stretched to a 2:1 box, so
	// the bottom edge is twice as long while the height stays the same.
	const float IconW = ItemWidth;
	const float IconH = FMath::Max(16.0f, ItemWidth * 0.5f);
	USizeBox* IconBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("ItemIconBox"));
	IconBox->SetWidthOverride(IconW);
	IconBox->SetHeightOverride(IconH);

	Icon = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("ItemIcon"));
	if (IconTexture)
	{
		Icon->SetBrushFromTexture(IconTexture, false);
		Icon->SetBrushSize(FVector2D(IconW, IconH));
	}
	IconBox->AddChild(Icon);
	UVerticalBoxSlot* IconSlot = Content->AddChildToVerticalBox(IconBox);
	IconSlot->SetHorizontalAlignment(HAlign_Center);
	IconSlot->SetVerticalAlignment(VAlign_Top);
	IconSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 2.0f));

	Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ItemLabel"));
	Label->SetText(FText::FromString(ColorName));
	Label->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 13));
	Label->SetColorAndOpacity(FSlateColor(Color));
	Label->SetShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.85f));
	Label->SetShadowOffset(FVector2D(0.0f, 1.0f));
	UVerticalBoxSlot* LabelSlot = Content->AddChildToVerticalBox(Label);
	LabelSlot->SetHorizontalAlignment(HAlign_Center);
	LabelSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 2.0f));

	Frame->AddChild(Button);
	SizeBox->AddChild(Frame);
	WidgetTree->RootWidget = SizeBox;
}

void UBeanPaletteItemWidget::HandleButtonClicked()
{
	if (OnClicked)
	{
		OnClicked(ColorIndex);
	}
}

void UBeanPaletteItemWidget::SetSelected(bool bSelected)
{
	if (Frame)
	{
		Frame->SetBrushColor(bSelected ? FLinearColor(1.0f, 1.0f, 1.0f, 0.95f) : FLinearColor(1.0f, 1.0f, 1.0f, 0.0f));
	}
}

// ===========================================================================
// UBeanHUDWidget
// ===========================================================================

UCanvasPanel* UBeanHUDWidget::GetRootCanvas()
{
	if (!RootCanvas)
	{
		RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("HudRoot"));
		WidgetTree->RootWidget = RootCanvas;
	}
	return RootCanvas;
}

TSharedRef<SWidget> UBeanHUDWidget::RebuildWidget()
{
	if (!RootCanvas)
	{
		BuildUI();
	}
	return Super::RebuildWidget();
}

void UBeanHUDWidget::BuildUI()
{
	UCanvasPanel* Canvas = GetRootCanvas();
	if (!Canvas) return;

	// ---- crosshair (screen center) ----
	Crosshair = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("Crosshair"));
	if (UTexture2D* CrosshairTex = FBeanAssets::CreateCrosshairTexture())
	{
		Crosshair->SetBrushFromTexture(CrosshairTex, true);
	}
	Crosshair->SetDesiredSizeOverride(FVector2D(32.0f, 32.0f));
	UCanvasPanelSlot* CrosshairSlot = Canvas->AddChildToCanvas(Crosshair);
	CrosshairSlot->SetAnchors(FAnchors(0.5f, 0.5f));
	CrosshairSlot->SetAlignment(FVector2D(0.5f, 0.5f));
	CrosshairSlot->SetAutoSize(true);

	// ---- top-left hint texts ----
	UVerticalBox* Hints = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("HintBox"));
	const TCHAR* HintLines[] = {
		TEXT("左键放置，右键移除。"),
		TEXT("e键显示/隐藏ui界面。"),
		TEXT("shift+f1，显示/隐藏鼠标。"),
		TEXT("f键切换行走/飞行，space上升，左ctrl下降。"),
		TEXT("F5 保存。"),
	};
	for (const TCHAR* Line : HintLines)
	{
		UTextBlock* Hint = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Hint->SetText(FText::FromString(Line));
		Hint->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 15));
		Hint->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 1.0f, 1.0f, 0.92f)));
		Hint->SetShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.9f));
		Hint->SetShadowOffset(FVector2D(1.0f, 1.0f));
		Hints->AddChildToVerticalBox(Hint);
	}
	UCanvasPanelSlot* HintsSlot = Canvas->AddChildToCanvas(Hints);
	HintsSlot->SetAnchors(FAnchors(0.0f, 0.0f));
	HintsSlot->SetAlignment(FVector2D(0.0f, 0.0f));
	HintsSlot->SetPosition(FVector2D(14.0f, 14.0f));
	HintsSlot->SetAutoSize(true);

	// ---- version marker (top-right, lets you confirm which build is running) ----
	UTextBlock* Version = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("VersionMarker"));
	Version->SetText(FText::FromString(TEXT("Bean v4")));
	Version->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 12));
	Version->SetColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.85f, 0.85f, 0.9f)));
	Version->SetShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.9f));
	Version->SetShadowOffset(FVector2D(1.0f, 1.0f));
	UCanvasPanelSlot* VersionSlot = Canvas->AddChildToCanvas(Version);
	VersionSlot->SetAnchors(FAnchors(1.0f, 0.0f));
	VersionSlot->SetAlignment(FVector2D(1.0f, 0.0f));
	// Push below the exit buttons in the top-right corner.
	VersionSlot->SetPosition(FVector2D(-14.0f, 54.0f));
	VersionSlot->SetAutoSize(true);

	// ---- top-right exit / fullscreen buttons ----
	UHorizontalBox* TopRightBar = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("TopRightBar"));
	auto MakeTopRightButton = [this](const FString& LabelText, float Width)
	{
		UButton* Button = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
		Button->WidgetStyle.Normal.TintColor = FSlateColor(FLinearColor(0.14f, 0.14f, 0.17f, 0.9f));
		Button->WidgetStyle.Hovered.TintColor = FSlateColor(FLinearColor(0.22f, 0.22f, 0.28f, 0.95f));
		Button->WidgetStyle.Pressed.TintColor = FSlateColor(FLinearColor(0.10f, 0.10f, 0.12f, 0.95f));

		UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Text->SetText(FText::FromString(LabelText));
		Text->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 13));
		Text->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 1.0f, 1.0f, 0.95f)));
		Text->SetShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.8f));
		Text->SetShadowOffset(FVector2D(0.0f, 1.0f));

		USizeBox* SizeBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		SizeBox->SetWidthOverride(Width);
		SizeBox->SetHeightOverride(30.0f);
		SizeBox->AddChild(Text);
		Button->AddChild(SizeBox);
		return Button;
	};

	UButton* FullscreenButton = MakeTopRightButton(TEXT("退出全屏"), 88.0f);
	FullscreenButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleToggleFullscreenClicked);
	UHorizontalBoxSlot* FullscreenSlot = TopRightBar->AddChildToHorizontalBox(FullscreenButton);
	FullscreenSlot->SetPadding(FMargin(0.0f, 0.0f, 8.0f, 0.0f));

	UButton* ExitButton = MakeTopRightButton(TEXT("退出"), 66.0f);
	ExitButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleExitClicked);
	UHorizontalBoxSlot* ExitSlot = TopRightBar->AddChildToHorizontalBox(ExitButton);
	ExitSlot->SetPadding(FMargin(0.0f, 0.0f));

	UCanvasPanelSlot* TopRightBarSlot = Canvas->AddChildToCanvas(TopRightBar);
	TopRightBarSlot->SetAnchors(FAnchors(1.0f, 0.0f));
	TopRightBarSlot->SetAlignment(FVector2D(1.0f, 0.0f));
	TopRightBarSlot->SetPosition(FVector2D(-14.0f, 14.0f));
	TopRightBarSlot->SetAutoSize(true);

	// ---- right-side button stack ----
	ButtonStack = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("ButtonStack"));

	auto MakeButton = [this](const FString& LabelText)
	{
		UButton* Button = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
		Button->WidgetStyle.Normal.TintColor = FSlateColor(FLinearColor(0.14f, 0.14f, 0.17f, 0.9f));
		Button->WidgetStyle.Hovered.TintColor = FSlateColor(FLinearColor(0.22f, 0.22f, 0.28f, 0.95f));
		Button->WidgetStyle.Pressed.TintColor = FSlateColor(FLinearColor(0.10f, 0.10f, 0.12f, 0.95f));

		UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Text->SetText(FText::FromString(LabelText));
		Text->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 14));
		Text->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 1.0f, 1.0f, 0.95f)));
		Text->SetShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.8f));
		Text->SetShadowOffset(FVector2D(0.0f, 1.0f));

		USizeBox* SizeBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		SizeBox->SetWidthOverride(122.0f);
		SizeBox->SetHeightOverride(34.0f);
		SizeBox->AddChild(Text);
		Button->AddChild(SizeBox);
		return Button;
	};

	UVerticalBoxSlot* StackSlot = nullptr;

	UButton* ImportButton = MakeButton(TEXT("导入图片"));
	ImportButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleImportClicked);
	StackSlot = ButtonStack->AddChildToVerticalBox(ImportButton);
	StackSlot->SetPadding(FMargin(0.0f, 3.0f));

	ApplyButton = MakeButton(TEXT("应用"));
	ApplyButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleApplyClicked);
	StackSlot = ButtonStack->AddChildToVerticalBox(ApplyButton);
	StackSlot->SetPadding(FMargin(0.0f, 3.0f));

	CancelButton = MakeButton(TEXT("取消"));
	CancelButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleCancelClicked);
	StackSlot = ButtonStack->AddChildToVerticalBox(CancelButton);
	StackSlot->SetPadding(FMargin(0.0f, 3.0f));

	UButton* OpacityIncButton = MakeButton(TEXT("透明度+"));
	OpacityIncButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleOpacityIncClicked);
	StackSlot = ButtonStack->AddChildToVerticalBox(OpacityIncButton);
	StackSlot->SetPadding(FMargin(0.0f, 3.0f));

	UButton* OpacityDecButton = MakeButton(TEXT("透明度−"));
	OpacityDecButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleOpacityDecClicked);
	StackSlot = ButtonStack->AddChildToVerticalBox(OpacityDecButton);
	StackSlot->SetPadding(FMargin(0.0f, 3.0f));

	ToggleOverlayButton = MakeButton(TEXT("显示/隐藏提示层"));
	ToggleOverlayButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleToggleOverlayClicked);
	StackSlot = ButtonStack->AddChildToVerticalBox(ToggleOverlayButton);
	StackSlot->SetPadding(FMargin(0.0f, 3.0f));

	UCanvasPanelSlot* StackCanvasSlot = Canvas->AddChildToCanvas(ButtonStack);
	StackCanvasSlot->SetAnchors(FAnchors(1.0f, 0.5f));
	StackCanvasSlot->SetAlignment(FVector2D(1.0f, 0.5f));
	StackCanvasSlot->SetPosition(FVector2D(-14.0f, 0.0f));
	StackCanvasSlot->SetAutoSize(true);

	// ---- bottom palette bar (two rows of cubes) ----
	PaletteBar = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("PaletteBar"));
	UCanvasPanelSlot* BarSlot = Canvas->AddChildToCanvas(PaletteBar);
	BarSlot->SetAnchors(FAnchors(0.5f, 1.0f));
	BarSlot->SetAlignment(FVector2D(0.5f, 1.0f));
	BarSlot->SetPosition(FVector2D(0.0f, -36.0f));
	BarSlot->SetAutoSize(true);

	SetImportButtonsEnabled(false);

	// Build the color palette bar immediately (independent of the grid actor).
	BuildPaletteBar();
}

void UBeanHUDWidget::BuildPaletteBar()
{
	if (bBuiltPaletteBar || !PaletteBar) return;
	bBuiltPaletteBar = true;

	const TArray<FBeanPaletteColor> Palette = FBeanPalette::LoadOrCreateDefault();

	// Two rows of 24 cubes each, filling the screen width.
	const int32 CubeCount = Palette.Num();
	const int32 RowCount = 2;
	const int32 PerRow = FMath::Max(1, (CubeCount + RowCount - 1) / RowCount);

	// Size items so a row spans the viewport width (big cubes).
	// Robustly determine the viewport width so item sizing never collapses (which
	// squashed the cube icons into nothing and made the labels overlap). If the
	// viewport reports no usable size yet, fall back to a sensible default.
	int32 ViewportWidth = 1920;
	FVector2D ViewportSize(0.0f, 0.0f);
	if (UWorld* World = GetWorld())
	{
		if (const UGameViewportClient* Viewport = World->GetGameViewport())
		{
			Viewport->GetViewportSize(ViewportSize);
		}
	}
	if (ViewportSize.X >= 100.0f)
	{
		ViewportWidth = FMath::Max(640, static_cast<int32>(ViewportSize.X));
	}
	// Stretch a row across the full viewport width; one item per column. Enforce a
	// sane minimum so a misreported size can never shrink the cubes/labels away.
	const float ItemWidth = FMath::Max(48.0f, static_cast<float>(ViewportWidth) / static_cast<float>(FMath::Max(1, PerRow)));
	const float ItemHeight = ItemWidth * 0.5f + 28.0f; // 2:1 cube + room for its label below

	PaletteItems.Reset();
	int32 Index = 0;
	for (int32 Row = 0; Row < RowCount; ++Row)
	{
		UHorizontalBox* RowBox = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), *FString::Printf(TEXT("PaletteRow_%d"), Row));
		for (int32 Col = 0; Col < PerRow && Index < CubeCount; ++Col, ++Index)
		{
			UBeanPaletteItemWidget* Item = CreateWidget<UBeanPaletteItemWidget>(GetWorld(), UBeanPaletteItemWidget::StaticClass());
			if (!Item) continue;
			UTexture2D* Icon = FBeanAssets::CreateCubeIconTexture(Palette[Index].Color);
			Item->Setup(Index, Palette[Index].Color, Palette[Index].Name, Icon, ItemWidth, ItemHeight);
			Item->OnClicked = [this](int32 ColorIndex)
			{
				if (OnColorSelected)
				{
					OnColorSelected(ColorIndex);
				}
			};
			UHorizontalBoxSlot* ItemSlot = RowBox->AddChildToHorizontalBox(Item);
			ItemSlot->SetPadding(FMargin(0.0f, 0.0f));
			PaletteItems.Add(Item);
		}
		UVerticalBoxSlot* RowSlot = PaletteBar->AddChildToVerticalBox(RowBox);
		RowSlot->SetHorizontalAlignment(HAlign_Center);
	}
	SetSelectedColor(1);
}

void UBeanHUDWidget::SetSelectedColor(int32 ColorIndex)
{
	for (int32 i = 0; i < PaletteItems.Num(); ++i)
	{
		if (PaletteItems[i])
		{
			PaletteItems[i]->SetSelected(i == ColorIndex);
		}
	}
}

void UBeanHUDWidget::SetImportButtonsEnabled(bool bHasOverlay)
{
	if (ApplyButton) ApplyButton->SetIsEnabled(bHasOverlay);
	if (CancelButton) CancelButton->SetIsEnabled(bHasOverlay);
	if (ToggleOverlayButton) ToggleOverlayButton->SetIsEnabled(bHasOverlay);
}

void UBeanHUDWidget::ToggleVisibility()
{
	UCanvasPanel* Canvas = GetRootCanvas();
	if (!Canvas) return;
	Canvas->SetVisibility(Canvas->GetVisibility() == ESlateVisibility::Visible ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
}

// ---------------------------------------------------------------------------
// Button handlers
// ---------------------------------------------------------------------------

void UBeanHUDWidget::HandleImportClicked()
{
	if (Controller)
	{
		Controller->ImportImage();
	}
}

void UBeanHUDWidget::HandleApplyClicked()
{
	if (Controller)
	{
		Controller->ApplyImage();
	}
}

void UBeanHUDWidget::HandleCancelClicked()
{
	if (Controller)
	{
		Controller->CancelImage();
	}
}

void UBeanHUDWidget::HandleOpacityIncClicked()
{
	if (Controller)
	{
		Controller->ChangeOverlayOpacity(-0.1f);
	}
}

void UBeanHUDWidget::HandleOpacityDecClicked()
{
	if (Controller)
	{
		Controller->ChangeOverlayOpacity(0.1f);
	}
}

void UBeanHUDWidget::HandleToggleOverlayClicked()
{
	if (Controller)
	{
		Controller->ToggleOverlayVisible();
	}
}

void UBeanHUDWidget::HandleExitClicked()
{
	APlayerController* PC = Controller ? Cast<APlayerController>(Controller) : UGameplayStatics::GetPlayerController(GetWorld(), 0);
	UKismetSystemLibrary::QuitGame(this, PC, EQuitPreference::Quit, false);
}

void UBeanHUDWidget::HandleToggleFullscreenClicked()
{
	UGameUserSettings* Settings = UGameUserSettings::GetGameUserSettings();
	if (!Settings) return;
	const bool bIsFullscreen = Settings->GetFullscreenMode() != EWindowMode::Windowed;
	Settings->SetFullscreenMode(bIsFullscreen ? EWindowMode::Windowed : EWindowMode::Fullscreen);
	Settings->ApplySettings(false);
}
