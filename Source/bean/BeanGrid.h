#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameFramework/SaveGame.h"
#include "BeanPalette.h"
#include "BeanGrid.generated.h"

class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;
class UTexture2D;
struct FImage;

/**
 * Canvas shape chosen at import time. The grid is no longer forced to be square: a landscape
 * canvas is wider than it is tall, a portrait one taller than it is wide. Resolution is still
 * counted in cells on the long edge (32/64/128/256), so every existing option keeps working.
 */
UENUM(BlueprintType)
enum class EBeanCanvasMode : uint8
{
	Square,    // Width == Height
	Landscape, // Width > Height  (e.g. 256 x 192)
	Portrait   // Height > Width  (e.g. 192 x 256)
};

/**
 * The three shapes offered by the import flow, in the order the UI lists them.
 * Square keeps the historical 256x256 canvas; landscape/portrait use a 4:3 box whose long edge
 * is still the full 256 cells, so the cell size never changes and old works stay 1:1 compatible.
 */
UENUM(BlueprintType)
enum class EBeanCanvasPreset : uint8
{
	Square256,    // 256 x 256
	Landscape4x3, // 256 x 192
	Portrait3x4   // 192 x 256
};

/** One palette color's part of the completion report. */
USTRUCT(BlueprintType)
struct FBeanColorProgress
{
	GENERATED_BODY()

	/** Palette index this row describes. */
	UPROPERTY(BlueprintReadOnly, Category = "Bean")
	int32 ColorIndex = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Bean")
	FString ColorName;

	/** Linear-space color, for drawing the progress bar / legend swatches. */
	UPROPERTY(BlueprintReadOnly, Category = "Bean")
	FLinearColor Color = FLinearColor::Black;

	/** Cells of this color the target pattern asks for. */
	UPROPERTY(BlueprintReadOnly, Category = "Bean")
	int32 Required = 0;

	/** Cells of this color that already hold exactly that color. */
	UPROPERTY(BlueprintReadOnly, Category = "Bean")
	int32 Correct = 0;

	/** Cells of this color that hold a different (non-empty) color: "wrong color". */
	UPROPERTY(BlueprintReadOnly, Category = "Bean")
	int32 Miscolored = 0;

	/** Required - Correct: how many beads of this color are still missing. */
	UPROPERTY(BlueprintReadOnly, Category = "Bean")
	int32 Remaining = 0;
};

/**
 * Completion report for the current work, computed from the real cell data (never from a plain
 * "placed / total" ratio).
 *
 *   CompletionPercent = CorrectCells / TargetCells * 100
 *
 * Only cells that the target pattern actually asks for (TargetMask true) take part, so the
 * transparent/blank background of an imported image never drags the percentage down.
 */
USTRUCT(BlueprintType)
struct FBeanCompletionStats
{
	GENERATED_BODY()

	/** Cells the target pattern asks to fill. 0 when no pattern is loaded. */
	UPROPERTY(BlueprintReadOnly, Category = "Bean")
	int32 TargetCells = 0;

	/** Cells holding the correct color at the correct position. */
	UPROPERTY(BlueprintReadOnly, Category = "Bean")
	int32 CorrectCells = 0;

	/** Target cells still empty. */
	UPROPERTY(BlueprintReadOnly, Category = "Bean")
	int32 MissingCells = 0;

	/** Target cells filled with the wrong color. */
	UPROPERTY(BlueprintReadOnly, Category = "Bean")
	int32 MiscoloredCells = 0;

	/** Placed beads that sit on a cell the pattern does not ask for at all. */
	UPROPERTY(BlueprintReadOnly, Category = "Bean")
	int32 ExtraCells = 0;

	/** CorrectCells / TargetCells * 100, clamped to [0,100]. */
	UPROPERTY(BlueprintReadOnly, Category = "Bean")
	float CompletionPercent = 0.0f;

	/** True when a target pattern is loaded and the numbers above mean something. */
	UPROPERTY(BlueprintReadOnly, Category = "Bean")
	bool bHasPattern = false;

	/** One entry per palette color, in palette order. */
	UPROPERTY(BlueprintReadOnly, Category = "Bean")
	TArray<FBeanColorProgress> PerColor;
};

/** The single-cell change a batch operation applies to one cell. */
USTRUCT()
struct BEAN_API FBeanCellEdit
{
	GENERATED_BODY()

	UPROPERTY()
	int32 X = 0;

	UPROPERTY()
	int32 Y = 0;

	/** Palette index before the change (or ABeanGrid::EmptyCell). */
	UPROPERTY()
	uint8 Before = 0;

	/** Palette index after the change (or ABeanGrid::EmptyCell). */
	UPROPERTY()
	uint8 After = 0;
};

/**
 * Batch operations offered by the rectangular selection tool. All of them funnel into
 * ABeanGrid::ApplyBatch, so undo, completion statistics, rendering and the dirty flag all see
 * exactly one consistent edit.
 */
UENUM(BlueprintType)
enum class EBeanBatchOp : uint8
{
	PlaceCurrentColor, // fill the selection with the currently selected palette color
	Remove,            // clear the selection
	ReplaceColor,      // recolor: where a cell holds ReplaceFrom (or anywhere when it is EmptyCell)
	FillFromPattern    // copy the target pattern's colors into the selection
};

/**
 * One undoable edit: only the cells that actually changed, each carrying its before/after palette
 * index. A single bead and a 256x256 batch are the same record shape, so a batch is undone in one
 * step. Records are grouped into a "stroke" (one mouse press, one batch button press) so a swept
 * drag undoes as a single action.
 */
USTRUCT()
struct BEAN_API FBeanEditRecord
{
	GENERATED_BODY()

	/** Cells that changed, in the order they were touched. */
	UPROPERTY()
	TArray<FBeanCellEdit> Edits;

	/** Group id: all edits applied during one stroke share this value (0 = first stroke). */
	UPROPERTY()
	int32 GroupId = 0;

	/** Sentinel: no group captured yet (a fresh stroke always starts a new group). */
	static constexpr int32 NoGroup = -1;

	/** True while this record still accepts more edits from the same stroke. */
	bool bOpen = false;

	/** Diagnostics only (shown in the status line): what the player did. */
	FString Label;

	/** Approximate memory this record keeps alive, in bytes. */
	int64 ByteSize() const
	{
		// TArray slack plus the element storage: good enough for the history budget.
		return sizeof(FBeanEditRecord) + static_cast<int64>(Edits.GetAllocatedSize());
	}
};

/**
 * Save file for one work.
 *
 * Format history
 * --------------
 *  vLegacy (== 0): the original layout, which only ever wrote `Cells` (exactly 256*256 bytes) and
 *                  `CurrentColor`. It carries no version field, so a file whose Version is 0 is
 *                  treated as legacy and loaded with the historical 256x256 interpretation.
 *  v2      ( == 2): adds Width/Height, the target pattern (so completion can be recomputed after a
 *                  reload), the canvas mode, the palette names and a save timestamp. Loading still
 *                  checks the cell count against Width*Height so a truncated/corrupt file is
 *                  rejected instead of misread.
 */
UCLASS()
class UBeanGridSaveData : public USaveGame
{
	GENERATED_BODY()

public:
	/** Current on-disk format version written by this build. */
	static constexpr int32 CurrentVersion = 2;

	/** 0 = the pre-versioning layout. */
	UPROPERTY()
	int32 Version = 0;

	/** Canvas size in cells. A legacy file reports 0 here and is read as 256x256. */
	UPROPERTY()
	int32 Width = 0;

	UPROPERTY()
	int32 Height = 0;

	/** Width*Height bytes; value 255 = empty, otherwise a palette index. */
	UPROPERTY()
	TArray<uint8> Cells;

	/** Target pattern: Width*Height bytes, 255 = this cell is not part of the work. */
	UPROPERTY()
	TArray<uint8> TargetCells;

	/** Target pattern mask: true for every cell the pattern asks to fill. */
	UPROPERTY()
	TArray<bool> TargetMask;

	/** Pattern rectangle inside the canvas (the imported image's box). */
	UPROPERTY()
	int32 PatternWidth = 0;

	UPROPERTY()
	int32 PatternHeight = 0;

	UPROPERTY()
	int32 PatternOriginX = 0;

	UPROPERTY()
	int32 PatternOriginY = 0;

	UPROPERTY()
	uint8 CurrentColor = 1;

	UPROPERTY()
	uint8 CanvasMode = 0;

	/** Human-readable work name, shown by the gallery. */
	UPROPERTY()
	FString WorkName;

	/** Palette color names, so a reload can detect that the palette changed under a save. */
	UPROPERTY()
	TArray<FString> PaletteNames;

	/** UTC time the file was written (FDateTime::UtcNow().GetTicks()). */
	UPROPERTY()
	int64 SavedUtcTicks = 0;

	/** True when the load failed validation and the caller silently fell back to an empty canvas. */
	UPROPERTY()
	bool bWasRecoveredFromBackup = false;
};

/**
 * The world: a Width x Height ground grid (default 256x256), up to 48 instanced cube colors,
 * the image preview overlay, the placement ghost cube, undo/redo, completion statistics and
 * save/load.
 *
 * This actor is the single data-entry point for the whole work: every mutation goes through
 * ApplyCellEdits / ApplyBatch, so undo, redo, completion, rendering and the save-dirty flag can
 * never disagree with each other.
 */
UCLASS()
class BEAN_API ABeanGrid : public AActor
{
	GENERATED_BODY()

public:
	ABeanGrid();

	/** Default canvas size, still used for the legacy save layout and the ground plane default. */
	static constexpr int32 GridSize = 256;
	/** Cell size in UE units (cm): 0.25m per cell. The default grid spans 64m x 64m. */
	static constexpr float CellSize = 25.0f;
	static constexpr uint8 EmptyCell = 255;

	/** Longest edge of the runtime-generated grid texture (must match FBeanAssets::CreateGridTexture). */
	static constexpr int32 GridTextureSize = 4096;

	/** Cell counts a canvas dimension may take. Kept from the original resolution system. */
	static const TArray<int32>& GetResolutionOptions();
	/** Canvas shapes offered at import, in UI order. */
	static const TArray<EBeanCanvasPreset>& GetCanvasPresets();
	static const TCHAR* GetCanvasPresetLabel(EBeanCanvasPreset Preset);
	/** Cell counts of one preset (long edge = Resolution). */
	static void GetPresetDimensions(EBeanCanvasPreset Preset, int32 Resolution, int32& OutWidth, int32& OutHeight);
	static EBeanCanvasMode GetPresetMode(EBeanCanvasPreset Preset);

	UPROPERTY(BlueprintReadOnly, Category = "Bean")
	TArray<FBeanPaletteColor> Palette;

	UPROPERTY(BlueprintReadOnly, Category = "Bean")
	int32 CurrentColorIndex = 1;

	// -- grid shape -----------------------------------------------------------
	int32 GetWidth() const { return Width; }
	int32 GetHeight() const { return Height; }
	bool IsReady() const { return bInitialized && Cells.Num() == Width * Height; }

	/** Replace the whole canvas (used by the import flow when the player changes its shape).
	 *  Clears the work, the pattern and the history; returns false if the size is not usable. */
	bool ResizeGrid(int32 NewWidth, int32 NewHeight);
	int32 GetCellCount() const { return Width * Height; }

	/** Cell count the ground material's grid texture was built for. The texture has to match the
	 *  canvas, otherwise the drawn cell lines and the beads disagree in size (m03538). 0 means no
	 *  texture has been assigned yet. */
	void GetGroundTextureCellCount(int32& OutCellsX, int32& OutCellsY) const
	{
		OutCellsX = GroundTextureCellsX;
		OutCellsY = GroundTextureCellsY;
	}

	// -- cell access ---------------------------------------------------------
	uint8 GetCell(int32 X, int32 Y) const;
	bool IsCellOccupied(int32 X, int32 Y) const;
	bool IsValidCell(int32 X, int32 Y) const;

	/** Place the current color; ignored when the cell is already occupied. */
	void PlaceCell(int32 X, int32 Y);
	void RemoveCell(int32 X, int32 Y);
	void ClearAllCells();

	void SetCurrentColor(int32 Index);
	void CycleColor(int32 Delta);

	FVector CellToWorld(int32 X, int32 Y) const;
	bool WorldToCell(const FVector& WorldPos, int32& X, int32& Y) const;

	int32 NearestColorIndex(const FLinearColor& Color) const;

	// -- single edit entry point (undo/redo aware) ----------------------------
	/** Continuous-placement grouping: every placement between BeginStroke and EndStroke shares one
	 *  undo step. Malformed strokes (End without Begin) are handled without losing history. */
	void BeginStroke(const FString& Label);
	void EndStroke();

	/**
	 * Closes the active stroke so the next edit starts a brand new undo record.
	 *
	 * Needed by the bulk operations: a batch must never be appended to a stroke that is still
	 * open. Merging a 65536-cell batch into a live stroke would grow that one record without
	 * bound (each batch appends its whole delta to the same record), which both defeats the
	 * history budget - every record is capped individually, not the total - and made a repeated
	 * full-canvas batch exhaust memory. Safe to call when no stroke is open.
	 */
	void EndActiveStroke();

	/** The one and only mutation path. Skips no-op edits, records a delta for undo, merges the
	 *  change into the open stroke when asked, and refreshes only the affected colors. */
	void ApplyCellEdits(TArray<FBeanCellEdit>&& Edits, const FString& Label, bool bMergeWithPrevious = false);
	void ApplyCellEdits(const TArray<FBeanCellEdit>& Edits, const FString& Label, bool bMergeWithPrevious = false);

	// -- undo / redo ----------------------------------------------------------
	/** Memory budget for the undo history, in bytes. */
	static constexpr int64 HistoryMemoryLimitBytes = 48ll * 1024 * 1024;
	/** Oldest records are dropped until the history fits under this after a single huge edit. */
	static constexpr int64 HistoryMemorySoftLimitBytes = 64ll * 1024 * 1024;

	void Undo();
	void Redo();
	bool CanUndo() const;
	bool CanRedo() const;
	int32 GetUndoCount() const { return UndoStack.Num(); }
	int32 GetRedoCount() const { return RedoStack.Num(); }
	/**
	 * Instanced-bead bookkeeping, exposed so the self-test can prove the cell<->instance maps stay
	 * consistent with the components after placements, removals, clears and undos.
	 */
	int32 GetInstanceCountForColor(int32 ColorIndex) const;
	int32 GetMappedCellCountForColor(int32 ColorIndex) const;

	/**
	 * Verify that the incremental cell<->instance maps still describe the real components.
	 *
	 * Counts alone are not enough: an index drift (every map entry above a removed instance
	 * pointing one slot too low) keeps the counts equal while the next edit moves the WRONG bead.
	 * This walks both maps and checks each mapped instance is actually sitting on its cell, and
	 * that every instance index is claimed exactly once. Returns false and fills OutError on the
	 * first disagreement. Used by the self-test; cheap enough to call there at any size.
	 */
	bool ValidateInstanceMappings(FString& OutError) const;

	/** True when the component is set up for swap-removal (see ABeanGrid::CreateInstancers). */
	bool SupportsInstanceRemoveSwap() const;

	/**
	 * Where the time inside a single apply goes, in microseconds.
	 *
	 * A "single bead on a crowded canvas" edit can only be slow for one of a few reasons, and this
	 * splits them so a regression names itself instead of needing another investigation: the data
	 * edit loop, the undo-record bookkeeping (including the history accounting), the instancer
	 * update, and the delegate broadcast. Counters are cumulative; the self-test resets them.
	 */
	struct FBeanEditTiming
	{
		double EditLoopUs = 0.0;
		double UndoUs = 0.0;
		double InstanceUs = 0.0;
		double BroadcastUs = 0.0;
		double BookkeepingUs = 0.0;
		/** Inside the instancer work: sweep the cells, hand the transforms to the ISM, rebuild the maps. */
		double RebuildSweepUs = 0.0;
		double AddInstancesUs = 0.0;
		double RebuildMapUs = 0.0;
		int32 Calls = 0;
	};
	static FBeanEditTiming EditTiming;
	static void ResetEditTiming();

	/** Drop the whole history (import, clear, load, resize: anything that makes the old steps lie). */
	void ClearHistory();
	/** Label of the step Undo would revert (shown on the button tooltip). */
	FString GetUndoLabel() const;
	FString GetRedoLabel() const;

	// -- rectangular batch operations ----------------------------------------
	/** Apply one batch operation over the inclusive cell rectangle. Returns the number of cells
	 *  that actually changed (0 = nothing to do, no history entry is created). */
	int32 ApplyBatch(EBeanBatchOp Op, int32 MinX, int32 MinY, int32 MaxX, int32 MaxY,
		uint8 ReplaceFrom = EmptyCell, uint8 ReplaceTo = EmptyCell);

	// -- target pattern (hint layer) & completion statistics -----------------
	/** True when a target pattern is loaded (an import preview exists, applied or not). */
	bool HasTargetPattern() const;
	/** Palette index the pattern wants at this cell, or EmptyCell when it asks for nothing. */
	uint8 GetTargetCell(int32 X, int32 Y) const;
	/** Record / clear the target pattern explicitly (used by the auto self-test). */
	void SetTargetPattern(int32 InWidth, int32 InHeight, int32 OriginX, int32 OriginY,
		const TArray<uint8>& InTargetCells, const TArray<bool>& InMask);
	void ClearTargetPattern();

	/** Recompute the completion report from the real cell data. */
	void GetCompletionStats(FBeanCompletionStats& OutStats) const;
	/** Cached variant: only recomputes when something changed since the last call. */
	const FBeanCompletionStats& GetCachedCompletionStats();
	/** Bumped on every data change (rendering, completion, save dirtiness all watch this). */
	uint64 GetDataRevision() const { return DataRevision; }

	// -- save/load -------------------------------------------------------------
	/** Save file base name of the work currently being edited. */
	const FString& GetWorkName() const { return WorkSlotName; }
	void SetWorkName(const FString& InName);

	/** Base name of a canvas that has never been saved. */
	static const TCHAR* GetUntitledWorkName() { return TEXT("未命名"); }

	/** A work name no save file uses yet, derived from BaseName ("未命名", "未命名 2", ...). */
	static FString MakeUniqueWorkName(const FString& BaseName);

	/**
	 * Throw the current work away and start a brand-new empty canvas of the given cell size: the
	 * cells, history, target pattern and selection all go, and the new canvas takes an unused work
	 * name so its first autosave writes a NEW file instead of overwriting the work it replaced.
	 */
	bool NewCanvas(int32 NewWidth, int32 NewHeight, FString& OutError);

	/** Directory holding every work's .sav file. */
	static FString GetSaveDirectory();
	/** Resolve a work name to its (sanitised) save slot name, so names and files never diverge. */
	static FString SanitizeSlotName(const FString& InName);
	static FString SanitizeFileName(const FString& InName);
	/** Full file path a work name maps to (works for slots that do not exist yet). */
	static FString GetSlotFilePath(const FString& SlotName);
	/** True when a file exists that this build can read as a work. */
	static bool DoesSlotExist(const FString& SlotName);

	/**
	 * Write already-serialized save data with the robust temp -> backup -> replace sequence:
	 * a temp file, a size sanity check, the previous good file kept as the single backup, and only
	 * then the temp moved into place. Returns false and preserves the previous save on any failure.
	 * Shared by the normal save, the gallery rename and anything else that rewrites a stored work.
	 */
	static bool WriteSaveDataAtomically(const FString& SlotName, const TArray<uint8>& Bytes, FString& OutError);

	/** Update only a stored work's name, keeping its data, thumbnail and slot identity intact. */
	static bool RenameStoredWork(const FString& SlotName, const FString& NewWorkName, FString& OutError);
	/** Delete a stored work's slot file together with the backup and temp files it owns. */
	static bool DeleteStoredWork(const FString& SlotName, FString& OutError);

	/** Write to the given slot (empty = the current work's slot). */
	void SaveGame();
	bool SaveGameAs(const FString& SlotName, FString& OutError);
	/** Load the given slot (empty = the current work's slot). */
	bool LoadGame();
	bool LoadGameFrom(const FString& SlotName, FString& OutError);

	/** True when the canvas changed since the last successful save. */
	bool IsDirty() const { return bDirty; }
	/** True when the most recent save attempt failed, so the caller can report it. */
	bool DidLastSaveFail() const { return bLastSaveFailed; }
	/** Latest save/load message, for the HUD status line. Empty when there is nothing to say. */
	const FString& GetSaveStatusMessage() const { return SaveStatusMessage; }
	/** Called by the autosave debounce; saves only when the canvas is dirty. */
	bool AutoSaveIfDirty();

	/** Fill a save object with the current canvas (shared by save, autosave and the gallery's
	 *  thumbnail pass, so the on-disk layout is produced in exactly one place). */
	void BuildSaveData(UBeanGridSaveData& OutSave) const;
	/** Apply a validated save object to the canvas. */
	void ApplySaveData(const UBeanGridSaveData& InSave);

	// -- ghost highlight ------------------------------------------------------
	void UpdateGhost(int32 X, int32 Y, bool bValid);
	void SetGhostColor(const FLinearColor& Color);
	void SetGhostVisible(bool bVisible);

	// -- image preview overlay ------------------------------------------------
	/** Project a palette-quantized imported image onto the ground as a hint layer.
	 *  QuantizedCells holds PatternWidth*PatternHeight palette indices (255 = empty). The overlay
	 *  magnifies each block onto the canvas, slightly translucent, divided by grid lines and
	 *  labeled with its 1-based palette number. Cells outside the pattern stay untouched. */
	void BeginImagePreview(const TArray<uint8>& QuantizedCells, int32 PatternWidth, int32 PatternHeight);
	/** Backwards-compatible square overload. */
	void BeginImagePreview(const TArray<uint8>& QuantizedCells, int32 Granularity);
	/** Re-place the current preview pattern without recomputing the image (shape change). */
	void RebuildOverlayTexture();
	/** Build the whole grid from the pending preview cells. */
	void ApplyImage();
	/** Remove the overlay and pending preview. */
	void CancelImage();
	bool HasOverlay() const { return bHasOverlay; }
	bool IsOverlayVisible() const { return bOverlayVisible; }
	float GetOverlayOpacity() const { return OverlayOpacity; }
	void SetOverlayOpacity(float Opacity);
	void SetOverlayVisible(bool bVisible);
	/** Pattern rectangle inside the canvas (valid while bHasOverlay). */
	void GetPatternRect(int32& OutMinX, int32& OutMinY, int32& OutMaxX, int32& OutMaxY) const;
	/** Pattern size in blocks, whether or not it has been applied yet. */
	int32 GetPatternWidth() const { return PatternWidth; }
	int32 GetPatternHeight() const { return PatternHeight; }

	// -- selection highlight ---------------------------------------------------
	/** Rectangular selection in cells. Collapsed (MinX > MaxX) means "no selection". */
	void SetSelection(int32 MinX, int32 MinY, int32 MaxX, int32 MaxY);
	void ClearSelection();
	bool HasSelection() const;
	void GetSelection(int32& OutMinX, int32& OutMinY, int32& OutMaxX, int32& OutMaxY) const;

	// -- image export ----------------------------------------------------------
	/** Largest width/height, in pixels, an exported image may have. */
	static constexpr int32 MaxExportSize = 4096;

	/** The pixels-per-bead choices an export offers, smallest first. Shared by the preview's scale
	 *  buttons and the code that validates the chosen scale, so both always agree. */
	static const TArray<int32>& GetExportScaleOptions();

	/** Inclusive cell bounds of everything an export would draw: the placed beads plus the hint
	 *  layer's blocks wherever no bead sits on them yet. False when there is nothing to draw. */
	bool GetContentBounds(int32& OutMinX, int32& OutMinY, int32& OutMaxX, int32& OutMaxY) const;

	/** The beads an export draws: the placed cells only (no hint layer). */
	void GetPlacedCells(TArray<uint8>& OutCells) const;

	/** Render the bead work into OutImage (see FBeanAssets::CreateCellsImage): the content bounding
	 *  box, one bead per cell, PixelsPerCell x PixelsPerCell pixels each. Returns false and fills
	 *  OutError with a user-facing message when there is nothing to export, the picture would be
	 *  larger than MaxExportSize, or the render fails. When bIncludeOverlay is false the hint layer
	 *  is left out, which is what the "pure work picture" export wants. */
	bool BuildExportImage(int32 PixelsPerCell, FImage& OutImage, FString& OutError, bool bIncludeOverlay = true) const;

	/** Write the bead work to a PNG (BuildExportImage + the file write). Returns false and fills
	 *  OutError with a user-facing message when that picture cannot be produced or saved. */
	bool SaveImageToPng(int32 PixelsPerCell, const FString& FilePath, FString& OutError, bool bIncludeOverlay = true) const;

	/** Render the current work into a small BGRA8 image for the gallery thumbnail. */
	bool BuildThumbnailImage(int32 MaxEdge, FImage& OutImage) const;

	// -- palette bar textures (generated in code) ------------------------------
	UPROPERTY(BlueprintReadOnly, Category = "Bean")
	TArray<UTexture2D*> CubeIconTextures;

	/** Fired after every data change (undo/redo/batch/import/load) so the HUD can refresh. */
	DECLARE_MULTICAST_DELEGATE(OnGridChanged);
	OnGridChanged GridChangedDelegate;

protected:
	virtual void BeginPlay() override;

private:
	void InitializeGrid();
	void BuildComponents();
	void BuildGround();
	void BuildCubeInstancers();
	void BuildOverlayMesh();
	void BuildGhostMesh();
	void BuildSelectionMesh();
	void ApplyPaletteMaterials();

	/** Rebuild one color's instanced meshes from the current cells (full sweep of the canvas). */
	void RebuildColorInstances(int32 ColorIndex);
	/** Rebuild every color's instances in a single pass over the canvas. */
	void RebuildAllInstances();
	/** Rebuild only the colors touched by an edit set (the incremental path). */
	void RebuildInstancesForColors(const TSet<int32>& Colors);
	/**
	 * Incremental instancing: add/remove just the instances of the cells an edit touched instead
	 * of sweeping the canvas for each affected color. OutChangedColors receives every color whose
	 * instanced data moved, so the caller can keep its bookkeeping in sync.
	 */
	void AddCellInstance(uint8 CellValue, int32 X, int32 Y, TSet<int32>& OutChangedColors);
	void RemoveCellInstance(uint8 CellValue, int32 X, int32 Y, TSet<int32>& OutChangedColors);
	/** True while the cell-to-instance maps are known to match the instancers. */
	bool HasIncrementalInstanceState() const;
	/** Grow the per-color maps to one entry per instancer WITHOUT clearing them. Must run before
	 *  any RebuildColorInstances() call: that function adds instances even when a map slot is
	 *  missing, which would leave instances that no cell claims (a fresh canvas that never went
	 *  through a full rebuild did exactly that, and every later incremental edit then desynced). */
	void EnsureCellInstanceMapSized();
	/** Forget the cell-to-instance maps (after any full rebuild or canvas resize). */
	void ResetCellInstanceMaps();
	/** Resize / reposition the ground, overlay and selection planes for the current canvas.
	 *  bAssignGridTexture is false only for the first pass in BuildComponents(), where the canvas
	 *  size is not final yet (the restored work may resize it) and a texture built now would be
	 *  thrown away. */
	void UpdatePlaneTransforms(bool bAssignGridTexture = true);
	void UpdateSelectionVisual();

	/** Drop the oldest undo steps until the history fits in its memory budget. */
	void TrimHistory();

	/** Bump the revision, remember the dirty flag and notify listeners. Rendering is untouched. */
	void MarkDataChanged(bool bMarksDirty);
	/** MarkDataChanged + rebuild only the given colors' instances. */
	void MarkDataChangedAndRebuild(const TSet<int32>& ChangedColors, bool bMarksDirty);

	/** Update the save status line and log it. */
	void SetSaveStatus(const FString& Message, bool bIsError);

	/** Build the target pattern arrays from a pending (unapplied) preview. */
	void SyncTargetFromPending();
	/** Write the pattern's rectangle into the cell arrays, or clear them when there is no pattern. */
	void PlacePatternIntoGrid(TArray<uint8>& InOutCells) const;

	/** Validity checks shared by load and validate: -1 OK, otherwise a message. */
	int32 ValidateSaveData(const UBeanGridSaveData& Save) const;

	UPROPERTY()
	TArray<uint8> Cells;

	/** Palette indices of the pending (unapplied) preview, PatternWidth*PatternHeight. */
	UPROPERTY()
	TArray<uint8> PendingCells;

	/** Target pattern the completion statistics are measured against, Width*Height, 255 = not asked. */
	UPROPERTY()
	TArray<uint8> TargetCells;

	/** True for every cell the target pattern asks to fill. */
	UPROPERTY()
	TArray<bool> TargetMask;

	UPROPERTY()
	TArray<UMaterialInstanceDynamic*> CubeMIDs;

	UPROPERTY()
	TObjectPtr<USceneComponent> RootComp = nullptr;

	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> GroundMesh = nullptr;

	UPROPERTY()
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> CubeInstancers;

	/**
	 * Per-color cell-index -> instance-index maps, used to update a single bead in O(1).
	 *
	 * A single-cell edit used to rebuild that color's whole instancer by sweeping the entire
	 * canvas (Width*Height cells per placement). With these maps an edit only touches the one
	 * instance it changed. They are only valid when every instancer was built through the
	 * incremental path; any ClearInstances() or canvas resize calls ResetCellInstanceMaps().
	 */
	TArray<TMap<int32, int32>> CellToInstance;

	/**
	 * Per-color instance-index -> cell-index, the inverse of CellToInstance.
	 *
	 * RemoveInstance() fills the freed slot with the component's LAST instance. Without this map,
	 * finding which cell that last instance belonged to meant scanning the whole CellToInstance map
	 * (up to Width*Height entries) for every single removal - quadratic, and the actual cause of the
	 * stutter when a large area is cleared or a big stroke is undone.
	 */
	TArray<TMap<int32, int32>> InstanceToCell;

	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> OverlayMesh = nullptr;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> OverlayMID = nullptr;

	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> GhostMesh = nullptr;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> GhostMID = nullptr;

	/** Translucent frame drawn around the rectangular selection. */
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> SelectionMesh = nullptr;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> SelectionMID = nullptr;

	UPROPERTY()
	TObjectPtr<UTexture2D> OverlayTexture = nullptr;

	/** Canvas size in cells. Both default to GridSize; the import flow can change them. */
	int32 Width = GridSize;
	int32 Height = GridSize;

	/** Cell count the ground material's current grid texture was built for (0 = none assigned yet). */
	int32 GroundTextureCellsX = 0;
	int32 GroundTextureCellsY = 0;

	/** Canvas shape, kept so a reload can restore the import UI's state. */
	EBeanCanvasMode CanvasMode = EBeanCanvasMode::Square;

	/** Pattern rectangle inside the canvas (blocks of the imported image). */
	int32 PatternWidth = 0;
	int32 PatternHeight = 0;
	int32 PatternOriginX = 0;
	int32 PatternOriginY = 0;

	/** Preview granularity chosen at import, kept for display and export. */
	int32 PreviewGranularity = GridSize;

	/** Undo history, oldest first. Redo holds the steps undone from the top of Undo. */
	TArray<FBeanEditRecord> UndoStack;
	TArray<FBeanEditRecord> RedoStack;
	int64 HistoryBytes = 0;
	/** Group id handed to the next stroke. NoGroup until the first stroke starts. */
	int32 CurrentGroupId = FBeanEditRecord::NoGroup;

	/** Rectangular selection, inclusive; MinX > MaxX means "no selection". */
	int32 SelectionMinX = 0;
	int32 SelectionMinY = 0;
	int32 SelectionMaxX = -1;
	int32 SelectionMaxY = -1;

	/** Preview overlay opacity. Higher is more opaque (more accurate color). */
	float OverlayOpacity = 1.0f;
	bool bHasOverlay = false;
	bool bOverlayVisible = true;
	bool bInitialized = false;

	/** Bumped on every data change. */
	uint64 DataRevision = 1;
	/** Revision the cached completion report was computed at. */
	mutable uint64 CachedStatsRevision = 0;
	mutable FBeanCompletionStats CachedStats;

	/** True when the canvas changed since the last successful save. */
	bool bDirty = false;
	/** Base name of this work's save slot. */
	FString WorkSlotName = TEXT("BeanGridSlot");
	/** Last save/load message for the HUD. */
	FString SaveStatusMessage;
	bool bLastSaveFailed = false;
};
