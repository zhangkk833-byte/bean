#include "BeanGrid.h"

#include "BeanAssets.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace BeanGridPrivate
{
	/** Suffix used by the "write to temp, then swap" save path. */
	const TCHAR* TempSuffix = TEXT(".tmp");
	/** One previous good file is kept next to the live one. */
	const TCHAR* BackupSuffix = TEXT(".bak");
	/** Save game class/version handed to SaveGameToSlot. */
	const int32 SaveUserIndex = 0;
}

ABeanGrid::ABeanGrid()
{
	// Nothing here ticks: every visual update is driven by an edit, and a 256x256 grid must never
	// scan its cells per frame.
	PrimaryActorTick.bCanEverTick = false;

	RootComp = CreateDefaultSubobject<USceneComponent>(TEXT("RootScene"));
	RootComp->SetMobility(EComponentMobility::Static);
	SetRootComponent(RootComp);
}

void ABeanGrid::BeginPlay()
{
	Super::BeginPlay();
	InitializeGrid();
}

void ABeanGrid::InitializeGrid()
{
	if (bInitialized) return;
	bInitialized = true;

	Palette = FBeanPalette::LoadOrCreateDefault();
	SetCurrentColor(CurrentColorIndex);

	Width = GridSize;
	Height = GridSize;
	Cells.Init(EmptyCell, Width * Height);
	TargetCells.Init(EmptyCell, Width * Height);
	TargetMask.Init(false, Width * Height);

	BuildComponents();
	ApplyPaletteMaterials();

	// A session always starts on a brand-new empty canvas (m03538: "点进去还是上次铺设的地面方块，
	// 修改为每次进去都是一个新的地面"). The previous work is NOT thrown away - it was autosaved while
	// it was being edited and is still listed in the gallery - but it is no longer restored into the
	// live canvas, and the new canvas takes an unused name so nothing can be overwritten.
	SetWorkName(MakeUniqueWorkName(GetUntitledWorkName()));
	bDirty = false;

	// Give the ground the grid texture that matches the fresh canvas. This is the first and only
	// place the initial texture is built, and it happens after the canvas size is final, so the
	// drawn cell lines always agree with the beads (m03538).
	UpdatePlaneTransforms();
}

// ---------------------------------------------------------------------------
// Resolution presets
// ---------------------------------------------------------------------------

const TArray<int32>& ABeanGrid::GetResolutionOptions()
{
	static const TArray<int32> Options = { 32, 64, 128, 256 };
	return Options;
}

const TArray<EBeanCanvasPreset>& ABeanGrid::GetCanvasPresets()
{
	static const TArray<EBeanCanvasPreset> Presets = {
		EBeanCanvasPreset::Square256,
		EBeanCanvasPreset::Landscape4x3,
		EBeanCanvasPreset::Portrait3x4
	};
	return Presets;
}

const TCHAR* ABeanGrid::GetCanvasPresetLabel(EBeanCanvasPreset Preset)
{
	switch (Preset)
	{
	case EBeanCanvasPreset::Landscape4x3: return TEXT("横向 4:3");
	case EBeanCanvasPreset::Portrait3x4:  return TEXT("纵向 3:4");
	default:                              return TEXT("方形 1:1");
	}
}

void ABeanGrid::GetPresetDimensions(EBeanCanvasPreset Preset, int32 Resolution, int32& OutWidth, int32& OutHeight)
{
	// The long edge always keeps the chosen resolution, so the cell size (25cm) and the meaning of
	// "32/64/128/256" never change. Only the short edge shrinks, giving 4:3 or 3:4.
	const int32 Res = FMath::Max(4, Resolution);
	switch (Preset)
	{
	case EBeanCanvasPreset::Landscape4x3:
		OutWidth = Res;
		OutHeight = FMath::Max(1, (Res * 3) / 4);
		break;
	case EBeanCanvasPreset::Portrait3x4:
		OutWidth = FMath::Max(1, (Res * 3) / 4);
		OutHeight = Res;
		break;
	default:
		OutWidth = Res;
		OutHeight = Res;
		break;
	}
}

EBeanCanvasMode ABeanGrid::GetPresetMode(EBeanCanvasPreset Preset)
{
	switch (Preset)
	{
	case EBeanCanvasPreset::Landscape4x3: return EBeanCanvasMode::Landscape;
	case EBeanCanvasPreset::Portrait3x4:  return EBeanCanvasMode::Portrait;
	default:                              return EBeanCanvasMode::Square;
	}
}

// ---------------------------------------------------------------------------
// Components
// ---------------------------------------------------------------------------

void ABeanGrid::BuildComponents()
{
	BuildGround();
	BuildCubeInstancers();
	BuildOverlayMesh();
	BuildGhostMesh();
	BuildSelectionMesh();
	// The grid texture is deliberately left alone here: see UpdatePlaneTransforms(bAssignGridTexture).
	UpdatePlaneTransforms(false);
}

void ABeanGrid::BuildGround()
{
	GroundMesh = NewObject<UStaticMeshComponent>(this, TEXT("GroundMesh"));
	GroundMesh->SetMobility(EComponentMobility::Static);
	GroundMesh->SetupAttachment(RootComp);
	GroundMesh->RegisterComponent();

	UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (PlaneMesh)
	{
		GroundMesh->SetStaticMesh(PlaneMesh);
	}
	// The Engine plane mesh is 100x100 units and lies in the XY plane; scale/position it to cover
	// exactly the canvas (see UpdatePlaneTransforms).

	UMaterial* GroundMaterial = FBeanAssets::GetGroundMaterial();
	if (GroundMaterial)
	{
		UMaterialInstanceDynamic* GroundMID = UMaterialInstanceDynamic::Create(GroundMaterial, this);
		GroundMID->SetTextureParameterValue(TEXT("GridTex"), FBeanAssets::CreateGridTexture());
		GroundMID->SetVectorParameterValue(TEXT("BaseColor"), FLinearColor::White);
		GroundMesh->SetMaterial(0, GroundMID);
	}
}

void ABeanGrid::BuildCubeInstancers()
{
	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UMaterial* CubeMaterial = FBeanAssets::GetCubeMaterial();

	// One instanced mesh per palette color: a whole 256x256 work is still drawn as 48 draw calls
	// and never as thousands of actors.
	CubeInstancers.Reset(Palette.Num());
	CubeMIDs.Reset(Palette.Num());
	for (int32 i = 0; i < Palette.Num(); ++i)
	{
		UInstancedStaticMeshComponent* Instancer = NewObject<UInstancedStaticMeshComponent>(this, *FString::Printf(TEXT("CubeInstancer_%d"), i));
		Instancer->SetMobility(EComponentMobility::Static);
		Instancer->SetupAttachment(RootComp);
		Instancer->RegisterComponent();

		// Ask the engine for swap-removal (UInstancedStaticMeshComponent::SetRemoveSwap, public).
		// This is not a micro-optimisation on a 256x256 work: the default ordered removal has to
		// reindex every instance above the hole (FPrimitiveInstanceDataManager is an id->index map),
		// which measured 887 us per single-bead removal on a canvas of ~65k beads. Swap-removal is
		// O(1) and, just as importantly, is the ordering CellToInstance/InstanceToCell already
		// assume: RemoveCellInstance follows the last instance into the freed slot.
		Instancer->SetRemoveSwap();

		if (CubeMesh)
		{
			Instancer->SetStaticMesh(CubeMesh);
		}
		// Cubes must not block the player/camera (no stepping up onto placed blocks — the
		// player walks/flys straight through them), but they keep blocking the placement
		// ray (Visibility channel) so aiming/removing stays precise.
		Instancer->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		Instancer->SetCastShadow(false);
		if (CubeMaterial)
		{
			UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(CubeMaterial, this);
			MID->SetVectorParameterValue(TEXT("Color"), Palette[i].Color);
			Instancer->SetMaterial(0, MID);
			CubeMIDs.Add(MID);
		}

		CubeInstancers.Add(Instancer);
	}

	// Size the cell<->instance maps together with the instancers. RebuildAllInstances() used to be
	// the only place that sized them, so a canvas that never loaded a save (a brand new one) kept
	// empty map arrays while its instancers filled up, and HasIncrementalInstanceState() stayed
	// false for the rest of the session (incremental edits silently did nothing).
	ResetCellInstanceMaps();
}

void ABeanGrid::BuildOverlayMesh()
{
	OverlayMesh = NewObject<UStaticMeshComponent>(this, TEXT("OverlayMesh"));
	OverlayMesh->SetMobility(EComponentMobility::Static);
	OverlayMesh->SetupAttachment(RootComp);
	OverlayMesh->RegisterComponent();

	UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (PlaneMesh)
	{
		OverlayMesh->SetStaticMesh(PlaneMesh);
	}
	OverlayMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	OverlayMesh->SetCastShadow(false);

	UMaterial* OverlayMaterial = FBeanAssets::GetOverlayMaterial();
	if (OverlayMaterial)
	{
		OverlayMID = UMaterialInstanceDynamic::Create(OverlayMaterial, this);
		OverlayMID->SetScalarParameterValue(TEXT("Opacity"), OverlayOpacity);
		OverlayMesh->SetMaterial(0, OverlayMID);
	}
	OverlayMesh->SetVisibility(false);
}

void ABeanGrid::BuildGhostMesh()
{
	GhostMesh = NewObject<UStaticMeshComponent>(this, TEXT("GhostMesh"));
	GhostMesh->SetMobility(EComponentMobility::Movable);
	GhostMesh->SetupAttachment(RootComp);
	GhostMesh->RegisterComponent();

	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh)
	{
		GhostMesh->SetStaticMesh(CubeMesh);
	}
	// Same footprint as a placed cube but slightly enlarged (3%) so it wraps rather
	// than shares coplanar surfaces, which would z-fight/flicker against a placed cube.
	GhostMesh->SetWorldScale3D(FVector(CellSize / 100.0f) * 1.03f);
	GhostMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GhostMesh->SetCastShadow(false);

	UMaterial* GhostMaterial = FBeanAssets::GetGhostMaterial();
	if (GhostMaterial)
	{
		GhostMID = UMaterialInstanceDynamic::Create(GhostMaterial, this);
		GhostMID->SetScalarParameterValue(TEXT("Opacity"), 0.4f);
		GhostMID->SetVectorParameterValue(TEXT("Color"), FLinearColor::White);
		GhostMesh->SetMaterial(0, GhostMID);
	}
	GhostMesh->SetVisibility(false);
}

void ABeanGrid::BuildSelectionMesh()
{
	SelectionMesh = NewObject<UStaticMeshComponent>(this, TEXT("SelectionMesh"));
	SelectionMesh->SetMobility(EComponentMobility::Movable);
	SelectionMesh->SetupAttachment(RootComp);
	SelectionMesh->RegisterComponent();

	UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (PlaneMesh)
	{
		SelectionMesh->SetStaticMesh(PlaneMesh);
	}
	SelectionMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SelectionMesh->SetCastShadow(false);

	// Reuse the ghost material: it is the project's existing translucent, unlit color shader, so
	// the marquee needs no new material or asset.
	UMaterial* GhostMaterial = FBeanAssets::GetGhostMaterial();
	if (GhostMaterial)
	{
		SelectionMID = UMaterialInstanceDynamic::Create(GhostMaterial, this);
		SelectionMID->SetScalarParameterValue(TEXT("Opacity"), 0.35f);
		SelectionMID->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.1f, 0.8f, 1.0f, 1.0f));
		SelectionMesh->SetMaterial(0, SelectionMID);
	}
	SelectionMesh->SetVisibility(false);
}

void ABeanGrid::UpdatePlaneTransforms(bool bAssignGridTexture)
{
	// The Engine plane is 100x100 units in the XY plane, so scaling by (cells * CellSize / 100)
	// makes one plane cover exactly one canvas. The canvas spans [-W/2*CellSize, +W/2*CellSize]
	// on X and the same on Y, which matches CellToWorld's centering.
	const float ScaleX = Width * CellSize / 100.0f;
	const float ScaleY = Height * CellSize / 100.0f;

	// The ground's drawn squares have to be the same size as the beads at EVERY canvas size, so the
	// grid texture is built for this cell count (m03538: a 32-cell canvas used to show the 256-cell
	// texture squeezed into 32 cells, i.e. 8 ground squares per bead).
	//
	// bAssignGridTexture is false for the very first pass inside BuildComponents(): the canvas is
	// still the default 256 there and the saved work may resize it moments later in ApplySaveData,
	// so building now would waste a 64MB texture (and ~200ms) on every small work. InitializeGrid
	// calls this again right after the load, once the real size is known.
	if (bAssignGridTexture && GroundMesh && (GroundTextureCellsX != Width || GroundTextureCellsY != Height))
	{
		if (UMaterialInstanceDynamic* GroundMID = Cast<UMaterialInstanceDynamic>(GroundMesh->GetMaterial(0)))
		{
			GroundMID->SetTextureParameterValue(TEXT("GridTex"), FBeanAssets::CreateGridTexture(Width, Height));
			GroundTextureCellsX = Width;
			GroundTextureCellsY = Height;
		}
	}

	if (GroundMesh)
	{
		GroundMesh->SetWorldScale3D(FVector(ScaleX, ScaleY, 1.0f));
		GroundMesh->SetRelativeLocation(FVector::ZeroVector);
	}
	if (OverlayMesh)
	{
		OverlayMesh->SetWorldScale3D(FVector(ScaleX, ScaleY, 1.0f));
		// Sit a clear distance above the ground plane so the translucent overlay doesn't
		// z-fight with it (a ~0 offset made it invisible).
		OverlayMesh->SetRelativeLocation(FVector(0.0f, 0.0f, 1.0f));
	}
}

void ABeanGrid::ApplyPaletteMaterials()
{
	CubeIconTextures.Reset(Palette.Num());
	for (int32 i = 0; i < Palette.Num(); ++i)
	{
		CubeIconTextures.Add(FBeanAssets::CreateCubeIconTexture(Palette[i].Color));
	}
}

bool ABeanGrid::ResizeGrid(int32 NewWidth, int32 NewHeight)
{
	if (NewWidth <= 0 || NewHeight <= 0) return false;
	if (static_cast<int64>(NewWidth) * NewHeight > 1024 * 1024) return false;

	Width = NewWidth;
	Height = NewHeight;
	Cells.Init(EmptyCell, Width * Height);
	TargetCells.Init(EmptyCell, Width * Height);
	TargetMask.Init(false, Width * Height);
	PatternWidth = 0;
	PatternHeight = 0;
	PatternOriginX = 0;
	PatternOriginY = 0;
	PreviewGranularity = FMath::Max(Width, Height);
	CanvasMode = (Width == Height) ? EBeanCanvasMode::Square
		: (Width > Height ? EBeanCanvasMode::Landscape : EBeanCanvasMode::Portrait);

	PendingCells.Reset();
	bHasOverlay = false;
	bOverlayVisible = false;
	if (OverlayMesh) OverlayMesh->SetVisibility(false);

	ClearSelection();
	ClearHistory();
	UpdatePlaneTransforms();
	RebuildAllInstances();
	MarkDataChanged(true);
	return true;
}

// ---------------------------------------------------------------------------
// Cell access
// ---------------------------------------------------------------------------

bool ABeanGrid::IsValidCell(int32 X, int32 Y) const
{
	return X >= 0 && X < Width && Y >= 0 && Y < Height;
}

uint8 ABeanGrid::GetCell(int32 X, int32 Y) const
{
	if (!IsValidCell(X, Y)) return EmptyCell;
	const int32 Index = Y * Width + X;
	return Cells.IsValidIndex(Index) ? Cells[Index] : EmptyCell;
}

bool ABeanGrid::IsCellOccupied(int32 X, int32 Y) const
{
	return GetCell(X, Y) != EmptyCell;
}

FVector ABeanGrid::CellToWorld(int32 X, int32 Y) const
{
	// Cell centers: cell 0 sits at -W/2*CellSize + CellSize/2, so the canvas is centered on the
	// actor origin for any width/height (the old square grid's -127.5 is the 256-wide case).
	const float WorldX = (static_cast<float>(X) - (static_cast<float>(Width) - 1.0f) * 0.5f) * CellSize;
	const float WorldY = (static_cast<float>(Y) - (static_cast<float>(Height) - 1.0f) * 0.5f) * CellSize;
	return FVector(WorldX, WorldY, 0.0f);
}

bool ABeanGrid::WorldToCell(const FVector& WorldPos, int32& X, int32& Y) const
{
	// Inverse of CellToWorld: shift to the canvas corner, then floor one cell.
	const float HalfExtentX = Width * CellSize * 0.5f;
	const float HalfExtentY = Height * CellSize * 0.5f;
	X = FMath::FloorToInt((WorldPos.X + HalfExtentX) / CellSize);
	Y = FMath::FloorToInt((WorldPos.Y + HalfExtentY) / CellSize);
	return IsValidCell(X, Y);
}

int32 ABeanGrid::NearestColorIndex(const FLinearColor& Color) const
{
	return FBeanPalette::NearestColorIndex(Palette, Color);
}

void ABeanGrid::PlaceCell(int32 X, int32 Y)
{
	// Single-cell convenience wrapper: still goes through the one mutation entry point so undo,
	// completion and the dirty flag all behave identically to a batch.
	// Occupied cells are ignored (right-click removes first), matching the original behavior.
	if (!IsValidCell(X, Y) || IsCellOccupied(X, Y)) return;

	FBeanCellEdit Edit;
	Edit.X = X;
	Edit.Y = Y;
	Edit.Before = GetCell(X, Y);
	Edit.After = static_cast<uint8>(CurrentColorIndex);
	if (Edit.Before == Edit.After) return;

	TArray<FBeanCellEdit> Edits;
	Edits.Add(Edit);
	ApplyCellEdits(MoveTemp(Edits), TEXT("放置方块"), true);
}

void ABeanGrid::RemoveCell(int32 X, int32 Y)
{
	if (!IsValidCell(X, Y)) return;
	const uint8 OldColor = GetCell(X, Y);
	if (OldColor == EmptyCell) return;

	FBeanCellEdit Edit;
	Edit.X = X;
	Edit.Y = Y;
	Edit.Before = OldColor;
	Edit.After = EmptyCell;

	TArray<FBeanCellEdit> Edits;
	Edits.Add(Edit);
	ApplyCellEdits(MoveTemp(Edits), TEXT("移除方块"), true);
}

void ABeanGrid::ClearAllCells()
{
	ClearSelection();

	TArray<FBeanCellEdit> Edits;
	const int32 Count = Width * Height;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		if (Cells[Index] == EmptyCell) continue;
		FBeanCellEdit Edit;
		Edit.X = Index % Width;
		Edit.Y = Index / Width;
		Edit.Before = Cells[Index];
		Edit.After = EmptyCell;
		Edits.Add(Edit);
	}

	if (Edits.Num() == 0) return;
	// Never merged into the previous stroke: "clear" is always its own undo step.
	ApplyCellEdits(MoveTemp(Edits), TEXT("清空画布"), false);
}

void ABeanGrid::SetCurrentColor(int32 Index)
{
	if (Palette.Num() == 0) return;
	CurrentColorIndex = FMath::Clamp(Index, 0, Palette.Num() - 1);
}

void ABeanGrid::CycleColor(int32 Delta)
{
	if (Palette.Num() == 0) return;
	CurrentColorIndex = (CurrentColorIndex + Delta + Palette.Num()) % Palette.Num();
}

// ---------------------------------------------------------------------------
// Editing entry point + undo history
// ---------------------------------------------------------------------------

ABeanGrid::FBeanEditTiming ABeanGrid::EditTiming;

void ABeanGrid::ResetEditTiming()
{
	EditTiming = FBeanEditTiming();
}

void ABeanGrid::ApplyCellEdits(TArray<FBeanCellEdit>&& Edits, const FString& Label, bool bMergeWithPrevious)
{
	if (Edits.Num() == 0) return;
	if (Cells.Num() != Width * Height) return;

	++EditTiming.Calls;
	const double T0 = FPlatformTime::Seconds();
	TSet<int32> ChangedColors;
	int32 AppliedCount = 0;
	for (const FBeanCellEdit& Edit : Edits)
	{
		if (!IsValidCell(Edit.X, Edit.Y)) continue;
		const int32 Index = Edit.Y * Width + Edit.X;
		if (Cells[Index] == Edit.After) continue; // already in the requested state: no-op

		// Re-read "before" from the live data so a chained edit can never record a stale value.
		const uint8 LiveBefore = Cells[Index];
		Cells[Index] = Edit.After;
		++AppliedCount;

		if (LiveBefore != EmptyCell) ChangedColors.Add(LiveBefore);
		if (Edit.After != EmptyCell) ChangedColors.Add(Edit.After);
	}
	if (AppliedCount == 0) return;

	// Kept before the merge below, which moves the caller's edits into the history record.
	// The rendering path needs it to decide between incremental instancing and a full sweep.
	const int32 EditCount = Edits.Num();
	const double T1 = FPlatformTime::Seconds();
	EditTiming.EditLoopUs += (T1 - T0) * 1.0e6;

	// ---- undo record -------------------------------------------------------
	FBeanEditRecord Record;
	Record.Edits = Edits; // stored with the caller's before/after, as applied above
	Record.Label = Label;

	bool bCanMerge = bMergeWithPrevious
		&& CurrentGroupId != FBeanEditRecord::NoGroup
		&& UndoStack.Num() > 0
		&& UndoStack.Last().bOpen
		&& UndoStack.Last().GroupId == CurrentGroupId;

	// Hard per-record cap: TrimHistory can only drop whole records, so a very long stroke would
	// otherwise be one unbounded record that no limit can reach. Passing the budget closes the live
	// record, so this edit starts a fresh one instead of appending to an oversized record.
	if (bCanMerge && UndoStack.Last().ByteSize() > HistoryMemoryLimitBytes)
	{
		UndoStack.Last().bOpen = false;
		bCanMerge = false;
	}

	if (bCanMerge)
	{
		FBeanEditRecord& Top = UndoStack.Last();
		for (const FBeanCellEdit& Edit : Record.Edits)
		{
			const int32 Index = Edit.Y * Width + Edit.X;
			Top.Edits.Add(Edit);

			// Keep one entry per cell while it stays inside the open stroke: walk back a bounded
			// number of entries (a drag briefly sweeping over the same cell twice must not scan
			// the whole stroke, which could be a 65536-cell clear).
			if (Top.Edits.Num() > 1)
			{
				const int32 ScanFrom = FMath::Max(0, Top.Edits.Num() - 1 - 1024);
				for (int32 i = Top.Edits.Num() - 2; i >= ScanFrom; --i)
				{
					if (Top.Edits[i].X == Edit.X && Top.Edits[i].Y == Edit.Y)
					{
						// The first value this cell had during the stroke is the real "before".
						Top.Edits[i].After = Edit.After;
						Top.Edits.RemoveAt(Top.Edits.Num() - 1, 1, EAllowShrinking::No);
						break;
					}
				}
			}
		}
	}
	else
	{
		// A new edit invalidates the redo branch, exactly like every editor does. The stroke is
		// closed first: a bulk operation must never be appended to a still-open stroke record, and
		// every limit in TrimHistory is per-record, so an append-only record could grow forever.
		EndActiveStroke();
		RedoStack.Reset();
		UndoStack.Add(MoveTemp(Record));
		TrimHistory();
	}
	HistoryBytes = 0;
	for (const FBeanEditRecord& Item : UndoStack) HistoryBytes += Item.ByteSize();

	const double T2 = FPlatformTime::Seconds();
	EditTiming.UndoUs += (T2 - T1) * 1.0e6;

	// ---- rendering: only the cells that actually changed -------------------
	// Incremental instancing is O(edits); a full per-color sweep is O(canvas) per touched color.
	// Single placements and drags (the interactive path) are tiny edit sets, so they must not
	// rescan the whole canvas. Wide edits fall back to the batched sweep, which is cheaper there.
	constexpr int32 MaxIncrementalEdits = 256;
	if (HasIncrementalInstanceState() && EditCount <= MaxIncrementalEdits)
	{
		TSet<int32> TouchedColors;
		for (const FBeanCellEdit& Edit : Edits)
		{
			if (!IsValidCell(Edit.X, Edit.Y)) continue;
			if (Edit.Before != EmptyCell)
			{
				RemoveCellInstance(Edit.Before, Edit.X, Edit.Y, TouchedColors);
			}
			if (Edit.After != EmptyCell)
			{
				AddCellInstance(Edit.After, Edit.X, Edit.Y, TouchedColors);
			}
		}
		MarkDataChanged(true);
	}
	else
	{
		MarkDataChangedAndRebuild(ChangedColors, true);
	}
	const double T3 = FPlatformTime::Seconds();
	EditTiming.InstanceUs += (T3 - T2) * 1.0e6;
}

void ABeanGrid::BeginStroke(const FString& Label)
{
	++CurrentGroupId;

	FBeanEditRecord Seed;
	Seed.GroupId = CurrentGroupId;
	Seed.bOpen = true;
	Seed.Label = Label;
	RedoStack.Reset();
	UndoStack.Add(MoveTemp(Seed));
}

void ABeanGrid::EndStroke()
{
	EndActiveStroke();
	CurrentGroupId = FBeanEditRecord::NoGroup;
	TrimHistory();
}

void ABeanGrid::EndActiveStroke()
{
	// Closing the group is what makes the NEXT edit start a new undo step.
	if (UndoStack.Num() > 0 && UndoStack.Last().bOpen && UndoStack.Last().GroupId == CurrentGroupId)
	{
		if (UndoStack.Last().Edits.Num() == 0)
		{
			// A stroke that changed nothing (a click on an occupied cell) must not leave an
			// empty undo step behind.
			UndoStack.Pop(EAllowShrinking::No);
		}
		else
		{
			UndoStack.Last().bOpen = false;
		}
	}
	TrimHistory();
}

void ABeanGrid::TrimHistory()
{
	// The history only ever stores changed cells, so a full 256x256 clear is ~192KB. The cap
	// exists to bound a long session, not to bound a single action: a lone oversized record is
	// kept so the player can always undo what they just did.
	HistoryBytes = 0;
	for (const FBeanEditRecord& Item : UndoStack) HistoryBytes += Item.ByteSize();

	while (UndoStack.Num() > 1 && HistoryBytes > HistoryMemoryLimitBytes)
	{
		HistoryBytes -= UndoStack[0].ByteSize();
		UndoStack.RemoveAt(0, 1, EAllowShrinking::No);
	}
}

void ABeanGrid::ClearHistory()
{
	UndoStack.Reset();
	RedoStack.Reset();
	HistoryBytes = 0;
	CurrentGroupId = FBeanEditRecord::NoGroup;
}

bool ABeanGrid::CanUndo() const
{
	return UndoStack.Num() > 0 && UndoStack.Last().Edits.Num() > 0;
}

bool ABeanGrid::CanRedo() const
{
	return RedoStack.Num() > 0;
}

FString ABeanGrid::GetUndoLabel() const
{
	return CanUndo() ? UndoStack.Last().Label : FString();
}

FString ABeanGrid::GetRedoLabel() const
{
	return CanRedo() ? RedoStack.Last().Label : FString();
}

void ABeanGrid::Undo()
{
	if (!CanUndo()) return;

	FBeanEditRecord Record = UndoStack.Pop(EAllowShrinking::No);
	TSet<int32> ChangedColors;
	for (const FBeanCellEdit& Edit : Record.Edits)
	{
		if (!IsValidCell(Edit.X, Edit.Y)) continue;
		const int32 Index = Edit.Y * Width + Edit.X;
		const uint8 Live = Cells[Index];
		Cells[Index] = Edit.Before;
		if (Live != EmptyCell) ChangedColors.Add(Live);
		if (Edit.Before != EmptyCell) ChangedColors.Add(Edit.Before);
	}
	RedoStack.Add(MoveTemp(Record));
	RebuildInstancesForColors(ChangedColors);
	MarkDataChanged(true);
}

void ABeanGrid::Redo()
{
	if (!CanRedo()) return;

	FBeanEditRecord Record = RedoStack.Pop(EAllowShrinking::No);
	TSet<int32> ChangedColors;
	for (const FBeanCellEdit& Edit : Record.Edits)
	{
		if (!IsValidCell(Edit.X, Edit.Y)) continue;
		const int32 Index = Edit.Y * Width + Edit.X;
		const uint8 Live = Cells[Index];
		Cells[Index] = Edit.After;
		if (Live != EmptyCell) ChangedColors.Add(Live);
		if (Edit.After != EmptyCell) ChangedColors.Add(Edit.After);
	}
	UndoStack.Add(MoveTemp(Record));
	RebuildInstancesForColors(ChangedColors);
	MarkDataChanged(true);
}

// ---------------------------------------------------------------------------
// Rectangular batch operations
// ---------------------------------------------------------------------------

int32 ABeanGrid::ApplyBatch(EBeanBatchOp Op, int32 MinX, int32 MinY, int32 MaxX, int32 MaxY,
	uint8 ReplaceFrom, uint8 ReplaceTo)
{
	if (Cells.Num() != Width * Height) return 0;

	const int32 X0 = FMath::Clamp(FMath::Min(MinX, MaxX), 0, Width - 1);
	const int32 X1 = FMath::Clamp(FMath::Max(MinX, MaxX), 0, Width - 1);
	const int32 Y0 = FMath::Clamp(FMath::Min(MinY, MaxY), 0, Height - 1);
	const int32 Y1 = FMath::Clamp(FMath::Max(MinY, MaxY), 0, Height - 1);

	TArray<FBeanCellEdit> Edits;
	Edits.Reserve((X1 - X0 + 1) * (Y1 - Y0 + 1));

	const bool bHasTarget = HasTargetPattern();
	const int32 PaletteCount = Palette.Num();

	// One pass builds the whole delta; rendering is refreshed once, per color, afterwards.
	for (int32 Y = Y0; Y <= Y1; ++Y)
	{
		for (int32 X = X0; X <= X1; ++X)
		{
			const int32 Index = Y * Width + X;
			const uint8 Before = Cells[Index];

			uint8 After = Before;
			switch (Op)
			{
			case EBeanBatchOp::PlaceCurrentColor:
				if (Before == EmptyCell) After = static_cast<uint8>(FMath::Clamp(CurrentColorIndex, 0, FMath::Max(0, PaletteCount - 1)));
				break;

			case EBeanBatchOp::Remove:
				After = EmptyCell;
				break;

			case EBeanBatchOp::ReplaceColor:
				// EmptyCell as the source means "every occupied cell".
				if (Before != EmptyCell && (ReplaceFrom == EmptyCell || Before == ReplaceFrom))
				{
					After = ReplaceTo;
				}
				break;

			case EBeanBatchOp::FillFromPattern:
				if (bHasTarget && TargetMask.IsValidIndex(Index) && TargetMask[Index])
				{
					After = TargetCells[Index];
				}
				break;
			}

			if (After == Before) continue;

			FBeanCellEdit Edit;
			Edit.X = X;
			Edit.Y = Y;
			Edit.Before = Before;
			Edit.After = After;
			Edits.Add(Edit);
		}
	}

	if (Edits.Num() == 0) return 0;

	const FString Label = FString::Printf(TEXT("批量%s（%d 格）"),
		Op == EBeanBatchOp::Remove ? TEXT("移除")
			: Op == EBeanBatchOp::ReplaceColor ? TEXT("换色")
			: Op == EBeanBatchOp::FillFromPattern ? TEXT("补齐")
			: TEXT("放置"),
		Edits.Num());

	// A batch is one atomic undo step: never merged with the previous stroke, and it also closes the
	// active stroke. Without closing it, a batch would be appended to a still-open stroke record and
	// every following batch would keep growing that same record - the per-record history budget
	// would never trigger, so a long session of full-canvas batches grow without bound.
	EndActiveStroke();
	ApplyCellEdits(MoveTemp(Edits), Label, false);
	return Edits.Num();
}

// ---------------------------------------------------------------------------
// Target pattern and completion statistics
// ---------------------------------------------------------------------------

bool ABeanGrid::HasTargetPattern() const
{
	return TargetCells.Num() == Width * Height && PatternWidth > 0 && PatternHeight > 0;
}

uint8 ABeanGrid::GetTargetCell(int32 X, int32 Y) const
{
	if (!IsValidCell(X, Y) || !HasTargetPattern()) return EmptyCell;
	const int32 Index = Y * Width + X;
	return TargetMask.IsValidIndex(Index) && TargetMask[Index] ? TargetCells[Index] : EmptyCell;
}

void ABeanGrid::SetTargetPattern(int32 InWidth, int32 InHeight, int32 OriginX, int32 OriginY,
	const TArray<uint8>& InTargetCells, const TArray<bool>& InMask)
{
	TargetCells.Init(EmptyCell, Width * Height);
	TargetMask.Init(false, Width * Height);

	PatternWidth = FMath::Max(0, InWidth);
	PatternHeight = FMath::Max(0, InHeight);
	PatternOriginX = OriginX;
	PatternOriginY = OriginY;

	if (InTargetCells.Num() == PatternWidth * PatternHeight && PatternWidth > 0 && PatternHeight > 0)
	{
		const bool bHasMask = InMask.Num() == InTargetCells.Num();
		for (int32 Y = 0; Y < PatternHeight; ++Y)
		{
			for (int32 X = 0; X < PatternWidth; ++X)
			{
				const int32 Src = Y * PatternWidth + X;
				const int32 DstX = OriginX + X;
				const int32 DstY = OriginY + Y;
				if (!IsValidCell(DstX, DstY)) continue;

				TargetCells[DstY * Width + DstX] = InTargetCells[Src];
				TargetMask[DstY * Width + DstX] = bHasMask ? InMask[Src] : (InTargetCells[Src] != EmptyCell);
			}
		}
	}

	MarkDataChanged(false);
}

void ABeanGrid::ClearTargetPattern()
{
	PatternWidth = 0;
	PatternHeight = 0;
	PatternOriginX = 0;
	PatternOriginY = 0;
	TargetCells.Init(EmptyCell, Width * Height);
	TargetMask.Init(false, Width * Height);
	MarkDataChanged(false);
}

void ABeanGrid::GetCompletionStats(FBeanCompletionStats& OutStats) const
{
	const int32 PaletteCount = Palette.Num();
	const bool bHasPattern = HasTargetPattern();

	OutStats = FBeanCompletionStats();
	OutStats.bHasPattern = bHasPattern;
	OutStats.PerColor.SetNum(PaletteCount);
	for (int32 i = 0; i < PaletteCount; ++i)
	{
		FBeanColorProgress& Row = OutStats.PerColor[i];
		Row.ColorIndex = i;
		Row.ColorName = Palette[i].Name;
		Row.Color = Palette[i].Color;
	}

	if (Cells.Num() != Width * Height)
	{
		return;
	}

	// Per-color tallies first, then the totals: one pass over the canvas, no per-cell allocation.
	TArray<int32> CorrectPerColor;
	TArray<int32> MiscoloredPerColor;
	CorrectPerColor.Init(0, PaletteCount);
	MiscoloredPerColor.Init(0, PaletteCount);

	const int32 Count = Width * Height;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const uint8 Placed = Cells[Index];
		const bool bWanted = bHasPattern && TargetMask.IsValidIndex(Index) && TargetMask[Index];
		const uint8 Target = bWanted ? TargetCells[Index] : EmptyCell;
		const bool bTargetEmpty = (Target == EmptyCell);

		if (!bWanted)
		{
			// Not part of the pattern: a bead here is simply extra, never a failure.
			if (Placed != EmptyCell) ++OutStats.ExtraCells;
			continue;
		}

		if (bTargetEmpty)
		{
			// Marked by the mask but asking for no colour: this is a background cell, not a
			// requirement. It must stay out of TargetCells AND out of MissingCells, otherwise
			// "missing" could exceed "target" and the percentage denominator would be wrong
			// (a bead here is already counted as extra by the !bWanted branch above).
			continue;
		}

		++OutStats.TargetCells;
		if (Placed == EmptyCell)
		{
			++OutStats.MissingCells;
		}
		else if (Placed == Target)
		{
			++OutStats.CorrectCells;
			if (CorrectPerColor.IsValidIndex(Target)) ++CorrectPerColor[Target];
		}
		else
		{
			++OutStats.MiscoloredCells;
			if (MiscoloredPerColor.IsValidIndex(Placed)) ++MiscoloredPerColor[Placed];
		}
	}

	// Required-per-color needs its own pass over the pattern (it is independent of what is placed).
	if (bHasPattern)
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (!TargetMask[Index]) continue;
			const uint8 Target = TargetCells[Index];
			if (Target != EmptyCell && OutStats.PerColor.IsValidIndex(Target))
			{
				++OutStats.PerColor[Target].Required;
			}
		}
	}

	for (int32 i = 0; i < OutStats.PerColor.Num(); ++i)
	{
		FBeanColorProgress& Row = OutStats.PerColor[i];
		Row.Correct = CorrectPerColor[i];
		Row.Miscolored = MiscoloredPerColor[i];
		Row.Remaining = FMath::Max(0, Row.Required - Row.Correct);
	}

	OutStats.CompletionPercent = OutStats.TargetCells > 0
		? FMath::Clamp(static_cast<float>(OutStats.CorrectCells) / static_cast<float>(OutStats.TargetCells) * 100.0f, 0.0f, 100.0f)
		: 0.0f;
}

const FBeanCompletionStats& ABeanGrid::GetCachedCompletionStats()
{
	if (CachedStatsRevision != DataRevision)
	{
		GetCompletionStats(CachedStats);
		CachedStatsRevision = DataRevision;
	}
	return CachedStats;
}

// ---------------------------------------------------------------------------
// Instanced rendering
// ---------------------------------------------------------------------------

void ABeanGrid::RebuildColorInstances(int32 ColorIndex)
{
	if (!CubeInstancers.IsValidIndex(ColorIndex)) return;
	UInstancedStaticMeshComponent* Instancer = CubeInstancers[ColorIndex];
	if (!Instancer) return;

	// A color can be rebuilt before any full rebuild sized the maps (fresh canvas, first edit).
	// Without this the instancer below would gain instances that no map entry claims.
	EnsureCellInstanceMapSized();

	Instancer->ClearInstances();
	if (CellToInstance.IsValidIndex(ColorIndex))
	{
		CellToInstance[ColorIndex].Reset();
	}
	if (InstanceToCell.IsValidIndex(ColorIndex))
	{
		InstanceToCell[ColorIndex].Reset();
	}

	// One pass fills the transforms AND the maps: the batched add below appends instances in exactly
	// this order, so the instance index is known while the cell is visited. This used to sweep the
	// cells twice (once for transforms, once for the maps); at 256x256 that second sweep, the
	// Rotator->Quat conversion per cell and the TArray growth together were ~8 ms of the 15 ms a
	// whole-canvas fill cost.
	TMap<int32, int32>* Map = CellToInstance.IsValidIndex(ColorIndex) ? &CellToInstance[ColorIndex] : nullptr;
	TMap<int32, int32>* Inverse = InstanceToCell.IsValidIndex(ColorIndex) ? &InstanceToCell[ColorIndex] : nullptr;

	int32 Matching = 0;
	for (int32 CellIndex = 0; CellIndex < Cells.Num(); ++CellIndex)
	{
		if (Cells[CellIndex] == ColorIndex)
		{
			++Matching;
		}
	}

	TArray<FTransform> Transforms;
	Transforms.Reserve(Matching);
	if (Map) { Map->Reserve(Matching); }
	if (Inverse) { Inverse->Reserve(Matching); }

	// Cube mesh is 100x100x100 units, so scale by CellSize/100 to match one cell (0.25m).
	const float CubeScale = CellSize / 100.0f;
	const FVector Scale3D(CubeScale);
	// Same maths as CellToWorld, hoisted out of the inner loop.
	const float OriginX = -(static_cast<float>(Width) - 1.0f) * 0.5f * CellSize;
	const float OriginY = -(static_cast<float>(Height) - 1.0f) * 0.5f * CellSize;
	const float WorldZ = CellSize * 0.5f;

	const double SweepStart = FPlatformTime::Seconds();
	int32 InstanceIndex = 0;
	int32 CellIndex = 0;
	for (int32 Y = 0; Y < Height; ++Y)
	{
		const float WorldY = OriginY + static_cast<float>(Y) * CellSize;
		for (int32 X = 0; X < Width; ++X, ++CellIndex)
		{
			if (Cells[CellIndex] != ColorIndex)
			{
				continue;
			}
			// FQuat::Identity rather than FRotator::ZeroRotator: the Rotator overload runs sin/cos
			// per instance, which at 65k beads is milliseconds for a rotation that is not there.
			Transforms.Emplace(FQuat::Identity, FVector(OriginX + static_cast<float>(X) * CellSize, WorldY, WorldZ), Scale3D);
			if (Map)
			{
				Map->Add(CellIndex, InstanceIndex);
			}
			if (Inverse)
			{
				Inverse->Add(InstanceIndex, CellIndex);
			}
			++InstanceIndex;
		}
	}
	const double AddStart = FPlatformTime::Seconds();
	if (Transforms.Num() > 0)
	{
		Instancer->AddInstances(Transforms, false, false, false);
	}
	const double Done = FPlatformTime::Seconds();
	EditTiming.RebuildSweepUs += (AddStart - SweepStart) * 1.0e6;
	EditTiming.AddInstancesUs += (Done - AddStart) * 1.0e6;
	EditTiming.RebuildMapUs += 0.0;
}

bool ABeanGrid::HasIncrementalInstanceState() const
{
	// The maps are only meaningful when they exist for every instancer and no full rebuild or
	// resize has invalidated them. CubeInstancers is rebuilt exactly when the canvas changes.
	if (CellToInstance.Num() != CubeInstancers.Num() || CubeInstancers.Num() == 0)
	{
		return false;
	}
	return true;
}

int32 ABeanGrid::GetInstanceCountForColor(int32 ColorIndex) const
{
	if (!CubeInstancers.IsValidIndex(ColorIndex) || !CubeInstancers[ColorIndex])
	{
		return 0;
	}
	return CubeInstancers[ColorIndex]->GetInstanceCount();
}

int32 ABeanGrid::GetMappedCellCountForColor(int32 ColorIndex) const
{
	return CellToInstance.IsValidIndex(ColorIndex) ? CellToInstance[ColorIndex].Num() : 0;
}

bool ABeanGrid::SupportsInstanceRemoveSwap() const
{
	for (const UInstancedStaticMeshComponent* Instancer : CubeInstancers)
	{
		if (!Instancer) continue;
		return Instancer->SupportsRemoveSwap();
	}
	return false;
}

bool ABeanGrid::ValidateInstanceMappings(FString& OutError) const
{
	const float CubeScale = CellSize / 100.0f;
	for (int32 ColorIndex = 0; ColorIndex < CubeInstancers.Num(); ++ColorIndex)
	{
		const UInstancedStaticMeshComponent* Instancer = CubeInstancers[ColorIndex];
		if (!Instancer) continue;

		const int32 InstanceCount = Instancer->GetInstanceCount();
		const TMap<int32, int32>* Map = CellToInstance.IsValidIndex(ColorIndex) ? &CellToInstance[ColorIndex] : nullptr;
		const TMap<int32, int32>* Inverse = InstanceToCell.IsValidIndex(ColorIndex) ? &InstanceToCell[ColorIndex] : nullptr;
		const int32 MappedCount = Map ? Map->Num() : 0;

		if (InstanceCount != MappedCount)
		{
			OutError = FString::Printf(TEXT("color %d: %d instances but %d mapped cells"), ColorIndex, InstanceCount, MappedCount);
			return false;
		}
		if (MappedCount == 0) continue;
		if (!Inverse || Inverse->Num() != MappedCount)
		{
			OutError = FString::Printf(TEXT("color %d: inverse map holds %d entries for %d cells"), ColorIndex, Inverse ? Inverse->Num() : -1, MappedCount);
			return false;
		}

		// Every instance index has to be claimed exactly once, and the two maps must mirror each
		// other. A swap that was not followed by the inverse update shows up here immediately.
		TArray<bool> Seen;
		Seen.Init(false, InstanceCount);
		for (const TPair<int32, int32>& Pair : *Map)
		{
			const int32 CellIndex = Pair.Key;
			const int32 InstanceIndex = Pair.Value;
			if (!Seen.IsValidIndex(InstanceIndex))
			{
				OutError = FString::Printf(TEXT("color %d: cell %d points at instance %d of %d"), ColorIndex, CellIndex, InstanceIndex, InstanceCount);
				return false;
			}
			if (Seen[InstanceIndex])
			{
				OutError = FString::Printf(TEXT("color %d: instance %d is claimed by more than one cell"), ColorIndex, InstanceIndex);
				return false;
			}
			Seen[InstanceIndex] = true;

			const int32* BackCell = Inverse->Find(InstanceIndex);
			if (!BackCell || *BackCell != CellIndex)
			{
				OutError = FString::Printf(TEXT("color %d: cell %d -> instance %d but the inverse says %d"), ColorIndex, CellIndex, InstanceIndex, BackCell ? *BackCell : -1);
				return false;
			}

			FTransform Actual;
			if (!Instancer->GetInstanceTransform(InstanceIndex, Actual, false))
			{
				OutError = FString::Printf(TEXT("color %d: instance %d has no transform"), ColorIndex, InstanceIndex);
				return false;
			}
			const int32 X = CellIndex % Width;
			const int32 Y = CellIndex / Width;
			const FVector Expected = CellToWorld(X, Y) + FVector(0.0f, 0.0f, CellSize * 0.5f);
			if (!Actual.GetLocation().Equals(Expected, 0.01f) || !Actual.GetScale3D().Equals(FVector(CubeScale), 0.01f))
			{
				OutError = FString::Printf(TEXT("color %d: instance %d sits at %s instead of cell (%d,%d) %s"),
					ColorIndex, InstanceIndex, *Actual.GetLocation().ToCompactString(), X, Y, *Expected.ToCompactString());
				return false;
			}
		}
	}

	OutError.Reset();
	return true;
}

void ABeanGrid::EnsureCellInstanceMapSized()
{
	// SetNum only ever grows/shinks the outer array here; existing TMap entries stay untouched, so
	// calling this on an already-sized (and fully populated) state is a no-op.
	if (CellToInstance.Num() != CubeInstancers.Num())
	{
		CellToInstance.SetNum(CubeInstancers.Num());
	}
	if (InstanceToCell.Num() != CubeInstancers.Num())
	{
		InstanceToCell.SetNum(CubeInstancers.Num());
	}
}

void ABeanGrid::ResetCellInstanceMaps(){
	EnsureCellInstanceMapSized();
	for (TMap<int32, int32>& Map : CellToInstance)
	{
		Map.Reset();
	}
	for (TMap<int32, int32>& Map : InstanceToCell)
	{
		Map.Reset();
	}
}

void ABeanGrid::AddCellInstance(uint8 CellValue, int32 X, int32 Y, TSet<int32>& OutChangedColors)
{
	if (!HasIncrementalInstanceState() || !CubeInstancers.IsValidIndex(CellValue) || !IsValidCell(X, Y))
	{
		return;
	}
	UInstancedStaticMeshComponent* Instancer = CubeInstancers[CellValue];
	if (!Instancer)
	{
		return;
	}

	const int32 CellIndex = Y * Width + X;
	TMap<int32, int32>& Map = CellToInstance[CellValue];
	if (const int32* Found = Map.Find(CellIndex))
	{
		// Already instanced (e.g. a redo): just make sure the transform is current.
		const float CubeScale = CellSize / 100.0f;
		const FTransform Transform(FRotator::ZeroRotator,
			CellToWorld(X, Y) + FVector(0.0f, 0.0f, CellSize * 0.5f), FVector(CubeScale));
		Instancer->UpdateInstanceTransform(*Found, Transform, false, true, true);
		OutChangedColors.Add(CellValue);
		return;
	}

	const float CubeScale = CellSize / 100.0f;
	const FTransform Transform(FRotator::ZeroRotator,
		CellToWorld(X, Y) + FVector(0.0f, 0.0f, CellSize * 0.5f), FVector(CubeScale));
	const int32 NewIndex = Instancer->AddInstance(Transform, false);
	if (NewIndex >= 0)
	{
		Map.Add(CellIndex, NewIndex);
		InstanceToCell[CellValue].Add(NewIndex, CellIndex);
	}
	OutChangedColors.Add(CellValue);
}

void ABeanGrid::RemoveCellInstance(uint8 CellValue, int32 X, int32 Y, TSet<int32>& OutChangedColors)
{
	if (!HasIncrementalInstanceState() || !CubeInstancers.IsValidIndex(CellValue) || !IsValidCell(X, Y))
	{
		return;
	}
	UInstancedStaticMeshComponent* Instancer = CubeInstancers[CellValue];
	if (!Instancer)
	{
		return;
	}

	const int32 CellIndex = Y * Width + X;
	TMap<int32, int32>& Map = CellToInstance[CellValue];
	const int32* Found = Map.Find(CellIndex);
	if (!Found)
	{
		return;
	}

	const int32 RemovedIndex = *Found;
	const int32 LastIndex = Instancer->GetInstanceCount() - 1;
	Map.Remove(CellIndex);

	// RemoveInstance swaps the last instance into the freed slot, so the inverse map has to follow
	// that instance (or the next edit would move the wrong bead). The inverse lookup is what makes
	// this O(1): scanning Map for the last index was the quadratic cost on big removals.
	TMap<int32, int32>& Inverse = InstanceToCell[CellValue];
	Inverse.Remove(RemovedIndex);

	if (RemovedIndex != LastIndex)
	{
		if (const int32* MovedCell = Inverse.Find(LastIndex))
		{
			const int32 MovedCellIndex = *MovedCell;
			Map[MovedCellIndex] = RemovedIndex;
			Inverse.Remove(LastIndex);
			Inverse.Add(RemovedIndex, MovedCellIndex);
		}
	}

	Instancer->RemoveInstance(RemovedIndex);
	OutChangedColors.Add(CellValue);
}

void ABeanGrid::RebuildInstancesForColors(const TSet<int32>& Colors)
{
	// Incremental path for placement/removal/batches: only the colors that changed are swept,
	// instead of all 48 instancers over the whole canvas.
	for (const int32 ColorIndex : Colors)
	{
		if (ColorIndex >= 0 && ColorIndex < Palette.Num())
		{
			RebuildColorInstances(ColorIndex);
		}
	}
}

void ABeanGrid::RebuildAllInstances()
{
	// Clear every instancer first, then do a single pass over the grid gathering
	// transforms per color. This avoids rebuilding each color with its own sweep
	// (48 * W * H iterations), which caused the "apply" hitch on big canvases.
	TArray<TArray<FTransform>> PerColor;
	TArray<TArray<int32>> PerColorCell;
	PerColor.SetNum(CubeInstancers.Num());
	PerColorCell.SetNum(CubeInstancers.Num());

	const float CubeScale = CellSize / 100.0f;
	const FVector Scale3D(CubeScale);
	const float OriginX = -(static_cast<float>(Width) - 1.0f) * 0.5f * CellSize;
	const float OriginY = -(static_cast<float>(Height) - 1.0f) * 0.5f * CellSize;
	const float WorldZ = CellSize * 0.5f;

	// Count per colour first: at 256x256 a full canvas means 65k transforms, and letting 48 arrays
	// grow into that (reallocating and copying ~4 MB several times each) dominated the rebuild.
	TArray<int32> PerColorCount;
	PerColorCount.Init(0, PerColor.Num());
	for (int32 CellIndex = 0; CellIndex < Cells.Num(); ++CellIndex)
	{
		const uint8 ColorIndex = Cells[CellIndex];
		if (ColorIndex != EmptyCell && ColorIndex < static_cast<uint8>(PerColor.Num()))
		{
			++PerColorCount[ColorIndex];
		}
	}
	for (int32 i = 0; i < PerColor.Num(); ++i)
	{
		PerColor[i].Reserve(PerColorCount[i]);
		PerColorCell[i].Reserve(PerColorCount[i]);
	}

	int32 CellIndex = 0;
	for (int32 Y = 0; Y < Height; ++Y)
	{
		const float WorldY = OriginY + static_cast<float>(Y) * CellSize;
		for (int32 X = 0; X < Width; ++X, ++CellIndex)
		{
			const uint8 ColorIndex = Cells[CellIndex];
			if (ColorIndex != EmptyCell && ColorIndex < static_cast<uint8>(PerColor.Num()))
			{
				// FQuat::Identity: the FRotator overload would run sin/cos for every bead.
				PerColor[ColorIndex].Emplace(FQuat::Identity, FVector(OriginX + static_cast<float>(X) * CellSize, WorldY, WorldZ), Scale3D);
				PerColorCell[ColorIndex].Add(CellIndex);
			}
		}
	}

	ResetCellInstanceMaps();

	for (int32 i = 0; i < CubeInstancers.Num(); ++i)
	{
		if (!CubeInstancers[i]) continue;
		CubeInstancers[i]->ClearInstances();
		if (PerColor[i].Num() > 0)
		{
			CubeInstancers[i]->AddInstances(PerColor[i], false, false, false);
		}
		// Added in sweep order, so instance i of this color is PerColorCell[i].
		if (CellToInstance.IsValidIndex(i))
		{
			TMap<int32, int32>& Map = CellToInstance[i];
			TMap<int32, int32>& Inverse = InstanceToCell[i];
			Map.Reset();
			Inverse.Reset();
			Map.Reserve(PerColorCell[i].Num());
			Inverse.Reserve(PerColorCell[i].Num());
			for (int32 InstanceIndex = 0; InstanceIndex < PerColorCell[i].Num(); ++InstanceIndex)
			{
				Map.Add(PerColorCell[i][InstanceIndex], InstanceIndex);
				Inverse.Add(InstanceIndex, PerColorCell[i][InstanceIndex]);
			}
		}
	}
}

void ABeanGrid::MarkDataChanged(bool bMarksDirty)
{
	++DataRevision;
	if (bMarksDirty) bDirty = true;
	const double T = FPlatformTime::Seconds();
	GridChangedDelegate.Broadcast();
	EditTiming.BroadcastUs += (FPlatformTime::Seconds() - T) * 1.0e6;
}

void ABeanGrid::MarkDataChangedAndRebuild(const TSet<int32>& ChangedColors, bool bMarksDirty)
{
	MarkDataChanged(bMarksDirty);
	RebuildInstancesForColors(ChangedColors);
}

// ---------------------------------------------------------------------------
// Ghost highlight + selection marquee
// ---------------------------------------------------------------------------

void ABeanGrid::UpdateGhost(int32 X, int32 Y, bool bValid)
{
	if (!GhostMesh) return;
	if (!bValid || !IsValidCell(X, Y))
	{
		GhostMesh->SetVisibility(false);
		return;
	}
	GhostMesh->SetVisibility(true);
	// Place on the cell; a small Z lift keeps the translucent bottom off the ground
	// and avoids z-fighting with the opaque ground plane.
	GhostMesh->SetWorldLocation(CellToWorld(X, Y) + FVector(0.0f, 0.0f, CellSize * 0.5f + 0.2f));
}

void ABeanGrid::SetGhostColor(const FLinearColor& Color)
{
	if (GhostMID)
	{
		GhostMID->SetVectorParameterValue(TEXT("Color"), Color);
	}
}

void ABeanGrid::SetGhostVisible(bool bVisible)
{
	if (GhostMesh)
	{
		GhostMesh->SetVisibility(bVisible);
	}
}

void ABeanGrid::SetSelection(int32 MinX, int32 MinY, int32 MaxX, int32 MaxY)
{
	SelectionMinX = FMath::Clamp(FMath::Min(MinX, MaxX), 0, FMath::Max(0, Width - 1));
	SelectionMaxX = FMath::Clamp(FMath::Max(MinX, MaxX), 0, FMath::Max(0, Width - 1));
	SelectionMinY = FMath::Clamp(FMath::Min(MinY, MaxY), 0, FMath::Max(0, Height - 1));
	SelectionMaxY = FMath::Clamp(FMath::Max(MinY, MaxY), 0, FMath::Max(0, Height - 1));
	UpdateSelectionVisual();
}

void ABeanGrid::ClearSelection()
{
	SelectionMaxX = -1;
	SelectionMaxY = -1;
	UpdateSelectionVisual();
}

bool ABeanGrid::HasSelection() const
{
	return SelectionMaxX >= SelectionMinX && SelectionMaxY >= SelectionMinY && Width > 0 && Height > 0;
}

void ABeanGrid::GetSelection(int32& OutMinX, int32& OutMinY, int32& OutMaxX, int32& OutMaxY) const
{
	OutMinX = SelectionMinX;
	OutMinY = SelectionMinY;
	OutMaxX = SelectionMaxX;
	OutMaxY = SelectionMaxY;
}

void ABeanGrid::UpdateSelectionVisual()
{
	if (!SelectionMesh) return;

	if (!HasSelection())
	{
		SelectionMesh->SetVisibility(false);
		return;
	}

	const int32 CellsX = SelectionMaxX - SelectionMinX + 1;
	const int32 CellsY = SelectionMaxY - SelectionMinY + 1;

	// Center of the selected rectangle in world space (the selection is drawn 1cm above the ground
	// so the translucent quad never z-fights with it, and just under the overlay's own 1cm lift).
	const FVector CornerA = CellToWorld(SelectionMinX, SelectionMinY);
	const FVector CornerB = CellToWorld(SelectionMaxX, SelectionMaxY);
	const FVector Center = (CornerA + CornerB) * 0.5f + FVector(0.0f, 0.0f, 1.05f);

	SelectionMesh->SetVisibility(true);
	SelectionMesh->SetWorldLocation(Center);
	// Slightly smaller than the covered cells so the beads stay visible inside the marquee.
	SelectionMesh->SetWorldScale3D(FVector(CellsX * CellSize / 100.0f * 0.98f, CellsY * CellSize / 100.0f * 0.98f, 1.0f));
}

// ---------------------------------------------------------------------------
// Image preview overlay
// ---------------------------------------------------------------------------

void ABeanGrid::BeginImagePreview(const TArray<uint8>& QuantizedCells, int32 InPatternWidth, int32 InPatternHeight)
{
	if (InPatternWidth <= 0 || InPatternHeight <= 0) return;
	if (QuantizedCells.Num() != InPatternWidth * InPatternHeight) return;

	PatternWidth = InPatternWidth;
	PatternHeight = InPatternHeight;
	PreviewGranularity = FMath::Max(InPatternWidth, InPatternHeight);

	// A pattern at full canvas size starts at 0,0; a smaller one is centered (the historical
	// behavior for a 32/64/128 import into the 256 canvas).
	PatternOriginX = FMath::Max(0, (Width - PatternWidth) / 2);
	PatternOriginY = FMath::Max(0, (Height - PatternHeight) / 2);

	PendingCells = QuantizedCells;

	// NOTE: applying a pattern must never throw away what the player already built. This used to
	// call ClearAllCells()/ClearHistory() here, which destroyed the whole work (and the undo history
	// that could have brought it back) the moment 应用 or 铺满画布 was pressed. The pattern is a
	// target to build towards, so the beads already on the board stay exactly where they are; the
	// fill operation then only adds the beads that are missing.

	// The pattern is what completion is measured against, even before the player builds anything.
	SyncTargetFromPending();

	bHasOverlay = true;
	bOverlayVisible = true;
	RebuildOverlayTexture();

	MarkDataChanged(true);
}

void ABeanGrid::BeginImagePreview(const TArray<uint8>& QuantizedCells, int32 Granularity)
{
	// Backwards-compatible square entry point.
	if (Granularity <= 0) return;
	BeginImagePreview(QuantizedCells, Granularity, Granularity);
}

void ABeanGrid::SyncTargetFromPending()
{
	TargetCells.Init(EmptyCell, Width * Height);
	TargetMask.Init(false, Width * Height);

	if (PendingCells.Num() != PatternWidth * PatternHeight || PatternWidth <= 0 || PatternHeight <= 0)
	{
		return;
	}

	for (int32 Y = 0; Y < PatternHeight; ++Y)
	{
		for (int32 X = 0; X < PatternWidth; ++X)
		{
			const uint8 ColorIndex = PendingCells[Y * PatternWidth + X];
			const int32 DstX = PatternOriginX + X;
			const int32 DstY = PatternOriginY + Y;
			if (!IsValidCell(DstX, DstY)) continue;

			const int32 Index = DstY * Width + DstX;
			TargetCells[Index] = ColorIndex;
			// Only cells that ask for a color count towards completion; an imported image's
			// transparent background stays out of the percentage entirely.
			TargetMask[Index] = (ColorIndex != EmptyCell && ColorIndex < static_cast<uint8>(Palette.Num()));
		}
	}
}

void ABeanGrid::RebuildOverlayTexture()
{
	if (!bHasOverlay || PendingCells.Num() != PatternWidth * PatternHeight) return;

	// Each block is painted with the palette colour it will be built with (the nearest of
	// the 48 palette colors for that pixel of the compressed image), one block per grid cell.
	// CreateOverlayTexture draws the blocks, a grid line on every block boundary and the 1-based
	// palette number in each block's corner over the whole canvas, so the pattern can sit anywhere.
	OverlayOpacity = 1.0f;

	TArray<FLinearColor> PaletteLinear;
	PaletteLinear.Reserve(Palette.Num());
	for (const FBeanPaletteColor& Item : Palette)
	{
		PaletteLinear.Add(Item.Color);
	}

	const int32 Total = Width * Height;
	TArray<FLinearColor> CellColors;
	TArray<bool> HasCell;
	TArray<int32> CellLabels;
	CellColors.SetNum(Total);
	HasCell.SetNum(Total);
	CellLabels.SetNum(Total);
	for (int32 i = 0; i < Total; ++i)
	{
		CellColors[i] = FLinearColor::Black;
		HasCell[i] = false;
		CellLabels[i] = 0;
	}

	for (int32 Y = 0; Y < PatternHeight; ++Y)
	{
		for (int32 X = 0; X < PatternWidth; ++X)
		{
			const uint8 ColorIndex = PendingCells[Y * PatternWidth + X];
			const int32 DstX = PatternOriginX + X;
			const int32 DstY = PatternOriginY + Y;
			if (!IsValidCell(DstX, DstY)) continue;
			if (ColorIndex == EmptyCell || ColorIndex >= static_cast<uint8>(Palette.Num())) continue;

			const int32 Index = DstY * Width + DstX;
			CellColors[Index] = Palette[ColorIndex].Color;
			HasCell[Index] = true;
			CellLabels[Index] = static_cast<int32>(ColorIndex) + 1;
		}
	}

	OverlayTexture = FBeanAssets::CreateOverlayTexture(CellColors, HasCell, CellLabels, Width, Height, PaletteLinear);
	if (OverlayMID)
	{
		OverlayMID->SetTextureParameterValue(TEXT("OverlayTex"), OverlayTexture);
		OverlayMID->SetScalarParameterValue(TEXT("Opacity"), OverlayOpacity);
	}
	if (OverlayMesh)
	{
		OverlayMesh->SetVisibility(bOverlayVisible);
	}
	UpdatePlaneTransforms();
}

void ABeanGrid::ApplyImage()
{
	if (!bHasOverlay) return;
	if (PendingCells.Num() != PatternWidth * PatternHeight) return;

	// Build the work as ONE undoable batch: the player can undo an import with a single Ctrl+Z.
	TArray<FBeanCellEdit> Edits;
	const int32 ColorCount = Palette.Num();
	for (int32 Y = 0; Y < PatternHeight; ++Y)
	{
		for (int32 X = 0; X < PatternWidth; ++X)
		{
			const uint8 ColorIndex = PendingCells[Y * PatternWidth + X];
			if (ColorIndex == EmptyCell || ColorIndex >= static_cast<uint8>(ColorCount)) continue;

			const int32 DstX = PatternOriginX + X;
			const int32 DstY = PatternOriginY + Y;
			if (!IsValidCell(DstX, DstY)) continue;

			FBeanCellEdit Edit;
			Edit.X = DstX;
			Edit.Y = DstY;
			Edit.Before = GetCell(DstX, DstY);
			Edit.After = ColorIndex;
			if (Edit.Before != Edit.After) Edits.Add(Edit);
		}
	}

	ApplyCellEdits(MoveTemp(Edits), TEXT("导入图案"), false);
	SyncTargetFromPending();
	MarkDataChanged(true);
}

void ABeanGrid::CancelImage()
{
	// Cancel does not touch the placed work: the preview is a hint layer, and discarding it must
	// leave the current work exactly as it was (the original code cleared the canvas here, which
	// destroyed the player's beads on a second import).
	PendingCells.Reset();
	bHasOverlay = false;
	bOverlayVisible = false;
	PatternWidth = 0;
	PatternHeight = 0;
	PatternOriginX = 0;
	PatternOriginY = 0;
	if (OverlayMesh)
	{
		OverlayMesh->SetVisibility(false);
	}
	ClearTargetPattern();
	MarkDataChanged(false);
}

void ABeanGrid::SetOverlayOpacity(float Opacity)
{
	OverlayOpacity = FMath::Clamp(Opacity, 0.1f, 1.0f);
	if (OverlayMID)
	{
		OverlayMID->SetScalarParameterValue(TEXT("Opacity"), OverlayOpacity);
	}
}

void ABeanGrid::SetOverlayVisible(bool bVisible)
{
	bOverlayVisible = bVisible;
	if (OverlayMesh)
	{
		OverlayMesh->SetVisibility(bVisible && bHasOverlay);
	}
}

void ABeanGrid::GetPatternRect(int32& OutMinX, int32& OutMinY, int32& OutMaxX, int32& OutMaxY) const
{
	OutMinX = PatternOriginX;
	OutMinY = PatternOriginY;
	OutMaxX = PatternOriginX + PatternWidth - 1;
	OutMaxY = PatternOriginY + PatternHeight - 1;
}

// ---------------------------------------------------------------------------
// Image export
// ---------------------------------------------------------------------------

const TArray<int32>& ABeanGrid::GetExportScaleOptions()
{
	static const TArray<int32> Options = { 4, 8, 16, 32 };
	return Options;
}

void ABeanGrid::GetPlacedCells(TArray<uint8>& OutCells) const
{
	OutCells = Cells;
}

void ABeanGrid::PlacePatternIntoGrid(TArray<uint8>& InOutCells) const
{
	// The hint layer's blocks are drawn only where no bead sits on them yet: after an Apply the
	// blocks are already real beads in Cells, so a bead the user has since removed stays removed.
	if (InOutCells.Num() != Width * Height) return;
	if (PendingCells.Num() != PatternWidth * PatternHeight) return;

	const int32 ColorCount = Palette.Num();
	for (int32 Y = 0; Y < PatternHeight; ++Y)
	{
		for (int32 X = 0; X < PatternWidth; ++X)
		{
			const uint8 ColorIndex = PendingCells[Y * PatternWidth + X];
			if (ColorIndex == EmptyCell || ColorIndex >= static_cast<uint8>(ColorCount)) continue;

			const int32 DstX = PatternOriginX + X;
			const int32 DstY = PatternOriginY + Y;
			if (!IsValidCell(DstX, DstY)) continue;

			const int32 Index = DstY * Width + DstX;
			if (InOutCells[Index] == EmptyCell)
			{
				InOutCells[Index] = ColorIndex;
			}
		}
	}
}

bool ABeanGrid::GetContentBounds(int32& OutMinX, int32& OutMinY, int32& OutMaxX, int32& OutMaxY) const
{
	OutMinX = Width;
	OutMinY = Height;
	OutMaxX = -1;
	OutMaxY = -1;

	TArray<uint8> ExportCells;
	ExportCells = Cells;
	if (bHasOverlay && PendingCells.Num() == PatternWidth * PatternHeight)
	{
		PlacePatternIntoGrid(ExportCells);
	}
	if (ExportCells.Num() != Width * Height)
	{
		return false;
	}

	// The exported picture only shows the bead work, not the whole canvas, so find the tightest
	// box that still contains every bead.
	for (int32 Y = 0; Y < Height; ++Y)
	{
		const uint8* Row = ExportCells.GetData() + Y * Width;
		for (int32 X = 0; X < Width; ++X)
		{
			if (Row[X] == EmptyCell) continue;
			OutMinX = FMath::Min(OutMinX, X);
			OutMaxX = FMath::Max(OutMaxX, X);
			OutMinY = FMath::Min(OutMinY, Y);
			OutMaxY = FMath::Max(OutMaxY, Y);
		}
	}

	return OutMaxX >= OutMinX && OutMaxY >= OutMinY;
}

bool ABeanGrid::BuildExportImage(int32 PixelsPerCell, FImage& OutImage, FString& OutError, bool bIncludeOverlay) const
{
	OutError.Reset();

	if (Cells.Num() != Width * Height)
	{
		OutError = TEXT("网格还没有准备好，请稍后再试。");
		return false;
	}

	// Work out what to draw first: the pure-work export leaves the (not yet built) hint layer out,
	// so a pattern that has not been applied is not part of that picture.
	TArray<uint8> ExportCells;
	ExportCells = Cells;
	if (bIncludeOverlay && bHasOverlay && PendingCells.Num() == PatternWidth * PatternHeight)
	{
		PlacePatternIntoGrid(ExportCells);
	}

	int32 MinX = Width;
	int32 MinY = Height;
	int32 MaxX = -1;
	int32 MaxY = -1;
	for (int32 Y = 0; Y < Height; ++Y)
	{
		const uint8* Row = ExportCells.GetData() + Y * Width;
		for (int32 X = 0; X < Width; ++X)
		{
			if (Row[X] == EmptyCell) continue;
			MinX = FMath::Min(MinX, X);
			MaxX = FMath::Max(MaxX, X);
			MinY = FMath::Min(MinY, Y);
			MaxY = FMath::Max(MaxY, Y);
		}
	}
	if (MaxX < MinX || MaxY < MinY)
	{
		OutError = bIncludeOverlay
			? TEXT("还没有拼豆可以导出：先放几个方块，或者用「导入图片」生成提示层。")
			: TEXT("还没有已放置的方块可以导出：请先拼出作品（或导出含提示层的画面）。");
		return false;
	}

	const int32 Scale = FMath::Max(1, PixelsPerCell);
	const int32 SizeX = (MaxX - MinX + 1) * Scale;
	const int32 SizeY = (MaxY - MinY + 1) * Scale;
	if (SizeX > MaxExportSize || SizeY > MaxExportSize)
	{
		OutError = FString::Printf(TEXT("导出图片太大（%d × %d，上限 %d），请选更小的比例。"), SizeX, SizeY, MaxExportSize);
		return false;
	}

	// The palette in linear space: the same colours the cubes and the hint layer are drawn with.
	TArray<FLinearColor> PaletteLinear;
	PaletteLinear.Reserve(Palette.Num());
	for (const FBeanPaletteColor& Item : Palette)
	{
		PaletteLinear.Add(Item.Color);
	}

	if (!FBeanAssets::CreateCellsImage(ExportCells, Width, Height, EmptyCell, PaletteLinear, MinX, MinY, MaxX, MaxY, Scale, OutImage))
	{
		OutError = TEXT("生成图片失败。");
		return false;
	}

	return true;
}

bool ABeanGrid::SaveImageToPng(int32 PixelsPerCell, const FString& FilePath, FString& OutError, bool bIncludeOverlay) const
{
	FImage Image;
	if (!BuildExportImage(PixelsPerCell, Image, OutError, bIncludeOverlay))
	{
		return false;
	}

	const FString Directory = FPaths::GetPath(FilePath);
	if (!Directory.IsEmpty() && !IFileManager::Get().DirectoryExists(*Directory))
	{
		IFileManager::Get().MakeDirectory(*Directory, true);
	}

	// Write to a temp file first, then move it over the target: a picture that never landed must
	// not leave a half-written PNG where a good one used to be.
	const FString TempPath = FilePath + BeanGridPrivate::TempSuffix;
	if (!FImageUtils::SaveImageByExtension(*TempPath, Image, 0))
	{
		OutError = FString::Printf(TEXT("写入图片失败：%s"), *FilePath);
		IFileManager::Get().Delete(*(FilePath + BeanGridPrivate::TempSuffix), false, true, true);
		return false;
	}
	if (!IFileManager::Get().Move(*FilePath, *TempPath, true, true, false, true))
	{
		OutError = FString::Printf(TEXT("保存图片失败：无法写入 %s"), *FilePath);
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("[Bean] Exported %d x %d px image (%d px per bead) to '%s'."), Image.SizeX, Image.SizeY, FMath::Max(1, PixelsPerCell), *FilePath);
	return true;
}

bool ABeanGrid::BuildThumbnailImage(int32 MaxEdge, FImage& OutImage) const
{
	// Thumbnails always show the whole work including the not-yet-built hint layer, because that is
	// what the work is meant to look like.
	int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
	if (!GetContentBounds(MinX, MinY, MaxX, MaxY))
	{
		return false;
	}

	const int32 CellsX = MaxX - MinX + 1;
	const int32 CellsY = MaxY - MinY + 1;
	const int32 Edge = FMath::Clamp(MaxEdge, 16, MaxExportSize);
	const int32 Scale = FMath::Max(1, Edge / FMath::Max(CellsX, CellsY));

	TArray<uint8> ExportCells;
	ExportCells = Cells;
	if (bHasOverlay && PendingCells.Num() == PatternWidth * PatternHeight)
	{
		PlacePatternIntoGrid(ExportCells);
	}

	TArray<FLinearColor> PaletteLinear;
	PaletteLinear.Reserve(Palette.Num());
	for (const FBeanPaletteColor& Item : Palette)
	{
		PaletteLinear.Add(Item.Color);
	}

	return FBeanAssets::CreateCellsImage(ExportCells, Width, Height, EmptyCell, PaletteLinear, MinX, MinY, MaxX, MaxY, Scale, OutImage);
}

// ---------------------------------------------------------------------------
// Save / load
// ---------------------------------------------------------------------------

FString ABeanGrid::GetSaveDirectory()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SaveGames"));
}

FString ABeanGrid::SanitizeFileName(const FString& InName)
{
	FString Result;
	Result.Reserve(InName.Len());
	for (const TCHAR Ch : InName)
	{
		const bool bIllegal = Ch == TEXT('<') || Ch == TEXT('>') || Ch == TEXT(':') || Ch == TEXT('"')
			|| Ch == TEXT('/') || Ch == TEXT('\\') || Ch == TEXT('|') || Ch == TEXT('?') || Ch == TEXT('*')
			|| Ch < 32;
		Result.AppendChar(bIllegal ? TEXT('_') : Ch);
	}
	Result.TrimStartAndEndInline();
	if (Result.Len() > 64)
	{
		Result.LeftInline(64);
	}
	return Result;
}

FString ABeanGrid::SanitizeSlotName(const FString& InName)
{
	const FString Clean = SanitizeFileName(InName);
	return Clean.IsEmpty() ? FString(TEXT("BeanGridSlot")) : Clean;
}

FString ABeanGrid::GetSlotFilePath(const FString& SlotName)
{
	return FPaths::Combine(GetSaveDirectory(), SanitizeSlotName(SlotName) + TEXT(".sav"));
}

bool ABeanGrid::DoesSlotExist(const FString& SlotName)
{
	return IFileManager::Get().FileExists(*GetSlotFilePath(SlotName));
}

void ABeanGrid::SetWorkName(const FString& InName)
{
	WorkSlotName = SanitizeSlotName(InName);
}

FString ABeanGrid::MakeUniqueWorkName(const FString& BaseName)
{
	FString Base = SanitizeFileName(BaseName);
	if (Base.IsEmpty())
	{
		Base = GetUntitledWorkName();
	}

	// The first free "名字", "名字 2", "名字 3", ... so a new canvas can never land on an existing
	// work and silently overwrite it on the next autosave.
	if (!DoesSlotExist(Base))
	{
		return Base;
	}
	for (int32 Suffix = 2; Suffix < 10000; ++Suffix)
	{
		const FString Candidate = FString::Printf(TEXT("%s %d"), *Base, Suffix);
		if (!DoesSlotExist(Candidate))
		{
			return Candidate;
		}
	}
	return FString::Printf(TEXT("%s %s"), *Base, *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
}

bool ABeanGrid::NewCanvas(int32 NewWidth, int32 NewHeight, FString& OutError)
{
	OutError.Reset();

	if (NewWidth <= 0 || NewHeight <= 0)
	{
		OutError = TEXT("新建失败：画布尺寸无效。");
		return false;
	}

	// ResizeGrid already empties the cells, the target pattern, the history and the selection, and
	// repoints the ground texture at the new cell count.
	if (!ResizeGrid(NewWidth, NewHeight))
	{
		OutError = TEXT("新建失败：无法创建该尺寸的画布。");
		return false;
	}

	// A name that is free on disk AND not the one we are leaving: neither canvas has been written
	// yet, so two "新建画布" presses in a row would otherwise both call themselves 未命名.
	FString NewName = MakeUniqueWorkName(GetUntitledWorkName());
	if (NewName == WorkSlotName)
	{
		for (int32 Suffix = 2; Suffix < 10000; ++Suffix)
		{
			const FString Candidate = FString::Printf(TEXT("%s %d"), *NewName, Suffix);
			if (Candidate != WorkSlotName && !DoesSlotExist(Candidate))
			{
				NewName = Candidate;
				break;
			}
		}
	}
	SetWorkName(NewName);

	// Nothing has been drawn on the new canvas yet, so it must not autosave itself into existence:
	// an untouched new work would otherwise litter the gallery with empty files.
	bDirty = false;
	SaveStatusMessage.Reset();
	bLastSaveFailed = false;
	MarkDataChanged(false);
	return true;
}

void ABeanGrid::BuildSaveData(UBeanGridSaveData& OutSave) const
{
	OutSave.Version = UBeanGridSaveData::CurrentVersion;
	OutSave.Width = Width;
	OutSave.Height = Height;
	OutSave.Cells = Cells;
	OutSave.TargetCells = TargetCells;
	OutSave.TargetMask = TargetMask;
	OutSave.PatternWidth = PatternWidth;
	OutSave.PatternHeight = PatternHeight;
	OutSave.PatternOriginX = PatternOriginX;
	OutSave.PatternOriginY = PatternOriginY;
	OutSave.CurrentColor = static_cast<uint8>(FMath::Clamp(CurrentColorIndex, 0, 255));
	OutSave.CanvasMode = static_cast<uint8>(CanvasMode);
	OutSave.WorkName = WorkSlotName;
	OutSave.SavedUtcTicks = FDateTime::UtcNow().GetTicks();
	OutSave.PaletteNames.Reset(Palette.Num());
	for (const FBeanPaletteColor& Item : Palette)
	{
		OutSave.PaletteNames.Add(Item.Name);
	}
}

int32 ABeanGrid::ValidateSaveData(const UBeanGridSaveData& Save) const
{
	if (Save.Version < 0 || Save.Version > UBeanGridSaveData::CurrentVersion)
	{
		return 1; // newer format written by a later build
	}

	// A legacy file (Version 0) always describes a 256x256 canvas.
	const int32 ExpectedW = Save.Width > 0 ? Save.Width : GridSize;
	const int32 ExpectedH = Save.Height > 0 ? Save.Height : GridSize;
	if (ExpectedW <= 0 || ExpectedH <= 0) return 2;
	if (static_cast<int64>(ExpectedW) * ExpectedH > 1024 * 1024) return 2;
	if (Save.Cells.Num() != ExpectedW * ExpectedH) return 3;

	const int32 ColorCount = Palette.Num();
	if (ColorCount > 0 && Save.CurrentColor >= static_cast<uint8>(ColorCount)) return 4;

	// The target arrays are optional (they only exist from format v2 on); if present they must fit.
	if (Save.TargetCells.Num() != 0 && Save.TargetCells.Num() != ExpectedW * ExpectedH) return 5;
	if (Save.TargetMask.Num() != 0 && Save.TargetMask.Num() != ExpectedW * ExpectedH) return 5;

	return -1;
}

void ABeanGrid::ApplySaveData(const UBeanGridSaveData& Save)
{
	const int32 NewW = Save.Width > 0 ? Save.Width : GridSize;
	const int32 NewH = Save.Height > 0 ? Save.Height : GridSize;
	const bool bSizeChanged = (NewW != Width || NewH != Height);

	Width = NewW;
	Height = NewH;
	Cells = Save.Cells;

	PatternWidth = Save.PatternWidth;
	PatternHeight = Save.PatternHeight;
	PatternOriginX = Save.PatternOriginX;
	PatternOriginY = Save.PatternOriginY;
	// Preview granularity is display-only, so derive it when the file does not carry it.
	PreviewGranularity = FMath::Max(PatternWidth, PatternHeight);
	if (PreviewGranularity <= 0) PreviewGranularity = FMath::Max(Width, Height);

	TargetCells = Save.TargetCells;
	TargetMask = Save.TargetMask;
	if (TargetCells.Num() != Width * Height) TargetCells.Init(EmptyCell, Width * Height);
	if (TargetMask.Num() != Width * Height) TargetMask.Init(false, Width * Height);

	CanvasMode = static_cast<EBeanCanvasMode>(Save.CanvasMode);
	if (CanvasMode != EBeanCanvasMode::Square && Width == Height) CanvasMode = EBeanCanvasMode::Square;

	// The applied work is the real data now; the hint layer stays empty until the next import.
	PendingCells.Reset();
	bHasOverlay = false;
	bOverlayVisible = false;
	if (OverlayMesh) OverlayMesh->SetVisibility(false);

	SetCurrentColor(Save.CurrentColor);
	ClearSelection();
	ClearHistory();

	if (!Save.WorkName.IsEmpty())
	{
		WorkSlotName = SanitizeSlotName(Save.WorkName);
	}

	// Palette drift check: cell values are palette indices, so a differently ordered palette would
	// silently recolor an old work. Detect it and say so instead of pretending everything is fine.
	if (Save.PaletteNames.Num() == Palette.Num())
	{
		for (int32 i = 0; i < Palette.Num(); ++i)
		{
			if (Save.PaletteNames[i] != Palette[i].Name)
			{
				UE_LOG(LogTemp, Warning, TEXT("[Bean] The palette changed since this work was saved (slot '%s', entry %d: '%s' -> '%s'); colors may map differently."),
					*WorkSlotName, i, *Save.PaletteNames[i], *Palette[i].Name);
				break;
			}
		}
	}

	if (bSizeChanged)
	{
		UpdatePlaneTransforms();
	}
	RebuildAllInstances();
	bDirty = false;
	MarkDataChanged(false);
}

bool ABeanGrid::WriteSaveDataAtomically(const FString& SlotName, const TArray<uint8>& Bytes, FString& OutError)
{
	OutError.Reset();

	if (SlotName.IsEmpty())
	{
		OutError = TEXT("作品名为空，无法保存。");
		return false;
	}
	if (Bytes.Num() == 0)
	{
		OutError = TEXT("保存失败：无法序列化存档数据。");
		return false;
	}

	const FString CleanSlot = SanitizeSlotName(SlotName);
	const FString FinalPath = GetSlotFilePath(CleanSlot);
	const FString TempPath = FinalPath + BeanGridPrivate::TempSuffix;
	const FString BackupPath = FinalPath + BeanGridPrivate::BackupSuffix;

	IFileManager& FileManager = IFileManager::Get();
	if (!FileManager.DirectoryExists(*GetSaveDirectory()))
	{
		FileManager.MakeDirectory(*GetSaveDirectory(), true);
	}

	// Always start from a clean temp file so a stale one from an earlier crash cannot be moved in.
	FileManager.Delete(*TempPath, false, true, true);
	if (!FFileHelper::SaveArrayToFile(Bytes, *TempPath))
	{
		OutError = FString::Printf(TEXT("保存失败：无法写入临时文件 %s"), *TempPath);
		return false;
	}

	// Sanity check before the swap: never replace a good save with an unreadable temp file.
	const int64 TempSize = FileManager.FileSize(*TempPath);
	if (TempSize <= 0 || TempSize != static_cast<int64>(Bytes.Num()))
	{
		FileManager.Delete(*TempPath, false, true, true);
		OutError = TEXT("保存失败：临时文件校验未通过，已保留原有存档。");
		return false;
	}

	if (FileManager.FileExists(*FinalPath))
	{
		// Keep the previous good file as the single backup. Move() refuses to overwrite, so the
		// destination is deleted first; that only ever removes the older backup (same slot).
		FileManager.Delete(*BackupPath, false, true, true);
		if (!FileManager.Move(*BackupPath, *FinalPath, true, true, false, true))
		{
			UE_LOG(LogTemp, Warning, TEXT("[Bean] Could not keep a backup of '%s'; the save itself continues."), *FinalPath);
		}
	}

	// Dest first, source second: IFileManager::Move(const TCHAR* Dest, const TCHAR* Src, ...).
	if (!FileManager.Move(*FinalPath, *TempPath, true, true, false, true))
	{
		// The temp file is now the only good copy, so it is deliberately left in place.
		OutError = FString::Printf(TEXT("保存失败：无法替换存档文件 %s"), *FinalPath);
		return false;
	}

	return true;
}

bool ABeanGrid::RenameStoredWork(const FString& SlotName, const FString& NewWorkName, FString& OutError)
{
	OutError.Reset();

	const FString CleanSlot = SanitizeSlotName(SlotName);
	UBeanGridSaveData* Save = Cast<UBeanGridSaveData>(UGameplayStatics::LoadGameFromSlot(CleanSlot, 0));
	if (!Save)
	{
		OutError = FString::Printf(TEXT("读不出存档「%s」，无法重命名。"), *CleanSlot);
		return false;
	}

	Save->WorkName = NewWorkName;

	// The rename goes through the same atomic writer as a normal save. Calling SaveGameToSlot here
	// instead would silently drop the temp-file/backup guarantees for exactly the write that is
	// most likely to be interrupted (the player is sitting in a menu).
	TArray<uint8> Bytes;
	if (!UGameplayStatics::SaveGameToMemory(Save, Bytes))
	{
		OutError = TEXT("重命名失败：无法序列化存档数据。");
		return false;
	}

	const bool bOk = WriteSaveDataAtomically(CleanSlot, Bytes, OutError);
	if (bOk)
	{
		// The slot keeps its name, so the work keeps its data, its thumbnail and its identity;
		// nothing is copied or deleted.
		UE_LOG(LogTemp, Log, TEXT("[Bean] Renamed work '%s' to '%s'."), *CleanSlot, *NewWorkName);
	}
	return bOk;
}

bool ABeanGrid::DeleteStoredWork(const FString& SlotName, FString& OutError)
{
	OutError.Reset();

	const FString CleanSlot = SanitizeSlotName(SlotName);
	const FString FinalPath = GetSlotFilePath(CleanSlot);
	const FString TempPath = FinalPath + BeanGridPrivate::TempSuffix;
	const FString BackupPath = FinalPath + BeanGridPrivate::BackupSuffix;

	IFileManager& FileManager = IFileManager::Get();
	bool bRemovedAny = false;

	// The backup and temp files belong to this slot, so they go with it - otherwise a deleted work
	// would quietly reappear as a "recovered" row the next time the slot name was reused.
	for (const FString& Path : { FinalPath, BackupPath, TempPath })
	{
		if (FileManager.FileExists(*Path))
		{
			if (FileManager.Delete(*Path, false, true, true))
			{
				bRemovedAny = true;
			}
			else
			{
				OutError = FString::Printf(TEXT("删除失败，文件可能正被占用：%s"), *Path);
				return false;
			}
		}
	}

	if (!bRemovedAny)
	{
		OutError = FString::Printf(TEXT("找不到作品「%s」的存档文件。"), *CleanSlot);
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("[Bean] Deleted the stored work '%s'."), *CleanSlot);
	return true;
}

bool ABeanGrid::SaveGameAs(const FString& SlotName, FString& OutError)
{
	OutError.Reset();

	if (SlotName.IsEmpty())
	{
		OutError = TEXT("作品名为空，无法保存。");
		SetSaveStatus(OutError, true);
		return false;
	}

	const FString CleanSlot = SanitizeSlotName(SlotName);
	UBeanGridSaveData* Save = NewObject<UBeanGridSaveData>();
	BuildSaveData(*Save);

	// The file has to carry the name it is being saved under, not the one the canvas happened to have
	// before. BuildSaveData stamps the current work name, and that is only updated below, so saving a
	// work as "名字" used to leave its previous auto-generated name inside the file - and the gallery
	// showed that instead of what the player typed (m05089).
	Save->WorkName = CleanSlot;

	// Serialize to memory and let the shared writer place the file. Relying on SaveGameToSlot here
	// would hand the file name to an engine-internal naming scheme ("<slot>.sav") that then has to
	// be undone again before the temp file can be renamed into place - that mismatch is exactly
	// what used to leave orphaned *.tmp.sav files behind and made every save after the first fail.
	TArray<uint8> Bytes;
	if (!UGameplayStatics::SaveGameToMemory(Save, Bytes) || Bytes.Num() == 0)
	{
		OutError = TEXT("保存失败：无法序列化存档数据。");
		SetSaveStatus(OutError, true);
		return false;
	}

	if (!WriteSaveDataAtomically(CleanSlot, Bytes, OutError))
	{
		SetSaveStatus(OutError, true);
		return false;
	}

	WorkSlotName = CleanSlot;
	bDirty = false;
	SetSaveStatus(FString::Printf(TEXT("已保存「%s」（%d × %d，%s）"), *CleanSlot, Width, Height, *FDateTime::Now().ToString(TEXT("%H:%M:%S"))), false);
	UE_LOG(LogTemp, Log, TEXT("[Bean] Saved work '%s' (%d x %d, %d cells)."), *CleanSlot, Width, Height, Cells.Num());
	return true;
}

void ABeanGrid::SaveGame()
{
	FString Error;
	SaveGameAs(WorkSlotName, Error);
}

bool ABeanGrid::AutoSaveIfDirty()
{
	if (!bDirty) return false;

	FString Error;
	const bool bOk = SaveGameAs(WorkSlotName, Error);
	if (bOk)
	{
		if (bLastSaveFailed)
		{
			// Only report a recovery once; the autosave runs every few seconds.
			UE_LOG(LogTemp, Log, TEXT("[Bean] Autosave recovered: '%s'."), *WorkSlotName);
			bLastSaveFailed = false;
		}
	}
	else
	{
		bLastSaveFailed = true;
		UE_LOG(LogTemp, Error, TEXT("[Bean] Autosave failed for '%s': %s"), *WorkSlotName, *Error);
	}
	return bOk;
}

bool ABeanGrid::LoadGameFrom(const FString& SlotName, FString& OutError)
{
	OutError.Reset();

	const FString CleanSlot = SanitizeSlotName(SlotName);
	const FString FinalPath = GetSlotFilePath(CleanSlot);
	const FString BackupPath = FinalPath + BeanGridPrivate::BackupSuffix;
	IFileManager& FileManager = IFileManager::Get();

	// Read the bytes ourselves instead of going through LoadGameFromSlot: that helper rebuilds the
	// file name from the slot name, which cannot address "work.sav.bak" (it would look for
	// "work.sav.sav"). Reading the exact path is what makes backup recovery actually work.
	auto ReadSaveFile = [](const FString& Path) -> UBeanGridSaveData*
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() == 0)
		{
			return nullptr;
		}
		return Cast<UBeanGridSaveData>(UGameplayStatics::LoadGameFromMemory(Bytes));
	};

	UBeanGridSaveData* Save = FileManager.FileExists(*FinalPath) ? ReadSaveFile(FinalPath) : nullptr;

	int32 Failure = Save ? ValidateSaveData(*Save) : -1;
	if (!Save)
	{
		Failure = 10; // missing or unreadable
	}

	bool bFromBackup = false;
	if (Failure != -1 && FileManager.FileExists(*BackupPath))
	{
		UBeanGridSaveData* Backup = ReadSaveFile(BackupPath);
		const int32 BackupFailure = Backup ? ValidateSaveData(*Backup) : 10;
		if (BackupFailure == -1)
		{
			Save = Backup;
			Failure = -1;
			bFromBackup = true;
		}
	}

	if (Failure != -1 || !Save)
	{
		switch (Failure)
		{
		case 3:
			OutError = FString::Printf(TEXT("存档「%s」数据不完整（方块数与画布尺寸不符），已跳过加载。"), *CleanSlot);
			break;
		case 1:
			OutError = FString::Printf(TEXT("存档「%s」由更新版本的游戏保存，当前版本无法读取。"), *CleanSlot);
			break;
		case 10:
			OutError = FString::Printf(TEXT("没有找到可读取的存档「%s」。"), *CleanSlot);
			break;
		default:
			OutError = FString::Printf(TEXT("存档「%s」损坏或版本不兼容（代码 %d），已跳过加载。"), *CleanSlot, Failure);
			break;
		}
		SetSaveStatus(OutError, true);
		UE_LOG(LogTemp, Warning, TEXT("[Bean] %s"), *OutError);
		return false;
	}

	ApplySaveData(*Save);

	if (bFromBackup)
	{
		SetSaveStatus(FString::Printf(TEXT("主存档损坏，已从备份恢复「%s」。"), *CleanSlot), true);
		// Keep the recovered data safe right away, which also refreshes .bak.
		FString SaveError;
		SaveGameAs(CleanSlot, SaveError);
	}
	else
	{
		SetSaveStatus(FString::Printf(TEXT("已加载「%s」（%d × %d）"), *CleanSlot, Width, Height), false);
	}
	UE_LOG(LogTemp, Log, TEXT("[Bean] Loaded work '%s' (%d x %d, %d cells)%s."), *CleanSlot, Width, Height, Cells.Num(), bFromBackup ? TEXT(" from backup") : TEXT(""));
	return true;
}

bool ABeanGrid::LoadGame()
{
	FString Error;
	if (LoadGameFrom(WorkSlotName, Error))
	{
		return true;
	}
	// A fresh install has no save yet: stay silent so the status line is not polluted with a
	// "not found" message the player did not cause. Anything else (corrupt/backup) keeps its text.
	if (!DoesSlotExist(WorkSlotName))
	{
		SaveStatusMessage.Reset();
		bLastSaveFailed = false;
		MarkDataChanged(false);
	}
	return false;
}

void ABeanGrid::SetSaveStatus(const FString& Message, bool bIsError)
{
	SaveStatusMessage = Message;
	bLastSaveFailed = bIsError;
	if (!Message.IsEmpty())
	{
		UE_LOG(LogTemp, Log, TEXT("[Bean] %s"), *Message);
	}
	MarkDataChanged(false);
}
