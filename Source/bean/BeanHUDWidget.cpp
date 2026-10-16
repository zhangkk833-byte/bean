#include "BeanHUDWidget.h"

#include "BeanAssets.h"
#include "BeanGrid.h"
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
#include "Components/ProgressBar.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/GameUserSettings.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "Styling/CoreStyle.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	/** Text colour used by every panel's title. */
	const FLinearColor BeanTitleColor(1.0f, 1.0f, 1.0f, 0.96f);
	/** Body text colour. */
	const FLinearColor BeanBodyColor(0.92f, 0.92f, 0.96f, 0.95f);
	/** Warning / notice colour. */
	const FLinearColor BeanNoticeColor(1.0f, 0.82f, 0.42f, 0.98f);
	/** Muted footer colour. */
	const FLinearColor BeanMutedColor(0.78f, 0.78f, 0.84f, 0.9f);
}

// ===========================================================================
// UBeanPaletteItemWidget
// ===========================================================================

void UBeanPaletteItemWidget::Setup(int32 InColorIndex, const FLinearColor& InColor, const FString& InName, UTexture2D* InIcon, const FVector2D& InFaceCenter, float InItemWidth, float InItemHeight)
{
	ColorIndex = InColorIndex;
	Color = InColor;
	ColorName = InName;
	IconTexture = InIcon;
	FaceCenter = InFaceCenter;
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

	// CanvasPanel sized by the SizeBox above: the cube image fills it, and the 1..48
	// palette number is anchored to the CENTRE of the cube's front face.
	UCanvasPanel* IconCanvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("ItemIconCanvas"));
	UCanvasPanelSlot* IconCanvasSlot = IconCanvas->AddChildToCanvas(Icon);
	IconCanvasSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
	IconCanvasSlot->SetOffsets(FMargin(0.0f));

	NumberLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ItemNumber"));
	NumberLabel->SetText(FText::FromString(FString::FromInt(ColorIndex + 1)));
	NumberLabel->SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 9));
	NumberLabel->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 1.0f, 1.0f, 0.85f)));
	NumberLabel->SetShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.7f));
	NumberLabel->SetShadowOffset(FVector2D(1.0f, 1.0f));
	UCanvasPanelSlot* NumberSlot = IconCanvas->AddChildToCanvas(NumberLabel);
	NumberSlot->SetAnchors(FAnchors(FaceCenter.X, FaceCenter.Y));
	NumberSlot->SetAlignment(FVector2D(0.5f, 0.5f));
	NumberSlot->SetAutoSize(true);

	IconBox->AddChild(IconCanvas);
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
// UBeanGalleryItemWidget
// ===========================================================================

void UBeanGalleryItemWidget::Setup(const FString& InSlotName, const FString& InWorkName, UTexture2D* InThumbnail,
	const FString& InMetaLine, bool bInReadable, const FString& InProblem, float InRowHeight)
{
	SlotName = InSlotName;
	WorkName = InWorkName;
	ThumbnailTexture = InThumbnail;
	MetaLine = InMetaLine;
	bReadable = bInReadable;
	Problem = InProblem;
	RowHeight = InRowHeight;
}

TSharedRef<SWidget> UBeanGalleryItemWidget::RebuildWidget()
{
	if (!OpenButton)
	{
		BuildContent();
	}
	return Super::RebuildWidget();
}

void UBeanGalleryItemWidget::BuildContent()
{
	UBorder* Frame = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("GalleryRowFrame"));
	Frame->SetBrushColor(FLinearColor(0.10f, 0.10f, 0.13f, 0.92f));
	Frame->SetPadding(FMargin(8.0f));

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("GalleryRow"));
	Frame->AddChild(Row);

	// ---- thumbnail (click to open) ----
	USizeBox* ThumbBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("GalleryThumbBox"));
	ThumbBox->SetWidthOverride(RowHeight);
	ThumbBox->SetHeightOverride(RowHeight);

	OpenButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("GalleryThumbButton"));
	FButtonStyle ThumbStyle = OpenButton->GetStyle();
	ThumbStyle.Normal.TintColor = FSlateColor(FLinearColor(0.16f, 0.16f, 0.20f, 0.6f));
	ThumbStyle.Hovered.TintColor = FSlateColor(FLinearColor(0.26f, 0.26f, 0.34f, 0.8f));
	ThumbStyle.Pressed.TintColor = FSlateColor(FLinearColor(0.10f, 0.10f, 0.13f, 0.9f));
	OpenButton->SetStyle(ThumbStyle);
	OpenButton->OnClicked.AddDynamic(this, &UBeanGalleryItemWidget::HandleOpenClicked);

	Thumbnail = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("GalleryThumb"));
	if (ThumbnailTexture)
	{
		Thumbnail->SetBrushFromTexture(ThumbnailTexture, false);
		Thumbnail->SetBrushSize(FVector2D(RowHeight, RowHeight));
	}
	OpenButton->AddChild(Thumbnail);
	ThumbBox->AddChild(OpenButton);
	UHorizontalBoxSlot* ThumbSlot = Row->AddChildToHorizontalBox(ThumbBox);
	ThumbSlot->SetPadding(FMargin(0.0f, 0.0f, 10.0f, 0.0f));
	ThumbSlot->SetVerticalAlignment(VAlign_Center);

	// ---- name + metadata + problem ----
	UVerticalBox* TextColumn = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("GalleryTextColumn"));

	NameLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("GalleryName"));
	NameLabel->SetText(FText::FromString(WorkName));
	NameLabel->SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 16));
	NameLabel->SetColorAndOpacity(FSlateColor(BeanTitleColor));
	NameLabel->SetShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.9f));
	TextColumn->AddChildToVerticalBox(NameLabel);

	MetaLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("GalleryMeta"));
	MetaLabel->SetText(FText::FromString(MetaLine));
	MetaLabel->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 13));
	MetaLabel->SetColorAndOpacity(FSlateColor(BeanMutedColor));
	TextColumn->AddChildToVerticalBox(MetaLabel);

	ProblemLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("GalleryProblem"));
	ProblemLabel->SetText(FText::FromString(Problem));
	ProblemLabel->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 13));
	ProblemLabel->SetColorAndOpacity(FSlateColor(BeanNoticeColor));
	ProblemLabel->SetAutoWrapText(true);
	ProblemLabel->SetVisibility(Problem.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	TextColumn->AddChildToVerticalBox(ProblemLabel);

	UHorizontalBoxSlot* TextSlot = Row->AddChildToHorizontalBox(TextColumn);
	TextSlot->SetPadding(FMargin(0.0f, 0.0f, 10.0f, 0.0f));
	TextSlot->SetVerticalAlignment(VAlign_Center);
	TextSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

	// ---- actions ----
	UHorizontalBox* GalleryButtonRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("GalleryActions"));

	auto MakeAction = [this, GalleryButtonRow](const FString& LabelText)
	{
		UButton* Button = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
		FButtonStyle Style = Button->GetStyle();
		Style.Normal.TintColor = FSlateColor(FLinearColor(0.16f, 0.16f, 0.20f, 0.94f));
		Style.Hovered.TintColor = FSlateColor(FLinearColor(0.26f, 0.26f, 0.34f, 0.98f));
		Style.Pressed.TintColor = FSlateColor(FLinearColor(0.10f, 0.10f, 0.13f, 0.98f));
		Button->SetStyle(Style);

		UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Text->SetText(FText::FromString(LabelText));
		Text->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 13));
		Text->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 1.0f, 1.0f, 0.95f)));

		USizeBox* SizeBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		SizeBox->SetWidthOverride(78.0f);
		SizeBox->SetHeightOverride(30.0f);
		SizeBox->AddChild(Text);
		Button->AddChild(SizeBox);

		UHorizontalBoxSlot* ButtonSlot = GalleryButtonRow->AddChildToHorizontalBox(Button);
		ButtonSlot->SetPadding(FMargin(3.0f, 0.0f));
		return Button;
	};

	MakeAction(TEXT("打开"))->OnClicked.AddDynamic(this, &UBeanGalleryItemWidget::HandleOpenClicked);
	MakeAction(TEXT("重命名"))->OnClicked.AddDynamic(this, &UBeanGalleryItemWidget::HandleRenameClicked);
	MakeAction(TEXT("导出图片"))->OnClicked.AddDynamic(this, &UBeanGalleryItemWidget::HandleExportClicked);
	MakeAction(TEXT("删除"))->OnClicked.AddDynamic(this, &UBeanGalleryItemWidget::HandleDeleteClicked);

	UHorizontalBoxSlot* ActionsSlot = Row->AddChildToHorizontalBox(GalleryButtonRow);
	ActionsSlot->SetVerticalAlignment(VAlign_Center);

	// An unreadable save cannot be opened; keep the row but grey out 打开 so a corrupt file is
	// reported instead of crashing the whole gallery (or silently disappearing).
	OpenButton->SetIsEnabled(bReadable);

	WidgetTree->RootWidget = Frame;
}

void UBeanGalleryItemWidget::SetProblem(const FString& InProblem, bool bInReadable)
{
	Problem = InProblem;
	bReadable = bInReadable;
	if (ProblemLabel)
	{
		ProblemLabel->SetText(FText::FromString(Problem));
		ProblemLabel->SetVisibility(Problem.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}
	if (OpenButton)
	{
		OpenButton->SetIsEnabled(bReadable);
	}
}

void UBeanGalleryItemWidget::HandleOpenClicked()
{
	if (Actions.OnOpen && bReadable)
	{
		Actions.OnOpen(SlotName);
	}
}

void UBeanGalleryItemWidget::HandleRenameClicked()
{
	if (Actions.OnRename)
	{
		Actions.OnRename(SlotName);
	}
}

void UBeanGalleryItemWidget::HandleDeleteClicked()
{
	if (Actions.OnDelete)
	{
		Actions.OnDelete(SlotName);
	}
}

void UBeanGalleryItemWidget::HandleExportClicked()
{
	if (Actions.OnExport && bReadable)
	{
		Actions.OnExport(SlotName);
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

FReply UBeanHUDWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	// Esc always means "back out of what is open, without touching the work": the panels are Slate
	// widgets outside the Enhanced Input path, so the HUD itself offers this escape hatch.
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		if (Controller)
		{
			Controller->ClearSelection();
			return FReply::Handled();
		}
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

UButton* UBeanHUDWidget::MakePanelButton(const FString& LabelText, float Width, float Height, int32 FontSize)
{
	UButton* Button = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
	FButtonStyle Style = Button->GetStyle();
	Style.Normal.TintColor = FSlateColor(FLinearColor(0.16f, 0.16f, 0.20f, 0.94f));
	Style.Hovered.TintColor = FSlateColor(FLinearColor(0.26f, 0.26f, 0.34f, 0.98f));
	Style.Pressed.TintColor = FSlateColor(FLinearColor(0.10f, 0.10f, 0.13f, 0.98f));
	Button->SetStyle(Style);

	UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Text->SetText(FText::FromString(LabelText));
	Text->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", FontSize));
	Text->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 1.0f, 1.0f, 0.95f)));
	Text->SetShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.8f));
	Text->SetShadowOffset(FVector2D(0.0f, 1.0f));

	USizeBox* SizeBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	SizeBox->SetWidthOverride(Width);
	SizeBox->SetHeightOverride(Height);
	SizeBox->AddChild(Text);
	Button->AddChild(SizeBox);
	return Button;
}

void UBeanHUDWidget::SetPanelButtonLabel(UButton* Button, const FString& LabelText)
{
	if (!Button) return;

	// MakePanelButton builds Button -> SizeBox -> TextBlock; walk that chain. UButton is a content
	// widget, so its single child is reached through GetContent() rather than a panel index.
	UWidget* Inner = Cast<UContentWidget>(Button) ? Cast<UContentWidget>(Button)->GetContent() : nullptr;
	while (Inner)
	{
		if (UTextBlock* Text = Cast<UTextBlock>(Inner))
		{
			Text->SetText(FText::FromString(LabelText));
			return;
		}
		Inner = Cast<UContentWidget>(Inner) ? Cast<UContentWidget>(Inner)->GetContent() : nullptr;
	}
}

void UBeanHUDWidget::SetPanelButtonHighlight(UButton* Button, bool bActive)
{
	if (!Button) return;
	FButtonStyle Style = Button->GetStyle();
	Style.Normal.TintColor = FSlateColor(bActive
		? FLinearColor(0.34f, 0.40f, 0.62f, 0.98f)
		: FLinearColor(0.16f, 0.16f, 0.20f, 0.94f));
	Button->SetStyle(Style);
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
		TEXT("长按左键连续放置；长按右键连续移除（单击只删一格）。"),
		TEXT("Ctrl+Z 撤销，Ctrl+Y 重做。"),
		TEXT("「框选」进入批量模式：拖出矩形后批量放置/移除/换色/补齐。"),
		TEXT("E键显示/隐藏UI界面。"),
		TEXT("Q键显示/隐藏鼠标。"),
		TEXT("F键切换行走/飞行，空格上升，左Ctrl下降。"),
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
	Version->SetText(FText::FromString(TEXT("Bean v8")));
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

	// 应用 and 应用并铺满 used to be two buttons; the user asked for one (m04494). The single button
	// now does both - it registers the pattern as the hint layer and places every bead of it in one
	// undo step. Hint-only is still reachable from the import panel's 设为提示层 button.
	ApplyFillButton = MakeButton(TEXT("应用并铺满"));
	ApplyFillButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleApplyFillClicked);
	StackSlot = ButtonStack->AddChildToVerticalBox(ApplyFillButton);
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

	// ---- undo / redo / selection / work group ----
	UndoButton = MakeButton(TEXT("撤销"));
	UndoButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleUndoClicked);
	StackSlot = ButtonStack->AddChildToVerticalBox(UndoButton);
	StackSlot->SetPadding(FMargin(0.0f, 3.0f));

	RedoButton = MakeButton(TEXT("重做"));
	RedoButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleRedoClicked);
	StackSlot = ButtonStack->AddChildToVerticalBox(RedoButton);
	StackSlot->SetPadding(FMargin(0.0f, 3.0f));

	SelectionButton = MakeButton(TEXT("框选"));
	SelectionButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleToggleSelectionClicked);
	StackSlot = ButtonStack->AddChildToVerticalBox(SelectionButton);
	StackSlot->SetPadding(FMargin(0.0f, 3.0f));

	UButton* NewCanvasButton = MakeButton(TEXT("新建画布"));
	NewCanvasButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleNewCanvasClicked);
	StackSlot = ButtonStack->AddChildToVerticalBox(NewCanvasButton);
	StackSlot->SetPadding(FMargin(0.0f, 3.0f));

	// Saving has two buttons on purpose: the quiet one needs no answer from the player and writes to
	// the slot the work already lives in, while "另存作品" is the one that asks for a name (m04820).
	UButton* SaveButton = MakeButton(TEXT("保存作品"));
	SaveButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleSaveClicked);
	StackSlot = ButtonStack->AddChildToVerticalBox(SaveButton);
	StackSlot->SetPadding(FMargin(0.0f, 3.0f));

	UButton* SaveWorkButton = MakeButton(TEXT("另存作品"));
	SaveWorkButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleSaveWorkClicked);
	StackSlot = ButtonStack->AddChildToVerticalBox(SaveWorkButton);
	StackSlot->SetPadding(FMargin(0.0f, 3.0f));

	UButton* GalleryButton = MakeButton(TEXT("作品画廊"));
	GalleryButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleGalleryClicked);
	StackSlot = ButtonStack->AddChildToVerticalBox(GalleryButton);
	StackSlot->SetPadding(FMargin(0.0f, 3.0f));

	// Export sits below the edit group, separated by a wider gap: it is a standalone action, not
	// another step of the import flow, and it stays enabled whatever the grid holds.
	UButton* ExportButton = MakeButton(TEXT("导出图片"));
	ExportButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleExportClicked);
	StackSlot = ButtonStack->AddChildToVerticalBox(ExportButton);
	StackSlot->SetPadding(FMargin(0.0f, 12.0f, 0.0f, 3.0f));

	UButton* ScreenshotButton = MakeButton(TEXT("截图展示"));
	ScreenshotButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleScreenshotClicked);
	StackSlot = ButtonStack->AddChildToVerticalBox(ScreenshotButton);
	StackSlot->SetPadding(FMargin(0.0f, 3.0f));

	UCanvasPanelSlot* StackCanvasSlot = Canvas->AddChildToCanvas(ButtonStack);
	StackCanvasSlot->SetAnchors(FAnchors(1.0f, 0.5f));
	StackCanvasSlot->SetAlignment(FVector2D(1.0f, 0.5f));
	StackCanvasSlot->SetPosition(FVector2D(-14.0f, 26.0f));
	StackCanvasSlot->SetAutoSize(true);

	// ---- status strip (top-centre): short-lived feedback, e.g. the export result ----
	StatusPanel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("StatusPanel"));
	StatusPanel->SetBrushColor(FLinearColor(0.06f, 0.06f, 0.08f, 0.86f));
	StatusPanel->SetPadding(FMargin(16.0f, 8.0f));

	StatusLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("StatusLabel"));
	StatusLabel->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 15));
	StatusLabel->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 1.0f, 1.0f, 0.96f)));
	StatusLabel->SetShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.9f));
	StatusLabel->SetShadowOffset(FVector2D(1.0f, 1.0f));
	// Wrap long messages (and keep multi-line ones centred) instead of running off the screen.
	StatusLabel->SetJustification(ETextJustify::Center);
	StatusLabel->SetAutoWrapText(true);
	StatusPanel->SetContent(StatusLabel);

	USizeBox* StatusSize = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("StatusSize"));
	StatusSize->SetMaxDesiredWidth(720.0f);
	StatusSize->AddChild(StatusPanel);

	UCanvasPanelSlot* StatusSlot = Canvas->AddChildToCanvas(StatusSize);
	StatusSlot->SetAnchors(FAnchors(0.5f, 0.0f));
	StatusSlot->SetAlignment(FVector2D(0.5f, 0.0f));
	StatusSlot->SetPosition(FVector2D(0.0f, 16.0f));
	StatusSlot->SetAutoSize(true);

	// Nothing has happened yet, so start hidden.
	StatusPanel->SetVisibility(ESlateVisibility::Collapsed);

	// ---- bottom palette bar (two rows of cubes) ----
	PaletteBar = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("PaletteBar"));
	UCanvasPanelSlot* BarSlot = Canvas->AddChildToCanvas(PaletteBar);
	BarSlot->SetAnchors(FAnchors(0.5f, 1.0f));
	BarSlot->SetAlignment(FVector2D(0.5f, 1.0f));
	BarSlot->SetPosition(FVector2D(0.0f, -36.0f));
	BarSlot->SetAutoSize(true);

	// ---- progress + selection panels (behind the modal overlays) ----
	BuildProgressPanel();
	BuildSelectionPanel();

	// Last children added, so the modal overlays sit on top of everything else.
	BuildExportPreview();
	BuildImportPreview();
	BuildGallery();
	BuildScreenshotMode();

	SetImportButtonsEnabled(false);
	SetHistoryState(false, false, FString(), FString());

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
			FVector2D FaceCenter;
			UTexture2D* Icon = FBeanAssets::CreateCubeIconTexture(Palette[Index].Color, &FaceCenter);
			Item->Setup(Index, Palette[Index].Color, Palette[Index].Name, Icon, FaceCenter, ItemWidth, ItemHeight);
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
	if (ApplyFillButton) ApplyFillButton->SetIsEnabled(bHasOverlay);
	if (CancelButton) CancelButton->SetIsEnabled(bHasOverlay);
	if (ToggleOverlayButton) ToggleOverlayButton->SetIsEnabled(bHasOverlay);
}

void UBeanHUDWidget::ToggleVisibility()
{
	UCanvasPanel* Canvas = GetRootCanvas();
	if (!Canvas) return;
	SetHUDVisible(Canvas->GetVisibility() != ESlateVisibility::Visible);
}

void UBeanHUDWidget::SetHUDVisible(bool bVisible)
{
	UCanvasPanel* Canvas = GetRootCanvas();
	if (!Canvas) return;
	Canvas->SetVisibility(bVisible ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
}

bool UBeanHUDWidget::IsHUDVisible() const
{
	return RootCanvas && RootCanvas->GetVisibility() == ESlateVisibility::Visible;
}

void UBeanHUDWidget::ShowStatus(const FString& Message, float DurationSeconds)
{
	if (!StatusPanel || !StatusLabel) return;

	StatusLabel->SetText(FText::FromString(Message));
	StatusPanel->SetVisibility(ESlateVisibility::HitTestInvisible);

	// Click-through: the strip must never swallow a place/remove click.
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(StatusTimerHandle);
		if (DurationSeconds > 0.0f)
		{
			World->GetTimerManager().SetTimer(StatusTimerHandle, FTimerDelegate::CreateUObject(this, &UBeanHUDWidget::ClearStatus), DurationSeconds, false);
		}
	}
}

void UBeanHUDWidget::ClearStatus()
{
	if (StatusPanel)
	{
		StatusPanel->SetVisibility(ESlateVisibility::Collapsed);
	}
}

// ---------------------------------------------------------------------------
// Progress panel
// ---------------------------------------------------------------------------

void UBeanHUDWidget::BuildProgressPanel()
{
	UCanvasPanel* Canvas = GetRootCanvas();
	if (!Canvas) return;

	ProgressPanel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("ProgressPanel"));
	ProgressPanel->SetBrushColor(FLinearColor(0.06f, 0.06f, 0.08f, 0.84f));
	ProgressPanel->SetPadding(FMargin(14.0f, 10.0f));
	// HitTestInvisible: the panel is a readout, it must never eat a place/remove click.
	ProgressPanel->SetVisibility(ESlateVisibility::HitTestInvisible);

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("ProgressColumn"));
	ProgressPanel->SetContent(Column);

	ProgressTitle = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ProgressTitle"));
	ProgressTitle->SetText(FText::FromString(TEXT("作品完成度")));
	ProgressTitle->SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 15));
	ProgressTitle->SetColorAndOpacity(FSlateColor(BeanTitleColor));
	ProgressTitle->SetShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.9f));
	Column->AddChildToVerticalBox(ProgressTitle);

	ProgressBar = WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass(), TEXT("ProgressBar"));
	ProgressBar->SetPercent(0.0f);
	ProgressBar->SetFillColorAndOpacity(FLinearColor(0.36f, 0.72f, 0.46f, 1.0f));
	USizeBox* BarSize = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("ProgressBarSize"));
	BarSize->SetWidthOverride(220.0f);
	BarSize->SetHeightOverride(14.0f);
	BarSize->AddChild(ProgressBar);
	UVerticalBoxSlot* BarSlot = Column->AddChildToVerticalBox(BarSize);
	BarSlot->SetPadding(FMargin(0.0f, 5.0f, 0.0f, 4.0f));

	ProgressPercent = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ProgressPercent"));
	ProgressPercent->SetText(FText::FromString(TEXT("还没有导入图案")));
	ProgressPercent->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 14));
	ProgressPercent->SetColorAndOpacity(FSlateColor(BeanBodyColor));
	ProgressPercent->SetShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.9f));
	Column->AddChildToVerticalBox(ProgressPercent);

	ProgressCounts = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ProgressCounts"));
	ProgressCounts->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 13));
	ProgressCounts->SetColorAndOpacity(FSlateColor(BeanMutedColor));
	ProgressCounts->SetShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.9f));
	ProgressCounts->SetAutoWrapText(true);
	USizeBox* CountsSize = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("ProgressCountsSize"));
	CountsSize->SetMaxDesiredWidth(260.0f);
	CountsSize->AddChild(ProgressCounts);
	Column->AddChildToVerticalBox(CountsSize);

	// Top-left, under the hint block: the top-right belongs to the exit buttons and the version tag.
	UCanvasPanelSlot* PanelSlot = Canvas->AddChildToCanvas(ProgressPanel);
	PanelSlot->SetAnchors(FAnchors(0.0f, 0.0f));
	PanelSlot->SetAlignment(FVector2D(0.0f, 0.0f));
	PanelSlot->SetPosition(FVector2D(14.0f, 232.0f));
	PanelSlot->SetAutoSize(true);
}

void UBeanHUDWidget::SetCompletionStats(const FBeanCompletionStats& Stats)
{
	if (ProgressTitle) ProgressTitle->SetText(FText::FromString(TEXT("作品完成度")));

	if (!Stats.bHasPattern)
	{
		if (ProgressBar) ProgressBar->SetPercent(0.0f);
		if (ProgressPercent) ProgressPercent->SetText(FText::FromString(TEXT("还没有导入图案")));
		if (ProgressCounts) ProgressCounts->SetText(FText::FromString(TEXT("导入图片后这里会显示完成进度。")));
		return;
	}

	const float Fraction = FMath::Clamp(Stats.CompletionPercent / 100.0f, 0.0f, 1.0f);
	if (ProgressBar) ProgressBar->SetPercent(Fraction);
	if (ProgressPercent)
	{
		ProgressPercent->SetText(FText::FromString(FString::Printf(
			TEXT("%.1f%%   已完成 %d / %d"), Stats.CompletionPercent, Stats.CorrectCells, Stats.TargetCells)));
	}
	if (ProgressCounts)
	{
		ProgressCounts->SetText(FText::FromString(FString::Printf(
			TEXT("未放 %d · 放错 %d · 多余 %d"), Stats.MissingCells, Stats.MiscoloredCells, Stats.ExtraCells)));
	}
}

// ---------------------------------------------------------------------------
// Selection / batch panel
// ---------------------------------------------------------------------------

void UBeanHUDWidget::BuildSelectionPanel()
{
	UCanvasPanel* Canvas = GetRootCanvas();
	if (!Canvas) return;

	SelectionPanel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("SelectionPanel"));
	SelectionPanel->SetBrushColor(FLinearColor(0.06f, 0.06f, 0.08f, 0.86f));
	SelectionPanel->SetPadding(FMargin(12.0f, 8.0f));
	SelectionPanel->SetVisibility(ESlateVisibility::Collapsed);

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("SelectionColumn"));
	SelectionPanel->SetContent(Column);

	SelectionLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("SelectionLabel"));
	SelectionLabel->SetText(FText::FromString(TEXT("框选：按住左键拖出一个矩形")));
	SelectionLabel->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 14));
	SelectionLabel->SetColorAndOpacity(FSlateColor(BeanBodyColor));
	SelectionLabel->SetShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.9f));
	SelectionLabel->SetJustification(ETextJustify::Center);
	UVerticalBoxSlot* LabelSlot = Column->AddChildToVerticalBox(SelectionLabel);
	LabelSlot->SetHorizontalAlignment(HAlign_Center);
	LabelSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 6.0f));

	UHorizontalBox* Row1 = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("SelectionRow1"));
	UHorizontalBox* Row2 = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("SelectionRow2"));

	auto AddBatchButton = [this](UHorizontalBox* Row, const FString& LabelText)
	{
		UButton* Button = MakePanelButton(LabelText, 112.0f, 30.0f, 13);
		UHorizontalBoxSlot* ButtonSlot = Row->AddChildToHorizontalBox(Button);
		ButtonSlot->SetPadding(FMargin(3.0f, 2.0f));
		BatchButtons.Add(Button);
		return Button;
	};

	AddBatchButton(Row1, TEXT("批量放置"))->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleBatchPlaceClicked);
	AddBatchButton(Row1, TEXT("批量移除"))->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleBatchRemoveClicked);
	AddBatchButton(Row1, TEXT("批量换色"))->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleBatchReplaceClicked);
	AddBatchButton(Row2, TEXT("按图案补齐"))->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleBatchFillClicked);
	AddBatchButton(Row2, TEXT("全选"))->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleSelectAllClicked);
	AddBatchButton(Row2, TEXT("取消选区"))->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleClearSelectionClicked);
	AddBatchButton(Row2, TEXT("清空画布"))->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleClearCanvasClicked);
	// An on-screen way out. Escape always left the mode, but the panel is a pointer UI, so a player
	// holding the mouse had no visible exit and the mode read as a trap (m03538: "无法退出").
	AddBatchButton(Row1, TEXT("退出框选"))->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleExitSelectionClicked);

	UVerticalBoxSlot* Row1Slot = Column->AddChildToVerticalBox(Row1);
	Row1Slot->SetHorizontalAlignment(HAlign_Center);
	UVerticalBoxSlot* Row2Slot = Column->AddChildToVerticalBox(Row2);
	Row2Slot->SetHorizontalAlignment(HAlign_Center);

	// Bottom-centre, above the two-row palette bar.
	UCanvasPanelSlot* PanelSlot = Canvas->AddChildToCanvas(SelectionPanel);
	PanelSlot->SetAnchors(FAnchors(0.5f, 1.0f));
	PanelSlot->SetAlignment(FVector2D(0.5f, 1.0f));
	PanelSlot->SetPosition(FVector2D(0.0f, -212.0f));
	PanelSlot->SetAutoSize(true);
}

void UBeanHUDWidget::SetSelectionPanelVisible(bool bVisible, bool bHasSelection, const FString& Summary)
{
	if (!SelectionPanel) return;
	SelectionPanel->SetVisibility(bVisible ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	if (bVisible)
	{
		SetSelectionSummary(Summary, bHasSelection);
	}
}

void UBeanHUDWidget::SetSelectionSummary(const FString& Summary, bool bHasSelection)
{
	if (SelectionLabel)
	{
		SelectionLabel->SetText(FText::FromString(Summary));
	}

	// 清空画布 only makes sense when nothing is selected (it is not a selection operation, but it
	// lives on this panel because that is where the destructive edits are). The batch buttons need
	// a selection to act on.
	const int32 ClearIndex = BatchButtons.Num() - 1;
	for (int32 Index = 0; Index < BatchButtons.Num(); ++Index)
	{
		if (!BatchButtons[Index]) continue;
		BatchButtons[Index]->SetIsEnabled(Index == ClearIndex ? !bHasSelection : bHasSelection);
	}
}

// ---------------------------------------------------------------------------
// Export preview
// ---------------------------------------------------------------------------

void UBeanHUDWidget::BuildExportPreview()
{
	UCanvasPanel* Canvas = GetRootCanvas();
	if (!Canvas) return;

	auto MakePreviewButton = [this](const FString& LabelText, float Width, float Height, int32 FontSize)
	{
		return MakePanelButton(LabelText, Width, Height, FontSize);
	};

	// Full-screen dim layer. Visible (not HitTestInvisible) on purpose: it swallows clicks, so the
	// palette and the button stack underneath cannot be driven while the picture is being judged.
	ExportPreviewPanel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("ExportPreviewPanel"));
	ExportPreviewPanel->SetBrushColor(FLinearColor(0.02f, 0.02f, 0.03f, 0.74f));
	ExportPreviewPanel->SetPadding(FMargin(24.0f));
	ExportPreviewPanel->SetHorizontalAlignment(HAlign_Center);
	ExportPreviewPanel->SetVerticalAlignment(VAlign_Center);
	ExportPreviewPanel->SetVisibility(ESlateVisibility::Collapsed);

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("ExportPreviewColumn"));

	ExportPreviewTitle = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ExportPreviewTitle"));
	ExportPreviewTitle->SetText(FText::FromString(TEXT("导出预览")));
	ExportPreviewTitle->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 20));
	ExportPreviewTitle->SetColorAndOpacity(FSlateColor(BeanTitleColor));
	ExportPreviewTitle->SetShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.9f));
	ExportPreviewTitle->SetShadowOffset(FVector2D(1.0f, 1.0f));
	ExportPreviewTitle->SetJustification(ETextJustify::Center);
	UVerticalBoxSlot* TitleSlot = Column->AddChildToVerticalBox(ExportPreviewTitle);
	TitleSlot->SetHorizontalAlignment(HAlign_Center);
	TitleSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 10.0f));

	// The picture itself. Its on-screen size is set from the texture's aspect ratio every time a new
	// preview is built, so nothing here fixes a size.
	ExportPreviewImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("ExportPreviewImage"));
	ExportPreviewImage->SetVisibility(ESlateVisibility::HitTestInvisible);
	UVerticalBoxSlot* ImageSlot = Column->AddChildToVerticalBox(ExportPreviewImage);
	ImageSlot->SetHorizontalAlignment(HAlign_Center);

	ExportPreviewInfo = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ExportPreviewInfo"));
	ExportPreviewInfo->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 15));
	ExportPreviewInfo->SetColorAndOpacity(FSlateColor(BeanBodyColor));
	ExportPreviewInfo->SetShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.9f));
	ExportPreviewInfo->SetShadowOffset(FVector2D(1.0f, 1.0f));
	ExportPreviewInfo->SetJustification(ETextJustify::Center);
	UVerticalBoxSlot* InfoSlot = Column->AddChildToVerticalBox(ExportPreviewInfo);
	InfoSlot->SetHorizontalAlignment(HAlign_Center);
	InfoSlot->SetPadding(FMargin(0.0f, 12.0f, 0.0f, 8.0f));

	// Notice line: cancel notice, export error, ... Collapsed until there is something to say.
	ExportPreviewMessage = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ExportPreviewMessage"));
	ExportPreviewMessage->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 14));
	ExportPreviewMessage->SetColorAndOpacity(FSlateColor(BeanNoticeColor));
	ExportPreviewMessage->SetShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.9f));
	ExportPreviewMessage->SetShadowOffset(FVector2D(1.0f, 1.0f));
	ExportPreviewMessage->SetJustification(ETextJustify::Center);
	ExportPreviewMessage->SetAutoWrapText(true);
	ExportPreviewMessage->SetVisibility(ESlateVisibility::Collapsed);
	USizeBox* MessageSize = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("ExportPreviewMessageSize"));
	MessageSize->SetMaxDesiredWidth(720.0f);
	MessageSize->AddChild(ExportPreviewMessage);
	UVerticalBoxSlot* MessageSlot = Column->AddChildToVerticalBox(MessageSize);
	MessageSlot->SetHorizontalAlignment(HAlign_Center);
	MessageSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 6.0f));

	// ---- scale row ----
	UHorizontalBox* ScaleRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("ExportPreviewScaleRow"));
	auto AddScaleButton = [this, ScaleRow, &MakePreviewButton](const FString& LabelText)
	{
		UButton* Button = MakePreviewButton(LabelText, 88.0f, 30.0f, 13);
		UHorizontalBoxSlot* ButtonSlot = ScaleRow->AddChildToHorizontalBox(Button);
		ButtonSlot->SetPadding(FMargin(4.0f, 0.0f));
		ExportScaleButtons.Add(Button);
		return Button;
	};

	// OnClicked is a dynamic delegate and cannot carry the value, so each scale gets its own
	// argument-free handler; these four mirror ABeanGrid::GetExportScaleOptions().
	AddScaleButton(TEXT("4 像素"))->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleExportScale4Clicked);
	AddScaleButton(TEXT("8 像素"))->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleExportScale8Clicked);
	AddScaleButton(TEXT("16 像素"))->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleExportScale16Clicked);
	AddScaleButton(TEXT("32 像素"))->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleExportScale32Clicked);

	UVerticalBoxSlot* ScaleRowSlot = Column->AddChildToVerticalBox(ScaleRow);
	ScaleRowSlot->SetHorizontalAlignment(HAlign_Center);
	ScaleRowSlot->SetPadding(FMargin(0.0f, 4.0f, 0.0f, 4.0f));

	UTextBlock* Footer = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ExportPreviewFooter"));
	Footer->SetText(FText::FromString(TEXT("保存前不会写入任何文件：满意就保存，想改就换个比例，或者返回继续拼豆。")));
	Footer->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 13));
	Footer->SetColorAndOpacity(FSlateColor(BeanMutedColor));
	Footer->SetShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.9f));
	Footer->SetShadowOffset(FVector2D(1.0f, 1.0f));
	Footer->SetJustification(ETextJustify::Center);
	UVerticalBoxSlot* FooterSlot = Column->AddChildToVerticalBox(Footer);
	FooterSlot->SetHorizontalAlignment(HAlign_Center);
	FooterSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 12.0f));

	// ---- save / go back ----
	UHorizontalBox* ActionRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("ExportPreviewActionRow"));

	UButton* SaveButton = MakePreviewButton(TEXT("保存图片"), 150.0f, 40.0f, 16);
	SaveButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleExportSaveClicked);
	UHorizontalBoxSlot* SaveSlot = ActionRow->AddChildToHorizontalBox(SaveButton);
	SaveSlot->SetPadding(FMargin(6.0f, 0.0f));

	UButton* BackButton = MakePreviewButton(TEXT("返回继续拼豆"), 170.0f, 40.0f, 16);
	BackButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleExportBackClicked);
	UHorizontalBoxSlot* BackSlot = ActionRow->AddChildToHorizontalBox(BackButton);
	BackSlot->SetPadding(FMargin(6.0f, 0.0f));

	UVerticalBoxSlot* ActionRowSlot = Column->AddChildToVerticalBox(ActionRow);
	ActionRowSlot->SetHorizontalAlignment(HAlign_Center);

	ExportPreviewPanel->SetContent(Column);

	UCanvasPanelSlot* PanelSlot = Canvas->AddChildToCanvas(ExportPreviewPanel);
	PanelSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
	PanelSlot->SetOffsets(FMargin(0.0f));
	PanelSlot->SetAlignment(FVector2D(0.0f, 0.0f));
}

void UBeanHUDWidget::ShowExportPreview(UTexture2D* Texture, int32 PixelsPerCell, int32 CellsW, int32 CellsH)
{
	if (!ExportPreviewPanel || !ExportPreviewImage || !Texture) return;

	ExportPreviewPixelsPerCell = PixelsPerCell;
	ExportPreviewCellsW = CellsW;
	ExportPreviewCellsH = CellsH;

	ExportPreviewImage->SetBrushFromTexture(Texture, false);

	// Fit the picture into the middle of the screen, aspect preserved. It is shown at whatever size
	// makes it readable: judging "is this scale good enough to look at" is the whole point, and the
	// nearest-filtered texture keeps the bead edges hard when it is blown up.
	const int32 TexW = FMath::Max(1, Texture->GetSizeX());
	const int32 TexH = FMath::Max(1, Texture->GetSizeY());

	FVector2D ViewportSize(1280.0f, 720.0f);
	if (UWorld* World = GetWorld())
	{
		if (const UGameViewportClient* Viewport = World->GetGameViewport())
		{
			Viewport->GetViewportSize(ViewportSize);
		}
	}
	const float MaxWidth = FMath::Max(160.0f, ViewportSize.X * 0.72f);
	const float MaxHeight = FMath::Max(120.0f, ViewportSize.Y * 0.52f);
	const float Fit = FMath::Min(MaxWidth / static_cast<float>(TexW), MaxHeight / static_cast<float>(TexH));
	ExportPreviewImage->SetDesiredSizeOverride(FVector2D(
		FMath::Max(1.0f, FMath::RoundToFloat(TexW * Fit)),
		FMath::Max(1.0f, FMath::RoundToFloat(TexH * Fit))));

	RefreshExportPreview();
	SetExportPreviewMessage(FString());

	ExportPreviewPanel->SetVisibility(ESlateVisibility::Visible);
}

void UBeanHUDWidget::HideExportPreview()
{
	if (ExportPreviewPanel)
	{
		ExportPreviewPanel->SetVisibility(ESlateVisibility::Collapsed);
	}
}

void UBeanHUDWidget::SetExportPreviewMessage(const FString& Message)
{
	if (!ExportPreviewMessage) return;

	ExportPreviewMessage->SetText(FText::FromString(Message));
	ExportPreviewMessage->SetVisibility(Message.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
}

void UBeanHUDWidget::RefreshExportPreview()
{
	if (ExportPreviewInfo)
	{
		const int32 SizeX = ExportPreviewCellsW * ExportPreviewPixelsPerCell;
		const int32 SizeY = ExportPreviewCellsH * ExportPreviewPixelsPerCell;
		ExportPreviewInfo->SetText(FText::FromString(FString::Printf(
			TEXT("每个豆 %d 像素 · 成品 %d × %d 像素"), ExportPreviewPixelsPerCell, SizeX, SizeY)));
	}

	// The scale row doubles as a readout: the scale on screen is highlighted, and the ones whose
	// picture would be past the export limit are greyed out instead of failing after the click.
	const TArray<int32>& Options = ABeanGrid::GetExportScaleOptions();
	for (int32 Index = 0; Index < ExportScaleButtons.Num(); ++Index)
	{
		UButton* Button = ExportScaleButtons[Index];
		if (!Button) continue;

		const int32 Option = Options.IsValidIndex(Index) ? Options[Index] : 0;
		const bool bFits = Option > 0
			&& ExportPreviewCellsW * Option <= ABeanGrid::MaxExportSize
			&& ExportPreviewCellsH * Option <= ABeanGrid::MaxExportSize;
		Button->SetIsEnabled(bFits);

		FButtonStyle Style = Button->GetStyle();
		Style.Normal.TintColor = FSlateColor(Option == ExportPreviewPixelsPerCell
			? FLinearColor(0.34f, 0.40f, 0.62f, 0.98f)
			: FLinearColor(0.16f, 0.16f, 0.20f, 0.94f));
		Button->SetStyle(Style);
	}
}

// ---------------------------------------------------------------------------
// Import preview
// ---------------------------------------------------------------------------

void UBeanHUDWidget::BuildImportPreview()
{
	UCanvasPanel* Canvas = GetRootCanvas();
	if (!Canvas) return;

	ImportPreviewPanel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("ImportPreviewPanel"));
	ImportPreviewPanel->SetBrushColor(FLinearColor(0.02f, 0.02f, 0.03f, 0.86f));
	ImportPreviewPanel->SetPadding(FMargin(22.0f));
	ImportPreviewPanel->SetHorizontalAlignment(HAlign_Center);
	ImportPreviewPanel->SetVerticalAlignment(VAlign_Center);
	ImportPreviewPanel->SetVisibility(ESlateVisibility::Collapsed);

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("ImportPreviewColumn"));
	ImportPreviewPanel->SetContent(Column);

	UTextBlock* Title = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ImportPreviewTitle"));
	Title->SetText(FText::FromString(TEXT("转化预览")));
	Title->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 20));
	Title->SetColorAndOpacity(FSlateColor(BeanTitleColor));
	Title->SetShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.9f));
	Title->SetJustification(ETextJustify::Center);
	UVerticalBoxSlot* TitleSlot = Column->AddChildToVerticalBox(Title);
	TitleSlot->SetHorizontalAlignment(HAlign_Center);
	TitleSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 8.0f));

	// ---- original | converted ----
	UHorizontalBox* PictureRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("ImportPictureRow"));

	auto MakePictureColumn = [this, PictureRow](const FString& CaptionText, UImage*& OutImage)
	{
		UVerticalBox* PictureColumn = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());

		UTextBlock* Caption = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Caption->SetText(FText::FromString(CaptionText));
		Caption->SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 14));
		Caption->SetColorAndOpacity(FSlateColor(BeanBodyColor));
		Caption->SetJustification(ETextJustify::Center);
		UVerticalBoxSlot* CaptionSlot = PictureColumn->AddChildToVerticalBox(Caption);
		CaptionSlot->SetHorizontalAlignment(HAlign_Center);

		OutImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
		OutImage->SetVisibility(ESlateVisibility::HitTestInvisible);
		USizeBox* ImageBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		ImageBox->SetWidthOverride(360.0f);
		ImageBox->SetHeightOverride(300.0f);
		ImageBox->AddChild(OutImage);
		UVerticalBoxSlot* ImageSlot = PictureColumn->AddChildToVerticalBox(ImageBox);
		ImageSlot->SetHorizontalAlignment(HAlign_Center);
		ImageSlot->SetPadding(FMargin(0.0f, 4.0f, 0.0f, 0.0f));

		UHorizontalBoxSlot* ColumnSlot = PictureRow->AddChildToHorizontalBox(PictureColumn);
		ColumnSlot->SetPadding(FMargin(8.0f, 0.0f));
	};

	MakePictureColumn(TEXT("原始图片"), ImportSourceImage);
	MakePictureColumn(TEXT("转化结果"), ImportConvertedImage);

	UVerticalBoxSlot* PictureRowSlot = Column->AddChildToVerticalBox(PictureRow);
	PictureRowSlot->SetHorizontalAlignment(HAlign_Center);

	// ---- readouts ----
	ImportInfoLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ImportInfoLabel"));
	ImportInfoLabel->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 14));
	ImportInfoLabel->SetColorAndOpacity(FSlateColor(BeanBodyColor));
	ImportInfoLabel->SetJustification(ETextJustify::Center);
	ImportInfoLabel->SetAutoWrapText(true);
	USizeBox* InfoSize = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("ImportInfoSize"));
	InfoSize->SetMaxDesiredWidth(820.0f);
	InfoSize->AddChild(ImportInfoLabel);
	UVerticalBoxSlot* InfoSlot = Column->AddChildToVerticalBox(InfoSize);
	InfoSlot->SetHorizontalAlignment(HAlign_Center);
	InfoSlot->SetPadding(FMargin(0.0f, 10.0f, 0.0f, 4.0f));

	ImportMessageLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ImportMessageLabel"));
	ImportMessageLabel->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 14));
	ImportMessageLabel->SetColorAndOpacity(FSlateColor(BeanNoticeColor));
	ImportMessageLabel->SetJustification(ETextJustify::Center);
	ImportMessageLabel->SetAutoWrapText(true);
	ImportMessageLabel->SetVisibility(ESlateVisibility::Collapsed);
	USizeBox* MessageSize = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("ImportMessageSize"));
	MessageSize->SetMaxDesiredWidth(820.0f);
	MessageSize->AddChild(ImportMessageLabel);
	UVerticalBoxSlot* MessageSlot = Column->AddChildToVerticalBox(MessageSize);
	MessageSlot->SetHorizontalAlignment(HAlign_Center);
	MessageSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 6.0f));

	// Import is deliberately hands-off: the whole picture is fitted into the canvas, so there is
	// nothing for a crop box, a nudge or a zoom to do. Those controls were removed at the player's
	// request after they made the common case (drop a photo in, keep all of it) the awkward one.

	// ---- colour count ----
	UHorizontalBox* ColorRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("ImportColorRow"));
	auto AddColorButton = [this, ColorRow](const FString& LabelText)
	{
		UButton* Button = MakePanelButton(LabelText, 86.0f, 28.0f, 13);
		UHorizontalBoxSlot* ButtonSlot = ColorRow->AddChildToHorizontalBox(Button);
		ButtonSlot->SetPadding(FMargin(3.0f, 0.0f));
		ImportColorButtons.Add(Button);
		return Button;
	};
	AddColorButton(TEXT("全部颜色"))->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleImportColorsAllClicked);
	AddColorButton(TEXT("24 色"))->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleImportColors24Clicked);
	AddColorButton(TEXT("16 色"))->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleImportColors16Clicked);
	AddColorButton(TEXT("8 色"))->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleImportColors8Clicked);

	ImportDitherButton = MakePanelButton(TEXT("抖动：关"), 100.0f, 28.0f, 13);
	ImportDitherButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleImportDitherClicked);
	UHorizontalBoxSlot* DitherSlot = ColorRow->AddChildToHorizontalBox(ImportDitherButton);
	DitherSlot->SetPadding(FMargin(9.0f, 0.0f, 3.0f, 0.0f));

	UVerticalBoxSlot* ColorRowSlot = Column->AddChildToVerticalBox(ColorRow);
	ColorRowSlot->SetHorizontalAlignment(HAlign_Center);
	ColorRowSlot->SetPadding(FMargin(0.0f, 2.0f));

	// ---- canvas shape + resolution ----
	UHorizontalBox* CanvasRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("ImportCanvasRow"));
	auto AddPresetButton = [this, CanvasRow](const FString& LabelText)
	{
		UButton* Button = MakePanelButton(LabelText, 92.0f, 28.0f, 13);
		UHorizontalBoxSlot* ButtonSlot = CanvasRow->AddChildToHorizontalBox(Button);
		ButtonSlot->SetPadding(FMargin(3.0f, 0.0f));
		ImportPresetButtons.Add(Button);
		return Button;
	};
	AddPresetButton(TEXT("方形画布"))->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleImportPresetSquareClicked);
	AddPresetButton(TEXT("横向画布"))->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleImportPresetLandscapeClicked);
	AddPresetButton(TEXT("纵向画布"))->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleImportPresetPortraitClicked);

	UButton* ResolutionButton = MakePanelButton(TEXT("分辨率 256"), 120.0f, 28.0f, 13);
	ResolutionButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleImportResolutionClicked);
	ImportResolutionButtons.Add(ResolutionButton);
	UHorizontalBoxSlot* ResolutionSlot = CanvasRow->AddChildToHorizontalBox(ResolutionButton);
	ResolutionSlot->SetPadding(FMargin(9.0f, 0.0f, 3.0f, 0.0f));

	UVerticalBoxSlot* CanvasRowSlot = Column->AddChildToVerticalBox(CanvasRow);
	CanvasRowSlot->SetHorizontalAlignment(HAlign_Center);
	CanvasRowSlot->SetPadding(FMargin(0.0f, 2.0f));

	// ---- apply / cancel ----
	UHorizontalBox* ActionRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("ImportActionRow"));

	UButton* ApplyBatchButton = MakePanelButton(TEXT("应用为提示层"), 170.0f, 40.0f, 15);
	ApplyBatchButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleImportApplyClicked);
	UHorizontalBoxSlot* ApplySlot = ActionRow->AddChildToHorizontalBox(ApplyBatchButton);
	ApplySlot->SetPadding(FMargin(6.0f, 0.0f));

	UButton* FillButton = MakePanelButton(TEXT("应用并铺满画布"), 170.0f, 40.0f, 15);
	FillButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleImportFillClicked);
	UHorizontalBoxSlot* FillSlot = ActionRow->AddChildToHorizontalBox(FillButton);
	FillSlot->SetPadding(FMargin(6.0f, 0.0f));

	UButton* CancelImportButton = MakePanelButton(TEXT("取消导入"), 130.0f, 40.0f, 15);
	CancelImportButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleImportCancelClicked);
	UHorizontalBoxSlot* CancelSlot = ActionRow->AddChildToHorizontalBox(CancelImportButton);
	CancelSlot->SetPadding(FMargin(6.0f, 0.0f));

	UVerticalBoxSlot* ActionRowSlot = Column->AddChildToVerticalBox(ActionRow);
	ActionRowSlot->SetHorizontalAlignment(HAlign_Center);
	ActionRowSlot->SetPadding(FMargin(0.0f, 6.0f, 0.0f, 0.0f));

	// 应用 is the grid's own button; the preview panel drives its own pair of buttons above.
	ImportApplyButton = ApplyBatchButton;

	UCanvasPanelSlot* PanelSlot = Canvas->AddChildToCanvas(ImportPreviewPanel);
	PanelSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
	PanelSlot->SetOffsets(FMargin(0.0f));
	PanelSlot->SetAlignment(FVector2D(0.0f, 0.0f));
}

void UBeanHUDWidget::ShowImportPreview(UTexture2D* SourceTexture, UTexture2D* ConvertedTexture)
{
	if (!ImportPreviewPanel) return;

	auto FitInto = [this](UImage* Image, UTexture2D* Texture, float MaxWidth, float MaxHeight)
	{
		if (!Image) return;
		if (!Texture)
		{
			Image->SetBrushFromTexture(nullptr, false);
			return;
		}
		Image->SetBrushFromTexture(Texture, false);
		const int32 TexW = FMath::Max(1, Texture->GetSizeX());
		const int32 TexH = FMath::Max(1, Texture->GetSizeY());
		const float Fit = FMath::Min(MaxWidth / static_cast<float>(TexW), MaxHeight / static_cast<float>(TexH));
		Image->SetDesiredSizeOverride(FVector2D(
			FMath::Max(1.0f, FMath::RoundToFloat(TexW * Fit)),
			FMath::Max(1.0f, FMath::RoundToFloat(TexH * Fit))));
	};

	FitInto(ImportSourceImage, SourceTexture, 360.0f, 300.0f);
	FitInto(ImportConvertedImage, ConvertedTexture, 360.0f, 300.0f);

	ImportPreviewPanel->SetVisibility(ESlateVisibility::Visible);
}

void UBeanHUDWidget::HideImportPreview()
{
	if (ImportPreviewPanel)
	{
		ImportPreviewPanel->SetVisibility(ESlateVisibility::Collapsed);
	}
}

bool UBeanHUDWidget::IsImportPreviewOpen() const
{
	return ImportPreviewPanel && ImportPreviewPanel->GetVisibility() == ESlateVisibility::Visible;
}

void UBeanHUDWidget::SetImportPreviewInfo(const FString& Info, const FString& Message, bool bHasResult,
	bool bDither, int32 MaxColors, int32 Resolution, EBeanCanvasPreset Preset)
{
	bImportDitherShown = bDither;
	ImportMaxColorsShown = MaxColors;
	ImportResolutionShown = Resolution;
	ImportPresetShown = Preset;

	if (ImportInfoLabel) ImportInfoLabel->SetText(FText::FromString(Info));
	if (ImportMessageLabel)
	{
		ImportMessageLabel->SetText(FText::FromString(Message));
		ImportMessageLabel->SetVisibility(Message.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}
	if (ImportApplyButton) ImportApplyButton->SetIsEnabled(bHasResult);

	// The tool buttons double as state readouts, so every one of them is re-labelled here.
	SetPanelButtonLabel(ImportDitherButton, bDither ? TEXT("抖动：开") : TEXT("抖动：关"));
	SetPanelButtonLabel(ImportResolutionButtons.IsValidIndex(0) ? ImportResolutionButtons[0] : nullptr,
		FString::Printf(TEXT("分辨率 %d"), FMath::Max(1, Resolution)));

	// Highlight the active colour count / canvas shape button.
	const int32 ColorOptions[] = { 0, 24, 16, 8 };
	for (int32 Index = 0; Index < ImportColorButtons.Num(); ++Index)
	{
		SetPanelButtonHighlight(ImportColorButtons[Index], ColorOptions[Index] == MaxColors);
	}

	const EBeanCanvasPreset Presets[] = { EBeanCanvasPreset::Square256, EBeanCanvasPreset::Landscape4x3, EBeanCanvasPreset::Portrait3x4 };
	for (int32 Index = 0; Index < ImportPresetButtons.Num(); ++Index)
	{
		SetPanelButtonHighlight(ImportPresetButtons[Index], Presets[Index] == Preset);
	}
}

void UBeanHUDWidget::SetImportPreviewError(const FString& Message)
{
	if (ImportMessageLabel)
	{
		ImportMessageLabel->SetText(FText::FromString(Message));
		ImportMessageLabel->SetVisibility(Message.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}
	if (ImportApplyButton) ImportApplyButton->SetIsEnabled(false);
}

// ---------------------------------------------------------------------------
// Gallery
// ---------------------------------------------------------------------------

void UBeanHUDWidget::BuildGallery()
{
	UCanvasPanel* Canvas = GetRootCanvas();
	if (!Canvas) return;

	GalleryPanel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("GalleryPanel"));
	GalleryPanel->SetBrushColor(FLinearColor(0.02f, 0.02f, 0.03f, 0.9f));
	GalleryPanel->SetPadding(FMargin(28.0f));
	GalleryPanel->SetHorizontalAlignment(HAlign_Center);
	GalleryPanel->SetVerticalAlignment(VAlign_Center);
	GalleryPanel->SetVisibility(ESlateVisibility::Collapsed);

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("GalleryColumn"));
	GalleryPanel->SetContent(Column);

	UTextBlock* Title = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("GalleryTitle"));
	Title->SetText(FText::FromString(TEXT("作品画廊")));
	Title->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 22));
	Title->SetColorAndOpacity(FSlateColor(BeanTitleColor));
	Title->SetJustification(ETextJustify::Center);
	UVerticalBoxSlot* TitleSlot = Column->AddChildToVerticalBox(Title);
	TitleSlot->SetHorizontalAlignment(HAlign_Center);
	TitleSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 10.0f));

	// Two actions belong in the gallery as much as in the side stack: this is where a player realises
	// "I want to keep this one" or "I want a fresh canvas", and having to walk back to the right-hand
	// stack first made both flows longer than they needed to be (m04820).
	UHorizontalBox* TitleActions = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("GalleryTitleActions"));
	UButton* GallerySaveButton = MakePanelButton(TEXT("保存当前作品"), 170.0f, 36.0f, 15);
	GallerySaveButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleSaveClicked);
	UHorizontalBoxSlot* GallerySaveSlot = TitleActions->AddChildToHorizontalBox(GallerySaveButton);
	GallerySaveSlot->SetPadding(FMargin(0.0f, 0.0f, 10.0f, 0.0f));

	UButton* GalleryNewCanvasButton = MakePanelButton(TEXT("新建画布"), 150.0f, 36.0f, 15);
	GalleryNewCanvasButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleNewCanvasClicked);
	TitleActions->AddChildToHorizontalBox(GalleryNewCanvasButton);

	UVerticalBoxSlot* TitleActionsSlot = Column->AddChildToVerticalBox(TitleActions);
	TitleActionsSlot->SetHorizontalAlignment(HAlign_Center);
	TitleActionsSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 10.0f));

	GalleryEmptyLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("GalleryEmpty"));
	GalleryEmptyLabel->SetText(FText::FromString(TEXT("还没有保存过作品。按 Ctrl+S 或点「保存当前作品」保存；「新建画布」可以另起一张。")));
	GalleryEmptyLabel->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 15));
	GalleryEmptyLabel->SetColorAndOpacity(FSlateColor(BeanMutedColor));
	GalleryEmptyLabel->SetJustification(ETextJustify::Center);
	UVerticalBoxSlot* EmptySlot = Column->AddChildToVerticalBox(GalleryEmptyLabel);
	EmptySlot->SetHorizontalAlignment(HAlign_Center);
	EmptySlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 8.0f));

	// The list is scrollable: a gallery with dozens of works must not push the buttons off screen.
	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("GalleryScroll"));
	Scroll->SetOrientation(Orient_Vertical);
	USizeBox* ScrollSize = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("GalleryScrollSize"));
	ScrollSize->SetWidthOverride(940.0f);
	ScrollSize->SetHeightOverride(460.0f);
	ScrollSize->AddChild(Scroll);

	GalleryList = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("GalleryList"));
	Scroll->AddChild(GalleryList);

	UVerticalBoxSlot* ScrollSlot = Column->AddChildToVerticalBox(ScrollSize);
	ScrollSlot->SetHorizontalAlignment(HAlign_Center);

	GalleryMessage = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("GalleryMessage"));
	GalleryMessage->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 14));
	GalleryMessage->SetColorAndOpacity(FSlateColor(BeanNoticeColor));
	GalleryMessage->SetJustification(ETextJustify::Center);
	GalleryMessage->SetAutoWrapText(true);
	GalleryMessage->SetVisibility(ESlateVisibility::Collapsed);
	USizeBox* MessageSize = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("GalleryMessageSize"));
	MessageSize->SetMaxDesiredWidth(900.0f);
	MessageSize->AddChild(GalleryMessage);
	UVerticalBoxSlot* MessageSlot = Column->AddChildToVerticalBox(MessageSize);
	MessageSlot->SetHorizontalAlignment(HAlign_Center);
	MessageSlot->SetPadding(FMargin(0.0f, 8.0f, 0.0f, 6.0f));

	UButton* CloseButton = MakePanelButton(TEXT("返回拼豆"), 150.0f, 38.0f, 15);
	CloseButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleGalleryCloseClicked);
	UVerticalBoxSlot* CloseSlot = Column->AddChildToVerticalBox(CloseButton);
	CloseSlot->SetHorizontalAlignment(HAlign_Center);

	UCanvasPanelSlot* PanelSlot = Canvas->AddChildToCanvas(GalleryPanel);
	PanelSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
	PanelSlot->SetOffsets(FMargin(0.0f));
	PanelSlot->SetAlignment(FVector2D(0.0f, 0.0f));
}

void UBeanHUDWidget::ShowGallery()
{
	if (GalleryPanel) GalleryPanel->SetVisibility(ESlateVisibility::Visible);
}

void UBeanHUDWidget::HideGallery()
{
	if (GalleryPanel) GalleryPanel->SetVisibility(ESlateVisibility::Collapsed);
}

void UBeanHUDWidget::SetGalleryRows(const TArray<FString>& SlotNames, const TArray<FString>& WorkNames,
	const TArray<UTexture2D*>& Thumbnails, const TArray<FString>& MetaLines,
	const TArray<bool>& Readable, const TArray<FString>& Problems)
{
	if (!GalleryList) return;

	GalleryList->ClearChildren();
	GalleryRows.Reset();

	const bool bHasRows = SlotNames.Num() > 0;
	if (GalleryEmptyLabel)
	{
		GalleryEmptyLabel->SetVisibility(bHasRows ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}

	for (int32 Index = 0; Index < SlotNames.Num(); ++Index)
	{
		UBeanGalleryItemWidget* Row = CreateWidget<UBeanGalleryItemWidget>(GetWorld(), UBeanGalleryItemWidget::StaticClass());
		if (!Row) continue;

		Row->Setup(SlotNames[Index], WorkNames.IsValidIndex(Index) ? WorkNames[Index] : SlotNames[Index],
			Thumbnails.IsValidIndex(Index) ? Thumbnails[Index] : nullptr,
			MetaLines.IsValidIndex(Index) ? MetaLines[Index] : FString(),
			Readable.IsValidIndex(Index) ? Readable[Index] : true,
			Problems.IsValidIndex(Index) ? Problems[Index] : FString(), 88.0f);

		// One shared action set: the controller fills GalleryActions in once and every row uses it.
		Row->Actions = GalleryActions;





		UVerticalBoxSlot* RowSlot = GalleryList->AddChildToVerticalBox(Row);
		RowSlot->SetPadding(FMargin(0.0f, 4.0f));
		GalleryRows.Add(Row);
	}
}

void UBeanHUDWidget::SetGalleryMessage(const FString& Message)
{
	if (!GalleryMessage) return;
	GalleryMessage->SetText(FText::FromString(Message));
	GalleryMessage->SetVisibility(Message.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
}

// ---------------------------------------------------------------------------
// Screenshot / display mode
// ---------------------------------------------------------------------------

void UBeanHUDWidget::BuildScreenshotMode()
{
	UCanvasPanel* Canvas = GetRootCanvas();
	if (!Canvas) return;

	// Only a small control bar: the point of the mode is a clean picture, so the panel is a strip at
	// the bottom and can hide itself entirely before the capture.
	ScreenshotPanel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("ScreenshotPanel"));
	ScreenshotPanel->SetBrushColor(FLinearColor(0.06f, 0.06f, 0.08f, 0.86f));
	ScreenshotPanel->SetPadding(FMargin(14.0f, 8.0f));
	ScreenshotPanel->SetVisibility(ESlateVisibility::Collapsed);

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("ScreenshotColumn"));
	ScreenshotPanel->SetContent(Column);

	ScreenshotStatus = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ScreenshotStatus"));
	ScreenshotStatus->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 14));
	ScreenshotStatus->SetColorAndOpacity(FSlateColor(BeanBodyColor));
	ScreenshotStatus->SetJustification(ETextJustify::Center);
	UVerticalBoxSlot* StatusSlot = Column->AddChildToVerticalBox(ScreenshotStatus);
	StatusSlot->SetHorizontalAlignment(HAlign_Center);

	ScreenshotMessage = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ScreenshotMessage"));
	ScreenshotMessage->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 13));
	ScreenshotMessage->SetColorAndOpacity(FSlateColor(BeanNoticeColor));
	ScreenshotMessage->SetJustification(ETextJustify::Center);
	ScreenshotMessage->SetAutoWrapText(true);
	ScreenshotMessage->SetVisibility(ESlateVisibility::Collapsed);
	UVerticalBoxSlot* MessageSlot = Column->AddChildToVerticalBox(ScreenshotMessage);
	MessageSlot->SetHorizontalAlignment(HAlign_Center);

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("ScreenshotRow"));

	UButton* StyleButton = MakePanelButton(TEXT("切换风格"), 110.0f, 32.0f, 14);
	StyleButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleScreenshotStyleClicked);
	UButton* HideUIButton = MakePanelButton(TEXT("隐藏界面"), 110.0f, 32.0f, 14);
	HideUIButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleScreenshotHideUIClicked);
	UButton* CaptureButton = MakePanelButton(TEXT("截图保存"), 130.0f, 32.0f, 14);
	CaptureButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleScreenshotCaptureClicked);
	UButton* CloseButton = MakePanelButton(TEXT("退出展示"), 110.0f, 32.0f, 14);
	CloseButton->OnClicked.AddDynamic(this, &UBeanHUDWidget::HandleScreenshotCloseClicked);

	for (UButton* Button : { StyleButton, HideUIButton, CaptureButton, CloseButton })
	{
		UHorizontalBoxSlot* ButtonSlot = Row->AddChildToHorizontalBox(Button);
		ButtonSlot->SetPadding(FMargin(4.0f, 2.0f));
	}

	UVerticalBoxSlot* RowSlot = Column->AddChildToVerticalBox(Row);
	RowSlot->SetHorizontalAlignment(HAlign_Center);

	UCanvasPanelSlot* PanelSlot = Canvas->AddChildToCanvas(ScreenshotPanel);
	PanelSlot->SetAnchors(FAnchors(0.5f, 1.0f));
	PanelSlot->SetAlignment(FVector2D(0.5f, 1.0f));
	PanelSlot->SetPosition(FVector2D(0.0f, -12.0f));
	PanelSlot->SetAutoSize(true);
}

void UBeanHUDWidget::ShowScreenshotMode()
{
	if (ScreenshotPanel) ScreenshotPanel->SetVisibility(ESlateVisibility::Visible);
}

void UBeanHUDWidget::HideScreenshotMode()
{
	if (ScreenshotPanel) ScreenshotPanel->SetVisibility(ESlateVisibility::Collapsed);
}

void UBeanHUDWidget::SetScreenshotStatus(const FString& StyleText, const FString& Message)
{
	if (ScreenshotStatus) ScreenshotStatus->SetText(FText::FromString(StyleText));
	if (ScreenshotMessage)
	{
		ScreenshotMessage->SetText(FText::FromString(Message));
		ScreenshotMessage->SetVisibility(Message.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}
}

void UBeanHUDWidget::SetScreenshotUIHidden(bool bHidden)
{
	// Hiding the whole HUD is what makes a clean picture possible: the status strip and the hint
	// block would otherwise end up in the captured frame.
	SetHUDVisible(!bHidden);
}

// ---------------------------------------------------------------------------
// Button handlers
// ---------------------------------------------------------------------------

void UBeanHUDWidget::SetHistoryState(bool bCanUndo, bool bCanRedo, const FString& UndoLabel, const FString& RedoLabel)
{
	if (UndoButton)
	{
		UndoButton->SetIsEnabled(bCanUndo);
	}
	if (RedoButton)
	{
		RedoButton->SetIsEnabled(bCanRedo);
	}
}

void UBeanHUDWidget::HandleImportClicked()
{
	if (Controller)
	{
		Controller->ImportImage();
	}
}

void UBeanHUDWidget::HandleApplyFillClicked()
{
	if (Controller)
	{
		Controller->ApplyImageAndFill();
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

void UBeanHUDWidget::HandleExportClicked()
{
	if (Controller)
	{
		Controller->ExportImage();
	}
}

void UBeanHUDWidget::HandleExportScale4Clicked()
{
	if (Controller)
	{
		Controller->SetExportPreviewScale(4);
	}
}

void UBeanHUDWidget::HandleExportScale8Clicked()
{
	if (Controller)
	{
		Controller->SetExportPreviewScale(8);
	}
}

void UBeanHUDWidget::HandleExportScale16Clicked()
{
	if (Controller)
	{
		Controller->SetExportPreviewScale(16);
	}
}

void UBeanHUDWidget::HandleExportScale32Clicked()
{
	if (Controller)
	{
		Controller->SetExportPreviewScale(32);
	}
}

void UBeanHUDWidget::HandleExportSaveClicked()
{
	if (Controller)
	{
		Controller->SaveExportImage();
	}
}

void UBeanHUDWidget::HandleExportBackClicked()
{
	if (Controller)
	{
		Controller->CloseExportPreview();
	}
}

void UBeanHUDWidget::HandleExitClicked()
{
	APlayerController* PC = Controller ? Cast<APlayerController>(Controller) : UGameplayStatics::GetPlayerController(GetWorld(), 0);
	UKismetSystemLibrary::QuitGame(this, PC, EQuitPreference::Quit, false);
}

void UBeanHUDWidget::HandleToggleFullscreenClicked()
{
	UWorld* World = GetWorld();
	UGameViewportClient* ViewportClient = World ? World->GetGameViewport() : nullptr;
	if (!ViewportClient)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Bean] No game viewport client, cannot toggle fullscreen."));
		return;
	}

	// Let the engine do it. Its own command is the only path that keeps the real viewport state
	// (FViewport::IsFullscreen), FSystemResolution, the RHI swapchain and the saved user settings
	// in step with each other, and it also re-derives a resolution that matches the monitor.
	//
	// Driving UGameUserSettings by hand instead desyncs them: the resolution request is then
	// treated as a no-op (FSystemResolution::RequestResolutionChange skips requests that match its
	// cached state), so the button appears dead, and once the window and the rendered viewport
	// disagree the present is stretched - the blurry, corrupted-looking picture after a few clicks.
	ViewportClient->HandleToggleFullscreenCommand();

	// Switching window modes drops the OS mouse capture, so the game input mode has to be
	// re-asserted or camera look and the HUD cursor stay wedged afterwards.
	if (Controller)
	{
		Controller->ReapplyCursorMode();
	}
}

void UBeanHUDWidget::HandleUndoClicked()
{
	if (Controller)
	{
		Controller->Undo();
	}
}

void UBeanHUDWidget::HandleRedoClicked()
{
	if (Controller)
	{
		Controller->Redo();
	}
}

void UBeanHUDWidget::HandleToggleSelectionClicked()
{
	if (Controller)
	{
		Controller->ToggleSelectionMode();
	}
}

void UBeanHUDWidget::HandleClearSelectionClicked()
{
	if (Controller)
	{
		Controller->ClearSelection();
	}
}

void UBeanHUDWidget::HandleSelectAllClicked()
{
	if (Controller)
	{
		Controller->SelectAll();
	}
}

void UBeanHUDWidget::HandleClearCanvasClicked()
{
	if (Controller)
	{
		if (ABeanGrid* Grid = Controller->GetGrid())
		{
			Grid->ClearAllCells();
			Controller->RefreshProgress();
			Controller->ClearSelection();
		}
	}
}

void UBeanHUDWidget::HandleSaveClicked()
{
	if (Controller)
	{
		// Asks for the name first (pre-filled with the current one), so saving a work is also how you
		// name it (m05089). Ctrl+S remains the quiet save.
		Controller->SaveWorkWithNamePrompt();
	}
}

void UBeanHUDWidget::HandleSaveWorkClicked()
{
	if (Controller)
	{
		Controller->SaveWorkAs();
	}
}

void UBeanHUDWidget::HandleExitSelectionClicked()
{
	if (Controller)
	{
		Controller->ToggleSelectionMode();
	}
}

void UBeanHUDWidget::HandleNewCanvasClicked()
{
	if (Controller)
	{
		// Deferred by the controller: the native/custom dialogs cannot run from inside a button
		// callback without the HUD re-entering itself.
		Controller->RequestNewCanvas();
	}
}

void UBeanHUDWidget::HandleGalleryClicked()
{
	if (Controller)
	{
		Controller->OpenGallery();
	}
}

void UBeanHUDWidget::HandleScreenshotClicked()
{
	if (Controller)
	{
		Controller->OpenScreenshotMode();
	}
}

void UBeanHUDWidget::HandleBatchPlaceClicked()
{
	if (Controller) Controller->ApplyBatchOperation(EBeanBatchOp::PlaceCurrentColor);
}

void UBeanHUDWidget::HandleBatchRemoveClicked()
{
	if (Controller) Controller->ApplyBatchOperation(EBeanBatchOp::Remove);
}

void UBeanHUDWidget::HandleBatchReplaceClicked()
{
	if (Controller) Controller->ApplyBatchOperation(EBeanBatchOp::ReplaceColor);
}

void UBeanHUDWidget::HandleBatchFillClicked()
{
	if (Controller) Controller->ApplyBatchOperation(EBeanBatchOp::FillFromPattern);
}

void UBeanHUDWidget::HandleImportApplyClicked()
{
	if (Controller) Controller->ApplyImage();
}

void UBeanHUDWidget::HandleImportFillClicked()
{
	if (Controller)
	{
		Controller->ApplyImageAndFill();
	}
}

void UBeanHUDWidget::HandleImportCancelClicked()
{
	if (Controller) Controller->CancelImage();
}

void UBeanHUDWidget::HandleImportDitherClicked()
{
	if (Controller) Controller->ToggleImportDither();
}

void UBeanHUDWidget::HandleImportColorsAllClicked()
{
	if (Controller) Controller->SetImportMaxColors(0);
}

void UBeanHUDWidget::HandleImportColors24Clicked()
{
	if (Controller) Controller->SetImportMaxColors(24);
}

void UBeanHUDWidget::HandleImportColors16Clicked()
{
	if (Controller) Controller->SetImportMaxColors(16);
}

void UBeanHUDWidget::HandleImportColors8Clicked()
{
	if (Controller) Controller->SetImportMaxColors(8);
}

void UBeanHUDWidget::HandleImportPresetSquareClicked()
{
	if (Controller) Controller->SetImportCanvasPreset(EBeanCanvasPreset::Square256);
}

void UBeanHUDWidget::HandleImportPresetLandscapeClicked()
{
	if (Controller) Controller->SetImportCanvasPreset(EBeanCanvasPreset::Landscape4x3);
}

void UBeanHUDWidget::HandleImportPresetPortraitClicked()
{
	if (Controller) Controller->SetImportCanvasPreset(EBeanCanvasPreset::Portrait3x4);
}

void UBeanHUDWidget::HandleImportResolutionClicked()
{
	if (Controller) Controller->SetImportResolution(0);
}

void UBeanHUDWidget::HandleGalleryCloseClicked()
{
	if (Controller) Controller->OpenGallery();
}

void UBeanHUDWidget::HandleScreenshotStyleClicked()
{
	if (Controller) Controller->SetScreenshotStyle(-1);
}

void UBeanHUDWidget::HandleScreenshotHideUIClicked()
{
	if (Controller) Controller->ToggleScreenshotUI();
}

void UBeanHUDWidget::HandleScreenshotCaptureClicked()
{
	if (Controller) Controller->CaptureScreenshot();
}

void UBeanHUDWidget::HandleScreenshotCloseClicked()
{
	if (Controller) Controller->OpenScreenshotMode();
}
