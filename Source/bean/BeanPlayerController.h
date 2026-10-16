#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "BeanGrid.h"
#include "BeanImageConverter.h"
#include "BeanPlayerController.generated.h"

class ABeanCharacter;
class UBeanHUDWidget;
class UInputAction;
class UInputMappingContext;
class UTexture2D;
struct FImage;
struct FInputActionValue;

/**
 * Player input, placement, image import/export and the edit commands the HUD calls into.
 *
 * The controller owns no work data at all: every mutation goes through ABeanGrid's single edit
 * entry point (ApplyCellEdits / ApplyBatch), so undo, redo, completion statistics, rendering and
 * the save-dirty flag can never disagree. What lives here is interaction state: the ray target,
 * the in-progress stroke, the rectangular selection drag, the import session and the export
 * preview.
 */
UCLASS()
class BEAN_API ABeanPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	ABeanPlayerController();

	void SetGrid(ABeanGrid* InGrid);

	/** The grid this controller drives (may be null for the first frames). */
	ABeanGrid* GetGrid() const { return Grid; }

	// -- called by the HUD -----------------------------------------------
	void ImportImage();
	void ApplyImage();
	/** ApplyImage, then immediately place every bead the converted pattern asks for. One undo step. */
	void ApplyImageAndFill();
	void CancelImage();
	void ChangeOverlayOpacity(float Delta);
	void ToggleOverlayVisible();
	/** Show the export preview: renders the bead work at a sensible scale, dims the game behind it
	 *  and offers 保存图片 / 返回继续拼豆. Nothing is written until the user saves. */
	void ExportImage();
	/** Export preview: switch to another scale (pixels per bead) and re-render the picture. */
	void SetExportPreviewScale(int32 PixelsPerCell);
	/** Export preview: ask for a destination, then write the PNG at the previewed scale. Stays in
	 *  the preview if the user cancels or the write fails. */
	void SaveExportImage();
	/** Leave the export preview without saving, and give the game its input back. */
	void CloseExportPreview();
	void CycleColor(int32 Delta);
	void SetCurrentColor(int32 Index);
	/** Re-assert the current mouse-cursor/input mode. The 全屏 button and the import picker both
	 *  hand the OS focus around, which drops the mouse capture and otherwise leaves camera look
	 *  and the HUD cursor wedged. */
	void ReapplyCursorMode() { ApplyCursorMode(); }

	// -- undo / redo -------------------------------------------------------
	/** Ctrl+Z / the 撤销 button. */
	void Undo();
	/** Ctrl+Y / the 重做 button. */
	void Redo();

	// -- rectangular selection + batch operations --------------------------
	/** Enter / leave the rectangular-selection tool. While it is on, the left button drags a
	 *  selection rectangle instead of placing beads. */
	void ToggleSelectionMode();
	bool IsSelectionMode() const { return bSelectionMode; }
	/** Run one batch operation over the current selection and refresh the HUD. */
	void ApplyBatchOperation(EBeanBatchOp Op);
	/** Drop the current selection ( Esc / the 取消选区 button ). */
	void ClearSelection();
	/** Select the whole canvas (the 全选 button), so a batch can cover everything. */
	void SelectAll();

	// -- completion statistics ---------------------------------------------
	/** Push the current completion report into the HUD progress panel. */
	void RefreshProgress();

	// -- import session (settings -> preview -> apply) ----------------------
	/** Re-run the conversion with the current canvas / colour-count settings and push the
	 *  result into the import preview panel. Cheap enough to call on every settings change. */
	void RefreshImportPreview();
	/** Import panel: how many palette colours the conversion may use (0 = all 48). */
	void SetImportMaxColors(int32 MaxColors);
	/** Import panel: toggle order-preserving error diffusion. */
	void ToggleImportDither();
	/** Import panel: pick the canvas shape, keeping the chosen resolution. */
	void SetImportCanvasPreset(EBeanCanvasPreset Preset);
	/** Import panel: pick the canvas resolution (long edge in cells). */
	void SetImportResolution(int32 Resolution);

	// -- work management ---------------------------------------------------
	/** Ask for a canvas size and start a brand-new empty work ("新建画布"). */
	void RequestNewCanvas();
	/** Save the current work into the slot it already lives in, with no dialog (Ctrl+S / F5). */
	void SaveWork();
	/** Save under a name the player types, pre-filled with the current name ("保存作品"). */
	void SaveWorkWithNamePrompt();
	/** Save as a copy under a name the player types, pre-filled with "<name> 副本" ("另存作品"). */
	void SaveWorkAs();
	/** Open the local work gallery. */
	void OpenGallery();
	/** Open the screenshot / display mode. */
	void OpenScreenshotMode();
	/** Screenshot mode: toggle the hint layer, background and picture style. */
	void SetScreenshotStyle(int32 StyleIndex);
	/** Screenshot mode: hide / show the display-mode UI so a clean picture can be taken. */
	void ToggleScreenshotUI();
	/** Screenshot mode: write the current view (or the clean bead picture) to disk. */
	void CaptureScreenshot();

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupInputComponent() override;

private:
	/**
	 * Undo / redo key handler.
	 *
	 * These are bound through the legacy action-mapping route rather than Enhanced Input: a
	 * UInputAction mapping is matched against the key alone and cannot require a modifier, so
	 * binding Z there would also fire on a bare Z. The legacy handler is declared with no
	 * parameters, so the live Ctrl state is read back from the keyboard to tell Ctrl+Z from Z.
	 */
	void HandleUndoKey();
	void HandleRedoKey();
	/** Ctrl+S: the same save as the "保存" button, without the F5 key some keyboards lack. */
	void HandleSaveKey();

	/** Push undo/redo button state, completion statistics and the import button state into the HUD. */
	void RefreshHUDState();
	/** Push the current selection summary into the HUD's selection panel. */
	void RefreshSelectionPanel();
	/** Let the grid autosave after an edit that an undo/redo/batch performed, and report it. */
	void MarkDirtyAndAutoSave();
	/** Called by the grid after any edit: refresh everything that depends on work data. */
	void HandleGridChanged();

	void BindEnhancedInput();
	void AddMappingContext();
	void ApplyCursorMode();
	void UpdateTarget();
	void EnsureGrid();
	/**
	 * True when the mouse cursor is over a clickable HUD control (a button, a scroll bar, ...).
	 *
	 * Used by pointer mode to tell "clicked on empty ground" (take the pointer away again and go
	 * back to aiming) from "clicked on the interface" (leave the pointer exactly where it is).
	 * Asking Slate for the widget path is the reliable way to answer this: the HUD's own layout is
	 * built at runtime, so looking for a panel rectangle here would rot the moment it is re-laid out.
	 */
	bool IsPointerOverHUDControl() const;
	/** Deferred import step: run the native file picker, then load the chosen image. */
	void OpenImportPicker();
	/** Export preview step: (re-)render the bead work at Scale and push it into the HUD preview.
	 *  Returns false (and reports why on the HUD) when there is nothing to draw. */
	bool RefreshExportPreview(int32 Scale);
	/** The scale the preview opens with: the largest offered option whose picture fits in
	 *  ChooseDefaultExportScale's comfort bound, or the smallest option when even that is too big. */
	int32 ChooseDefaultExportScale(int32 CellsW, int32 CellsH) const;
	/** Deferred export step: run the native save dialog, then write the PNG. */
	void OpenExportSaveDialog();
	/** Deferred step: ask for a work name, then save under it. */
	void OpenSaveWorkDialog();
	/** Ask for a canvas size and start a brand-new empty work on it ("新建画布"). */
	void NewCanvasFromMenu();
	/** Deferred step: run the native save dialog for a screenshot and write the file. */
	void OpenScreenshotSaveDialog();
	/** Freeze / release walking, looking and placing while a modal panel owns the screen. */
	void SetPreviewInputLock(bool bLocked);
	/** Shared modal "pick one of N" window used by the resolution / canvas-shape prompts: a
	 *  two-wide grid of enlarged buttons (labels may contain newlines) plus a cancel row, laid
	 *  out in Slate units scaled to the window's DPI so it reads the same in PIE (720 units tall)
	 *  and in a full-height packaged window. Disabled labels are shown greyed out. Returns the
	 *  chosen value, or 0 when the user cancels or closes the window. */
	int32 ShowOptionDialog(const FString& Title, const TArray<FString>& Labels, const TArray<int32>& Values, const TArray<bool>& bEnabled);
	/** Native single-line text prompt (work name). Returns false when the user cancels. */
	bool ShowTextPrompt(const FString& Title, const FString& DefaultText, FString& OutText);

	/** Release the loaded import session (source picture + conversion state). */
	void CloseImportSession();
	/** Apply the pending conversion result into the grid and close the import session. */
	void CommitImport();
	/** Store the pending conversion result as the grid's target pattern without placing beads. */
	void CommitPattern();

	/** Apply the current canvas preset + resolution to ImportOptions. */
	void ApplyCanvasSize();

	/** Write the work's gallery thumbnail next to its save file (best effort, never fatal). */
	void WriteWorkThumbnail(const FString& SlotName, const FString& WorkName);
	/** Rebuild the gallery's rows from the save directory and hand them to the HUD. */
	void RefreshGalleryRows();
	/** Gallery row actions. */
	void LoadWorkFromGallery(const FString& SlotName);
	void RenameWorkFromGallery(const FString& SlotName);
	void DeleteWorkFromGallery(const FString& SlotName);
	/** Export a stored work as a PNG without disturbing the work the player is editing. */
	void ExportWorkFromGallery(const FString& SlotName);
	/** Keep the screenshot panel's status line in step with the display style. */
	void RefreshScreenshotStatus();

	/** True while some modal panel (import preview / export preview / gallery / screenshot) owns
	 *  the screen: the game ignores place/remove and walking input. */
	bool IsAnyPanelOpen() const;

	// -- input handlers ---------------------------------------------------
	void OnMoveForward(const FInputActionValue& Value);
	void OnMoveRight(const FInputActionValue& Value);
	void OnLook(const FInputActionValue& Value);
	void OnPlace(const FInputActionValue& Value);
	void OnPlaceHeld(const FInputActionValue& Value);
	void OnPlaceReleased(const FInputActionValue& Value);
	void OnRemove(const FInputActionValue& Value);
	void OnRemoveHeld(const FInputActionValue& Value);
	void OnRemoveReleased(const FInputActionValue& Value);
	void OnCycleColor(const FInputActionValue& Value);
	void OnToggleFly(const FInputActionValue& Value);
	void OnFlyUp(const FInputActionValue& Value);
	void OnFlyDown(const FInputActionValue& Value);
	void OnToggleCursor(const FInputActionValue& Value);
	void OnToggleUI(const FInputActionValue& Value);
	void OnSave(const FInputActionValue& Value);
	/** Esc: cancel the current selection / panel state without touching the work. */
	void OnEscape(const FInputActionValue& Value);

	/** While the left button is held, place on every cell swept from the last acted cell to (ToX, ToY). */
	void SweepPlace(int32 ToX, int32 ToY);
	/** While the right button is held, erase every cell swept from the last erased one (never for a click). */
	void SweepRemove(int32 ToX, int32 ToY);

	/** Start / finish one drag of the selection rectangle. */
	void BeginSelectionDrag();
	void UpdateSelectionDrag();
	void EndSelectionDrag();

	/** Begin a continuous placement stroke on the grid (one undo step per press). */
	void BeginEditStroke(const FString& Label);
	/** Finish the stroke started by BeginEditStroke. Safe to call when none is open. */
	void EndEditStroke();

	UPROPERTY()
	TObjectPtr<UInputMappingContext> MappingContext = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_MoveForward = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_MoveRight = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_Look = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_Place = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_Remove = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_CycleColor = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_ToggleFly = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_FlyUp = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_FlyDown = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_ToggleCursor = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_ToggleUI = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_Save = nullptr;
	UPROPERTY()
	TObjectPtr<UInputAction> IA_Escape = nullptr;

	UPROPERTY()
	TObjectPtr<ABeanGrid> Grid = nullptr;
	UPROPERTY()
	TObjectPtr<UBeanHUDWidget> HUDWidget = nullptr;
	UPROPERTY()
	TObjectPtr<ABeanCharacter> PawnChar = nullptr;

	int32 CurrentColor = 1;
	int32 TargetX = 0;
	int32 TargetY = 0;
	bool bTargetValid = false;
	bool bFlyUpHeld = false;
	bool bFlyDownHeld = false;

	// -- held place/remove state (continuous sweep) -------------------------
	/** Last grid cell placed on / erased while the button is held (-1 = none). */
	int32 LastPlaceCellX = -1;
	int32 LastPlaceCellY = -1;
	int32 LastRemoveCellX = -1;
	int32 LastRemoveCellY = -1;
	/** World seconds when the right button went down: a hold only erases past HoldToEraseDelay, so a
	 *  plain click can never wipe a string of beads (m05220/m05308). */
	double RemovePressTime = 0.0;
	/** True when the current press started in pointer (cursor) mode and was consumed by
	 *  exiting it — the rest of that press must not place/remove. */
	bool bPlacePressConsumed = false;
	bool bRemovePressConsumed = false;
	/** True while a place/remove stroke is open on the grid (so EndStroke is only called once). */
	bool bStrokeOpen = false;

	// -- rectangular selection tool ----------------------------------------
	/** True while the selection tool owns the left button. */
	bool bSelectionMode = false;
	/** True between press and release of a selection drag. */
	bool bSelectionDragging = false;
	/** Cell the selection drag started on. */
	int32 SelectionAnchorX = 0;
	int32 SelectionAnchorY = 0;

	// -- deferred import flow ----------------------------------------------
	/** True while the deferred import flow is pending or its file picker is open, so a second
	 *  click on 导入图片 cannot queue a second dialog. */
	bool bImportPickerOpen = false;

	// -- import session -----------------------------------------------------
	/** True while the import preview panel is up (source loaded, nothing committed yet). */
	bool bImportPanelOpen = false;
	/** The picture the player picked, exactly as loaded (BGRA8 sRGB). Kept so a crop or colour
	 *  count change can be reconverted without going back to disk. */
	FImage ImportSource;
	/** Current conversion settings (canvas / colour count / dither). */
	FBeanConvertOptions ImportOptions;
	/** Source picture has any transparency, so the panel can say why the pattern has holes. */
	bool bImportSourceTransparent = false;
	/** Last conversion result (the pattern the 应用 button would commit). */
	FBeanConvertResult ImportResult;
	/** Canvas resolution (long edge, cells) chosen in the import panel. */
	int32 ImportResolution = 256;
	/** Canvas shape chosen in the import panel; drives the aspect the picture is fitted to. */
	EBeanCanvasPreset ImportPresetShown = EBeanCanvasPreset::Square256;
	/** Texture wrapping ImportResult.Preview for the panel. Kept referenced while shown. */
	UPROPERTY()
	TObjectPtr<UTexture2D> ImportPreviewTexture = nullptr;
	/** Texture wrapping the downscaled original picture, for the panel's left-hand side. */
	UPROPERTY()
	TObjectPtr<UTexture2D> ImportSourceTexture = nullptr;

	// -- deferred export flow ----------------------------------------------
	/** True while the native save dialog is open (or queued), so a second click on 保存图片 cannot
	 *  queue a second dialog. */
	bool bExportDialogOpen = false;

	// -- export preview -----------------------------------------------------
	/** True while the export preview owns the screen: the game ignores place/remove clicks and
	 *  walking/looking input, and the HUD shows the preview instead of the plain game. */
	bool bExportPreviewOpen = false;
	/** Scale currently previewed (pixels per bead); this is what 保存图片 writes. */
	int32 ExportPreviewScale = 8;
	/** Cursor visibility as it was before the preview took over, restored when it closes. */
	bool bShowCursorBeforeExport = false;
	/** Cursor state to restore when the import preview closes (it also borrows the mouse). */
	bool bShowCursorBeforeImport = false;
	/** The picture on screen in the preview, kept referenced so it is not collected while shown. */
	UPROPERTY()
	TObjectPtr<UTexture2D> ExportPreviewTexture = nullptr;

	// -- work naming / dialogs ---------------------------------------------
	/** True while the save name prompt is up or queued. */
	bool bSaveWorkDialogOpen = false;
	/** True when that prompt came from 另存作品: it pre-fills "<name> 副本" instead of the current name. */
	bool bSaveWorkDialogSaveCopy = false;
	/** True while a modal option dialog (resolution, canvas shape, ...) is up. */
	bool bOptionDialogOpen = false;

	// -- gallery / screenshot ----------------------------------------------
	/** True while the gallery panel owns the screen. */
	bool bGalleryOpen = false;
	/** True while the screenshot / display mode owns the screen. */
	bool bScreenshotModeOpen = false;
	/** Screenshot display style: 0 = clean bead picture, 1 = picture with the hint layer,
	 *  2 = the game scene as it is. Cycled by SetScreenshotStyle. */
	int32 ScreenshotStyle = 0;
	/** True while the screenshot mode hides its own UI so a clean frame can be captured. */
	bool bScreenshotUIHidden = false;
	/** True while the deferred screenshot save dialog is queued / open. */
	bool bScreenshotDialogOpen = false;
	/** Cursor visibility before the gallery / screenshot mode took over. */
	bool bShowCursorBeforeGallery = false;

	// ~1000 eDPI at 800 DPI (mouse sensitivity tuned for faster look).
	float LookSensitivity = 1.25f;
};
