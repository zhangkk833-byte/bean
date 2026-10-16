#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Engine/TimerHandle.h"
#include "BeanPalette.h"
#include "BeanGrid.h"
#include "BeanHUDWidget.generated.h"

class ABeanPlayerController;
class UBorder;
class UButton;
class UCanvasPanel;
class UHorizontalBox;
class UImage;
class UProgressBar;
class UTextBlock;
class UTexture2D;
class UVerticalBox;
class UWidget;

/** What a gallery row does when its buttons are pressed. The HUD fills this in once and hands the
 *  same instance to every row, so adding a row action does not mean adding another six callbacks
 *  to UBeanHUDWidget. Plain (non-reflected) struct: it holds std::function-style delegates only. */
struct FBeanGalleryActions
{
	/** Open a saved work for further editing. */
	TFunction<void(const FString&)> OnOpen;
	/** Rename a saved work. */
	TFunction<void(const FString&)> OnRename;
	/** Delete a saved work (the handler asks for confirmation). */
	TFunction<void(const FString&)> OnDelete;
	/** Export a saved work as a picture. */
	TFunction<void(const FString&)> OnExport;
};

/** One entry of the bottom palette bar: a 3D-cube icon + its color label. */
UCLASS()
class BEAN_API UBeanPaletteItemWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Configure before the widget is added to the viewport tree. InFaceCenter is the
	 *  normalized centre of the cube's front face (from CreateCubeIconTexture), used to
	 *  anchor the palette number onto the middle of the face. */
	void Setup(int32 InColorIndex, const FLinearColor& InColor, const FString& InName, UTexture2D* InIcon, const FVector2D& InFaceCenter, float InItemWidth, float InItemHeight);

	TFunction<void(int32)> OnClicked;

	void SetSelected(bool bSelected);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	UFUNCTION()
	void HandleButtonClicked();

	void BuildContent();

	UPROPERTY()
	TObjectPtr<UButton> Button = nullptr;
	UPROPERTY()
	TObjectPtr<UImage> Icon = nullptr;
	UPROPERTY()
	TObjectPtr<UTextBlock> Label = nullptr;
	UPROPERTY()
	TObjectPtr<UTextBlock> NumberLabel = nullptr;
	UPROPERTY()
	TObjectPtr<UBorder> Frame = nullptr;

	int32 ColorIndex = 0;
	FLinearColor Color = FLinearColor::White;
	FString ColorName;
	FVector2D FaceCenter = FVector2D(0.5f, 0.5f);
	TObjectPtr<UTexture2D> IconTexture = nullptr;
	float ItemWidth = 40.0f;
	float ItemHeight = 66.0f;
};

/** One row of the gallery list: the work's thumbnail, name and metadata + its action buttons. */
UCLASS()
class BEAN_API UBeanGalleryItemWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Configure before the widget enters the viewport tree. */
	void Setup(const FString& InSlotName, const FString& InWorkName, UTexture2D* InThumbnail,
		const FString& InMetaLine, bool bInReadable, const FString& InProblem, float InRowHeight);

	/** Slot (save file base name) this row stands for. */
	const FString& GetSlotName() const { return SlotName; }

	/** Called with the row's slot name. The destination callbacks for the panel-level actions are
	 *  wired straight to the controller by the gallery's builder. */
	TFunction<void(const FString&)> OnExportPicture;

	/** Row actions shared with every other row (open / rename / delete / export). */
	FBeanGalleryActions Actions;

	void SetProblem(const FString& InProblem, bool bInReadable);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	UFUNCTION()
	void HandleOpenClicked();
	UFUNCTION()
	void HandleRenameClicked();
	UFUNCTION()
	void HandleDeleteClicked();
	UFUNCTION()
	void HandleExportClicked();

	void BuildContent();

	UPROPERTY()
	TObjectPtr<UButton> OpenButton = nullptr;
	UPROPERTY()
	TObjectPtr<UTextBlock> NameLabel = nullptr;
	UPROPERTY()
	TObjectPtr<UTextBlock> MetaLabel = nullptr;
	UPROPERTY()
	TObjectPtr<UTextBlock> ProblemLabel = nullptr;
	UPROPERTY()
	TObjectPtr<UImage> Thumbnail = nullptr;

	FString SlotName;
	FString WorkName;
	FString MetaLine;
	FString Problem;
	bool bReadable = true;
	TObjectPtr<UTexture2D> ThumbnailTexture = nullptr;
	float RowHeight = 84.0f;
};

/** Full-screen HUD: palette bar, hint texts, edit buttons, progress panel, import/export/gallery
 *  panels and the crosshair. Every panel is built hidden by BuildUI and shown on demand. */
UCLASS()
class BEAN_API UBeanHUDWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetController(ABeanPlayerController* InController) { Controller = InController; }
	ABeanPlayerController* GetController() const { return Controller; }

	void SetSelectedColor(int32 ColorIndex);
	void SetImportButtonsEnabled(bool bHasOverlay);
	void ToggleVisibility();

	/** Show / hide the whole HUD (E key and the screenshot display mode both need this). */
	void SetHUDVisible(bool bVisible);
	bool IsHUDVisible() const;

	/** Show a short message in the status strip under the top edge (export result, ...) and hide it
	 *  again after DurationSeconds (<= 0 keeps it up until the next message). */
	void ShowStatus(const FString& Message, float DurationSeconds = 6.0f);

	/** Enable / disable the undo and redo buttons and label them with what they would do. */
	void SetHistoryState(bool bCanUndo, bool bCanRedo, const FString& UndoLabel, const FString& RedoLabel);

	/** Refresh the completion progress panel (progress bar, percentage and the remaining counts). */
	void SetCompletionStats(const FBeanCompletionStats& Stats);

	/** Show / hide the rectangular-selection batch panel. */
	void SetSelectionPanelVisible(bool bVisible, bool bHasSelection, const FString& Summary);

	/** Highlight which batch buttons make sense with the current selection. */
	void SetSelectionSummary(const FString& Summary, bool bHasSelection);

	// -- gallery -----------------------------------------------------------
	/** Show the gallery panel and fill it with the given rows. */
	void ShowGallery();
	void HideGallery();
	/** Rebuild the gallery's work list. Rows own their thumbnails; pass an empty image for a
	 *  work whose thumbnail could not be produced. */
	void SetGalleryRows(const TArray<FString>& SlotNames, const TArray<FString>& WorkNames,
		const TArray<UTexture2D*>& Thumbnails, const TArray<FString>& MetaLines,
		const TArray<bool>& Readable, const TArray<FString>& Problems);
	/** Notice line at the bottom of the gallery (delete confirmation, damage report, ...). */
	void SetGalleryMessage(const FString& Message);

	// -- screenshot / display mode ----------------------------------------
	void ShowScreenshotMode();
	void HideScreenshotMode();
	/** Describe the current display style on the panel. */
	void SetScreenshotStatus(const FString& StyleText, const FString& Message);
	/** Hide / show the screenshot panel's own widgets (so a clean frame can be captured). */
	void SetScreenshotUIHidden(bool bHidden);

	// -- export preview ----------------------------------------------------
	/** Bring up the export preview: the picture the grid would export right now, at the given
	 *  pixels per bead, with the scale chooser and the save/back buttons. Reported size comes from
	 *  the grid's content bounds. Nothing is written to disk until the user asks for it. */
	void ShowExportPreview(UTexture2D* Texture, int32 PixelsPerCell, int32 CellsW, int32 CellsH);

	/** Take the preview panel back down (after a successful save, or when going back to the beads). */
	void HideExportPreview();

	/** Replace the notice line under the picture (cancel notice, export error, ...). An empty
	 *  message collapses the line. */
	void SetExportPreviewMessage(const FString& Message);

	// -- import preview ----------------------------------------------------
	/** Bring up the conversion preview: the original picture on the left, the converted pattern on
	 *  the right, and the canvas size / colour count / crop controls in between. Nothing is applied
	 *  to the work until the player presses 应用. */
	void ShowImportPreview(UTexture2D* SourceTexture, UTexture2D* ConvertedTexture);
	/** Take the import preview down (apply, cancel, or a failed conversion). */
	void HideImportPreview();
	/** Update the panel's readouts after a conversion (canvas size, colours used, fit info). */
	void SetImportPreviewInfo(const FString& Info, const FString& Message, bool bHasResult,
		bool bDither, int32 MaxColors, int32 Resolution, EBeanCanvasPreset Preset);
	/** Same, but for a failed conversion: no result, so 应用 must stay disabled. */
	void SetImportPreviewError(const FString& Message);
	/** True while the import preview panel is on screen. */
	bool IsImportPreviewOpen() const;

	/** Invoked by palette item clicks. */
	TFunction<void(int32)> OnColorSelected;

	/** Invoked by the batch buttons on the selection panel, which is deliberately a parameterised
	 *  list: adding a batch operation must not mean adding another six callbacks. */
	TFunction<void(EBeanBatchOp)> OnBatchOperation;

	/** Gallery row: export a saved work as a picture. */
	TFunction<void(const FString&)> OnGalleryExport;

	/** Row actions handed to every gallery row (open / rename / delete / export). */
	FBeanGalleryActions GalleryActions;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

private:
	void BuildUI();
	void BuildPaletteBar();
	/** Build the (initially hidden) export preview overlay. Called once, at the end of BuildUI so it
	 *  lands on top of the palette bar and the button stack. */
	void BuildExportPreview();
	/** Build the (initially hidden) conversion preview overlay. */
	void BuildImportPreview();
	/** Build the (initially hidden) gallery overlay. */
	void BuildGallery();
	/** Build the (initially hidden) screenshot / display mode overlay. */
	void BuildScreenshotMode();
	/** Build the completion progress panel (top-right, above the button stack). */
	void BuildProgressPanel();
	/** Build the rectangular-selection batch panel (bottom-centre, above the palette bar). */
	void BuildSelectionPanel();
	/** Re-read the current preview state into the panel's texts and scale buttons. */
	void RefreshExportPreview();
	UCanvasPanel* GetRootCanvas();
	/** Timer callback that takes the status strip back down. */
	void ClearStatus();

	/** Small helper shared by every panel: a flat dark button with centred label text. */
	UButton* MakePanelButton(const FString& LabelText, float Width, float Height, int32 FontSize);
	/** Replace the text of a button produced by MakePanelButton (state readouts reuse their buttons). */
	static void SetPanelButtonLabel(UButton* Button, const FString& LabelText);
	/** Tint a MakePanelButton button to show whether it is the active option. */
	static void SetPanelButtonHighlight(UButton* Button, bool bActive);

	UFUNCTION()
	void HandleImportClicked();
	/** 应用并铺满: apply the pending pattern and place every bead it asks for in one undo step. */
	UFUNCTION()
	void HandleApplyFillClicked();
	UFUNCTION()
	void HandleCancelClicked();
	UFUNCTION()
	void HandleOpacityIncClicked();
	UFUNCTION()
	void HandleOpacityDecClicked();
	UFUNCTION()
	void HandleToggleOverlayClicked();
	UFUNCTION()
	void HandleExportClicked();
	/** One handler per offered scale: OnClicked is a dynamic delegate, so it cannot carry the value
	 *  itself. The four values mirror ABeanGrid::GetExportScaleOptions(). */
	UFUNCTION()
	void HandleExportScale4Clicked();
	UFUNCTION()
	void HandleExportScale8Clicked();
	UFUNCTION()
	void HandleExportScale16Clicked();
	UFUNCTION()
	void HandleExportScale32Clicked();
	UFUNCTION()
	void HandleExportSaveClicked();
	UFUNCTION()
	void HandleExportBackClicked();
	UFUNCTION()
	void HandleExitClicked();
	UFUNCTION()
	void HandleToggleFullscreenClicked();
	UFUNCTION()
	void HandleUndoClicked();
	UFUNCTION()
	void HandleRedoClicked();
	UFUNCTION()
	void HandleToggleSelectionClicked();
	UFUNCTION()
	void HandleClearSelectionClicked();
	UFUNCTION()
	void HandleSelectAllClicked();
	UFUNCTION()
	void HandleClearCanvasClicked();
	UFUNCTION()
	void HandleExitSelectionClicked();
	UFUNCTION()
	void HandleSaveClicked();
	UFUNCTION()
	void HandleSaveWorkClicked();
	UFUNCTION()
	void HandleNewCanvasClicked();
	UFUNCTION()
	void HandleGalleryClicked();
	UFUNCTION()
	void HandleScreenshotClicked();
	UFUNCTION()
	void HandleBatchPlaceClicked();
	UFUNCTION()
	void HandleBatchRemoveClicked();
	UFUNCTION()
	void HandleBatchReplaceClicked();
	UFUNCTION()
	void HandleBatchFillClicked();
	UFUNCTION()
	void HandleImportApplyClicked();
	/** 确定并铺满画布: apply the pattern AND place every bead of it in one step. */
	UFUNCTION()
	void HandleImportFillClicked();
	UFUNCTION()
	void HandleImportCancelClicked();
	UFUNCTION()
	void HandleImportDitherClicked();
	UFUNCTION()
	void HandleImportColorsAllClicked();
	UFUNCTION()
	void HandleImportColors24Clicked();
	UFUNCTION()
	void HandleImportColors16Clicked();
	UFUNCTION()
	void HandleImportColors8Clicked();
	UFUNCTION()
	void HandleImportPresetSquareClicked();
	UFUNCTION()
	void HandleImportPresetLandscapeClicked();
	UFUNCTION()
	void HandleImportPresetPortraitClicked();
	UFUNCTION()
	void HandleImportResolutionClicked();
	UFUNCTION()
	void HandleGalleryCloseClicked();
	UFUNCTION()
	void HandleScreenshotStyleClicked();
	UFUNCTION()
	void HandleScreenshotHideUIClicked();
	UFUNCTION()
	void HandleScreenshotCaptureClicked();
	UFUNCTION()
	void HandleScreenshotCloseClicked();

	UPROPERTY()
	TObjectPtr<UCanvasPanel> RootCanvas = nullptr;
	UPROPERTY()
	TObjectPtr<UVerticalBox> PaletteBar = nullptr;
	UPROPERTY()
	TObjectPtr<UVerticalBox> ButtonStack = nullptr;
	UPROPERTY()
	TObjectPtr<UImage> Crosshair = nullptr;
	/** 应用并铺满: the single apply button (m04494) - registers the pattern and places it at once. */
	UPROPERTY()
	TObjectPtr<UButton> ApplyFillButton = nullptr;
	UPROPERTY()
	TObjectPtr<UButton> CancelButton = nullptr;
	UPROPERTY()
	TObjectPtr<UButton> ToggleOverlayButton = nullptr;
	UPROPERTY()
	TObjectPtr<UButton> UndoButton = nullptr;
	UPROPERTY()
	TObjectPtr<UButton> RedoButton = nullptr;
	UPROPERTY()
	TObjectPtr<UButton> SelectionButton = nullptr;
	UPROPERTY()
	TObjectPtr<UBorder> StatusPanel = nullptr;
	UPROPERTY()
	TObjectPtr<UTextBlock> StatusLabel = nullptr;
	/** Hides the status strip again once its message has been up long enough. */
	FTimerHandle StatusTimerHandle;
	UPROPERTY()
	TArray<TObjectPtr<UBeanPaletteItemWidget>> PaletteItems;

	// -- completion progress panel (top-right) ------------------------------
	UPROPERTY()
	TObjectPtr<UBorder> ProgressPanel = nullptr;
	UPROPERTY()
	TObjectPtr<UTextBlock> ProgressTitle = nullptr;
	UPROPERTY()
	TObjectPtr<UProgressBar> ProgressBar = nullptr;
	UPROPERTY()
	TObjectPtr<UTextBlock> ProgressPercent = nullptr;
	UPROPERTY()
	TObjectPtr<UTextBlock> ProgressCounts = nullptr;

	// -- selection / batch panel (bottom-centre) ----------------------------
	UPROPERTY()
	TObjectPtr<UBorder> SelectionPanel = nullptr;
	UPROPERTY()
	TObjectPtr<UTextBlock> SelectionLabel = nullptr;
	UPROPERTY()
	TArray<TObjectPtr<UButton>> BatchButtons;

	// -- import preview panel ------------------------------------------------
	UPROPERTY()
	TObjectPtr<UBorder> ImportPreviewPanel = nullptr;
	UPROPERTY()
	UImage* ImportSourceImage = nullptr;
	UPROPERTY()
	UImage* ImportConvertedImage = nullptr;
	UPROPERTY()
	TObjectPtr<UTextBlock> ImportInfoLabel = nullptr;
	UPROPERTY()
	TObjectPtr<UTextBlock> ImportMessageLabel = nullptr;
	UPROPERTY()
	TObjectPtr<UButton> ImportApplyButton = nullptr;
	UPROPERTY()
	TObjectPtr<UButton> ImportDitherButton = nullptr;
	UPROPERTY()
	TArray<TObjectPtr<UButton>> ImportColorButtons;
	UPROPERTY()
	TArray<TObjectPtr<UButton>> ImportPresetButtons;
	UPROPERTY()
	TArray<TObjectPtr<UButton>> ImportResolutionButtons;
	/** Current settings mirrored from the controller, so the buttons can show their state. */
	bool bImportDitherShown = false;
	int32 ImportMaxColorsShown = 0;
	int32 ImportResolutionShown = 256;
	EBeanCanvasPreset ImportPresetShown = EBeanCanvasPreset::Square256;

	// -- export preview overlay ---------------------------------------------
	UPROPERTY()
	TObjectPtr<UBorder> ExportPreviewPanel = nullptr;
	UPROPERTY()
	TObjectPtr<UTextBlock> ExportPreviewTitle = nullptr;
	UPROPERTY()
	TObjectPtr<UImage> ExportPreviewImage = nullptr;
	UPROPERTY()
	TObjectPtr<UTextBlock> ExportPreviewInfo = nullptr;
	UPROPERTY()
	TObjectPtr<UTextBlock> ExportPreviewMessage = nullptr;
	UPROPERTY()
	TArray<TObjectPtr<UButton>> ExportScaleButtons;
	/** What the panel currently shows; drives the scale readout and the highlighted scale button. */
	int32 ExportPreviewPixelsPerCell = 0;
	int32 ExportPreviewCellsW = 0;
	int32 ExportPreviewCellsH = 0;

	// -- gallery overlay -----------------------------------------------------
	UPROPERTY()
	TObjectPtr<UBorder> GalleryPanel = nullptr;
	UPROPERTY()
	TObjectPtr<UVerticalBox> GalleryList = nullptr;
	UPROPERTY()
	TObjectPtr<UTextBlock> GalleryMessage = nullptr;
	UPROPERTY()
	TObjectPtr<UTextBlock> GalleryEmptyLabel = nullptr;
	UPROPERTY()
	TArray<TObjectPtr<UBeanGalleryItemWidget>> GalleryRows;

	// -- screenshot / display mode overlay -----------------------------------
	UPROPERTY()
	TObjectPtr<UBorder> ScreenshotPanel = nullptr;
	UPROPERTY()
	TObjectPtr<UTextBlock> ScreenshotStatus = nullptr;
	UPROPERTY()
	TObjectPtr<UTextBlock> ScreenshotMessage = nullptr;

	TObjectPtr<ABeanPlayerController> Controller = nullptr;
	bool bBuiltPaletteBar = false;
};
