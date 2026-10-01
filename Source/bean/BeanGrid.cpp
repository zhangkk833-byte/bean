#include "BeanGrid.h"

#include "BeanAssets.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"

const FString BeanGridSlotName = TEXT("BeanGridSlot");

ABeanGrid::ABeanGrid()
{
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
	Cells.Init(EmptyCell, GridSize * GridSize);

	BuildComponents();
	ApplyPaletteMaterials();
	LoadGame();
}

void ABeanGrid::BuildComponents()
{
	BuildGround();
	BuildCubeInstancers();
	BuildOverlayMesh();
	BuildGhostMesh();
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
	// The Engine plane mesh is 100x100 units, so scale it to span the full grid.
	GroundMesh->SetWorldScale3D(FVector(GridSize * CellSize / 100.0f, GridSize * CellSize / 100.0f, 1.0f));
	GroundMesh->SetRelativeLocation(FVector::ZeroVector);

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

	CubeInstancers.Reset(Palette.Num());
	CubeMIDs.Reset(Palette.Num());
	for (int32 i = 0; i < Palette.Num(); ++i)
	{
		UInstancedStaticMeshComponent* Instancer = NewObject<UInstancedStaticMeshComponent>(this, *FString::Printf(TEXT("CubeInstancer_%d"), i));
		Instancer->SetMobility(EComponentMobility::Static);
		Instancer->SetupAttachment(RootComp);
		Instancer->RegisterComponent();

		if (CubeMesh)
		{
			Instancer->SetStaticMesh(CubeMesh);
		}
		// Cubes must not block the player/camera (no stepping up onto placed blocks — the
		// player walks/flys straight through them), but they keep blocking the placement
		// ray (Visibility channel) so aiming/removing stays precise.
		Instancer->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		if (CubeMaterial)
		{
			UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(CubeMaterial, this);
			MID->SetVectorParameterValue(TEXT("Color"), Palette[i].Color);
			Instancer->SetMaterial(0, MID);
			CubeMIDs.Add(MID);
		}

		CubeInstancers.Add(Instancer);
	}
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
	// Size/position are updated per-import in BeginImagePreview (covers the central
	// N x N cell region). Default to the full grid here.
	OverlayMesh->SetWorldScale3D(FVector(GridSize * CellSize / 100.0f, GridSize * CellSize / 100.0f, 1.0f));
	// Sit a clear distance above the ground plane so the translucent overlay doesn't
	// z-fight with it (a ~0 offset made it invisible).
	OverlayMesh->SetRelativeLocation(FVector(0.0f, 0.0f, 1.0f));
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
		GhostMesh->SetMaterial(0, GhostMID);
	}
	GhostMesh->SetVisibility(false);
}

void ABeanGrid::ApplyPaletteMaterials()
{
	for (int32 i = 0; i < Palette.Num(); ++i)
	{
		CubeIconTextures.Add(FBeanAssets::CreateCubeIconTexture(Palette[i].Color));
	}
}

// ---------------------------------------------------------------------------
// Cell access
// ---------------------------------------------------------------------------

bool ABeanGrid::IsValidCell(int32 X, int32 Y) const
{
	return X >= 0 && X < GridSize && Y >= 0 && Y < GridSize;
}

uint8 ABeanGrid::GetCell(int32 X, int32 Y) const
{
	if (!IsValidCell(X, Y)) return EmptyCell;
	return Cells[Y * GridSize + X];
}

bool ABeanGrid::IsCellOccupied(int32 X, int32 Y) const
{
	return GetCell(X, Y) != EmptyCell;
}

FVector ABeanGrid::CellToWorld(int32 X, int32 Y) const
{
	const float WorldX = (static_cast<float>(X) - 127.5f) * CellSize;
	const float WorldY = (static_cast<float>(Y) - 127.5f) * CellSize;
	return FVector(WorldX, WorldY, 0.0f);
}

bool ABeanGrid::WorldToCell(const FVector& WorldPos, int32& X, int32& Y) const
{
	const float HalfExtent = GridSize * CellSize * 0.5f; // 3200 (32m half-extent)
	X = FMath::FloorToInt((WorldPos.X + HalfExtent) / CellSize);
	Y = FMath::FloorToInt((WorldPos.Y + HalfExtent) / CellSize);
	return IsValidCell(X, Y);
}

void ABeanGrid::PlaceCell(int32 X, int32 Y)
{
	if (!IsValidCell(X, Y)) return;
	const int32 Index = Y * GridSize + X;
	if (Cells[Index] != EmptyCell) return; // occupied: ignore (right-click removes first)

	Cells[Index] = static_cast<uint8>(CurrentColorIndex);
	RebuildColorInstances(CurrentColorIndex);
}

void ABeanGrid::RemoveCell(int32 X, int32 Y)
{
	if (!IsValidCell(X, Y)) return;
	const int32 Index = Y * GridSize + X;
	const uint8 OldColor = Cells[Index];
	if (OldColor == EmptyCell) return;

	Cells[Index] = EmptyCell;
	RebuildColorInstances(OldColor);
}

void ABeanGrid::ClearAllCells()
{
	Cells.Init(EmptyCell, GridSize * GridSize);
}

void ABeanGrid::SetCurrentColor(int32 Index)
{
	if (Index < 0 || Index >= Palette.Num()) return;
	CurrentColorIndex = Index;
}

void ABeanGrid::CycleColor(int32 Delta)
{
	if (Palette.Num() == 0) return;
	CurrentColorIndex = (CurrentColorIndex + Delta + Palette.Num()) % Palette.Num();
}

int32 ABeanGrid::NearestColorIndex(const FLinearColor& Color) const
{
	return FBeanPalette::NearestColorIndex(Palette, Color);
}

// ---------------------------------------------------------------------------
// Instanced rendering
// ---------------------------------------------------------------------------

void ABeanGrid::RebuildColorInstances(int32 ColorIndex)
{
	if (ColorIndex < 0 || ColorIndex >= CubeInstancers.Num()) return;
	UInstancedStaticMeshComponent* Instancer = CubeInstancers[ColorIndex];
	if (!Instancer) return;

	Instancer->ClearInstances();

	// Collect transforms, then add them all in one batched call (much faster than
	// calling AddInstance once per cell, which is what caused the "apply" hitch).
	TArray<FTransform> Transforms;
	// Cube mesh is 100x100x100 units, so scale by CellSize/100 to match one cell (0.25m).
	const float CubeScale = CellSize / 100.0f;
	for (int32 Y = 0; Y < GridSize; ++Y)
	{
		for (int32 X = 0; X < GridSize; ++X)
		{
			const int32 Index = Y * GridSize + X;
			if (Cells[Index] == ColorIndex)
			{
				Transforms.Add(FTransform(FRotator::ZeroRotator, CellToWorld(X, Y) + FVector(0.0f, 0.0f, CellSize * 0.5f), FVector(CubeScale)));
			}
		}
	}
	if (Transforms.Num() > 0)
	{
		Instancer->AddInstances(Transforms, false, false, false);
	}
}

void ABeanGrid::RebuildAllInstances()
{
	// Clear every instancer first, then do a single pass over the grid gathering
	// transforms per color. This avoids rebuilding each color with its own 256x256
	// sweep (48 * 65536 iterations), which caused the "apply" hitch.
	TArray<TArray<FTransform>> PerColor;
	PerColor.SetNum(CubeInstancers.Num());

	const float CubeScale = CellSize / 100.0f;
	for (int32 Y = 0; Y < GridSize; ++Y)
	{
		for (int32 X = 0; X < GridSize; ++X)
		{
			const int32 Index = Y * GridSize + X;
			const uint8 ColorIndex = Cells[Index];
			if (ColorIndex != EmptyCell && ColorIndex < static_cast<uint8>(PerColor.Num()))
			{
				PerColor[ColorIndex].Add(FTransform(FRotator::ZeroRotator, CellToWorld(X, Y) + FVector(0.0f, 0.0f, CellSize * 0.5f), FVector(CubeScale)));
			}
		}
	}

	for (int32 i = 0; i < CubeInstancers.Num(); ++i)
	{
		if (!CubeInstancers[i]) continue;
		CubeInstancers[i]->ClearInstances();
		if (PerColor[i].Num() > 0)
		{
			CubeInstancers[i]->AddInstances(PerColor[i], false, false, false);
		}
	}
}

// ---------------------------------------------------------------------------
// Ghost highlight
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

// ---------------------------------------------------------------------------
// Image preview overlay
// ---------------------------------------------------------------------------

void ABeanGrid::BeginImagePreview(const TArray<uint8>& QuantizedCells, int32 Granularity)
{
	// Granularity must be a power-of-two divisor of GridSize (32/64/128/256).
	const int32 N = Granularity;
	if (N <= 0 || N > GridSize || (GridSize % N) != 0) return;
	if (QuantizedCells.Num() != N * N) return;

	PreviewGranularity = N;
	PendingCells = QuantizedCells;
	CellsBackup = Cells; // snapshot so Cancel can undo an Apply.

	// A new import begins a fresh preview: clear any cubes placed by a previously
	// applied image so the new overlay isn't shown over stale blocks.
	ClearAllCells();
	RebuildAllInstances();

	bHasOverlay = true;
	bOverlayVisible = true;

	// Each block is painted with the palette colour it will be built with (the nearest of
	// the 48 palette colors for that pixel of the compressed image), one block per grid
	// cell. CreateOverlayTexture draws the blocks, a grid line on every block boundary and
	// the 1-based palette number in each block's corner. Opacity is baked per-texel inside
	// the texture (light blocks 0.8, dark blocks 1.0), so the global scalar starts at 1.0 —
	// the "透明度±" slider still scales the whole layer from there.
	OverlayOpacity = 1.0f;

	// Palette colors in linear space, used to tint the blocks and their corner numbers.
	TArray<FLinearColor> PaletteLinear;
	PaletteLinear.Reserve(Palette.Num());
	for (const FBeanPaletteColor& Item : Palette)
	{
		PaletteLinear.Add(Item.Color);
	}

	TArray<FLinearColor> CellColors;
	TArray<bool> HasCell;
	TArray<int32> CellLabels;
	CellColors.SetNum(N * N);
	HasCell.SetNum(N * N);
	CellLabels.SetNum(N * N);
	for (int32 i = 0; i < N * N; ++i)
	{
		const uint8 ColorIndex = QuantizedCells[i];
		if (ColorIndex != EmptyCell && ColorIndex < static_cast<uint8>(Palette.Num()))
		{
			CellColors[i] = Palette[ColorIndex].Color;
			HasCell[i] = true;
			CellLabels[i] = static_cast<int32>(ColorIndex) + 1;
		}
		else
		{
			CellColors[i] = FLinearColor::Black;
			HasCell[i] = false;
			CellLabels[i] = 0;
		}
	}

	OverlayTexture = FBeanAssets::CreateOverlayTexture(CellColors, HasCell, CellLabels, N, PaletteLinear);
	if (OverlayMID)
	{
		OverlayMID->SetTextureParameterValue(TEXT("OverlayTex"), OverlayTexture);
		OverlayMID->SetScalarParameterValue(TEXT("Opacity"), OverlayOpacity);
	}
	if (OverlayMesh)
	{
		// The overlay texture spans the full grid (one cell per block, data centered),
		// so the plane always covers the whole grid.
		OverlayMesh->SetWorldScale3D(FVector(GridSize * CellSize / 100.0f, GridSize * CellSize / 100.0f, 1.0f));
		OverlayMesh->SetRelativeLocation(FVector(0.0f, 0.0f, 1.0f)); // centered, just above ground
		OverlayMesh->SetVisibility(bOverlayVisible);
	}
}

void ABeanGrid::ApplyImage()
{
	if (!bHasOverlay) return;
	const int32 N = PreviewGranularity;
	if (N <= 0 || N > GridSize || (GridSize % N) != 0) return;
	if (PendingCells.Num() != N * N) return;

	ClearAllCells();

	// Place the image into the CENTER N x N cells of the 256-cell grid. Each image
	// pixel maps 1:1 to a grid cell (cell size stays 25cm; the grid count stays 256).
	const int32 Start = (GridSize - N) / 2;
	for (int32 Y = 0; Y < N; ++Y)
	{
		for (int32 X = 0; X < N; ++X)
		{
			const uint8 ColorIndex = PendingCells[Y * N + X];
			if (ColorIndex == EmptyCell || ColorIndex >= static_cast<uint8>(Palette.Num()))
			{
				continue;
			}
			Cells[(Start + Y) * GridSize + (Start + X)] = ColorIndex;
		}
	}
	RebuildAllInstances();
}

void ABeanGrid::CancelImage()
{
	// Cancel removes the cubes created by this import's Apply instead of restoring
	// a snapshot (which resurrected the previously-applied image on a 2nd import).
	ClearAllCells();
	RebuildAllInstances();
	CellsBackup.Reset();
	PendingCells.Reset();
	bHasOverlay = false;
	bOverlayVisible = false;
	if (OverlayMesh)
	{
		OverlayMesh->SetVisibility(false);
	}
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
		OverlayMesh->SetVisibility(bVisible);
	}
}

// ---------------------------------------------------------------------------
// Save / load
// ---------------------------------------------------------------------------

void ABeanGrid::SaveGame()
{
	UBeanGridSaveData* Save = NewObject<UBeanGridSaveData>();
	Save->Cells = Cells;
	Save->CurrentColor = static_cast<uint8>(CurrentColorIndex);
	UGameplayStatics::SaveGameToSlot(Save, BeanGridSlotName, 0);
	UE_LOG(LogTemp, Log, TEXT("[Bean] Grid saved (%d cells)."), Cells.Num());
}

void ABeanGrid::LoadGame()
{
	USaveGame* Raw = UGameplayStatics::LoadGameFromSlot(BeanGridSlotName, 0);
	UBeanGridSaveData* Save = Cast<UBeanGridSaveData>(Raw);
	if (!Save || Save->Cells.Num() != GridSize * GridSize)
	{
		return;
	}

	Cells = Save->Cells;
	SetCurrentColor(Save->CurrentColor);
	RebuildAllInstances();
	UE_LOG(LogTemp, Log, TEXT("[Bean] Grid loaded from slot."));
}
