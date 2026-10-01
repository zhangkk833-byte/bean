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

/** Save data: full 256x256 grid + currently selected color. */
UCLASS()
class UBeanGridSaveData : public USaveGame
{
	GENERATED_BODY()

public:
	/** GridSize*GridSize bytes; value 255 = empty, otherwise a palette index. */
	UPROPERTY()
	TArray<uint8> Cells;

	UPROPERTY()
	uint8 CurrentColor = 1;
};

/**
 * The world: 256x256 ground grid, up to 48 instanced cube colors,
 * the image preview overlay, the placement ghost cube, and save/load.
 */
UCLASS()
class BEAN_API ABeanGrid : public AActor
{
	GENERATED_BODY()

public:
	ABeanGrid();

	static constexpr int32 GridSize = 256;
	// Cell size in UE units (cm): 0.25m per cell. The grid spans 64m x 64m.
	static constexpr float CellSize = 25.0f;
	static constexpr uint8 EmptyCell = 255;

	UPROPERTY(BlueprintReadOnly, Category = "Bean")
	TArray<FBeanPaletteColor> Palette;

	UPROPERTY(BlueprintReadOnly, Category = "Bean")
	int32 CurrentColorIndex = 1;

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

	// -- ghost highlight ------------------------------------------------------
	void UpdateGhost(int32 X, int32 Y, bool bValid);
	void SetGhostColor(const FLinearColor& Color);
	void SetGhostVisible(bool bVisible);

	// -- image preview overlay ------------------------------------------------
	/** Project the palette-quantized imported image onto the ground as a hint layer.
	 *  QuantizedCells holds the palette index per block (255 = empty) — the image was
	 *  compressed to Granularity x Granularity and each pixel snapped to the nearest of the
	 *  48 palette colors. The overlay magnifies each pixel onto one centre grid cell and
	 *  paints it with that palette color, slightly translucent, divided by grid lines and
	 *  labeled with its 1-based palette number. */
	void BeginImagePreview(const TArray<uint8>& QuantizedCells, int32 Granularity);
	/** Build the whole grid from the pending preview cells. */
	void ApplyImage();
	/** Remove the overlay and pending preview. */
	void CancelImage();
	bool HasOverlay() const { return bHasOverlay; }
	bool IsOverlayVisible() const { return bOverlayVisible; }
	float GetOverlayOpacity() const { return OverlayOpacity; }
	void SetOverlayOpacity(float Opacity);
	void SetOverlayVisible(bool bVisible);

	// -- save/load -------------------------------------------------------------
	void SaveGame();
	void LoadGame();

	// -- palette bar textures (generated in code) ------------------------------
	UPROPERTY(BlueprintReadOnly, Category = "Bean")
	TArray<UTexture2D*> CubeIconTextures;

protected:
	virtual void BeginPlay() override;

private:
	void InitializeGrid();
	void BuildComponents();
	void BuildGround();
	void BuildCubeInstancers();
	void BuildOverlayMesh();
	void BuildGhostMesh();
	void ApplyPaletteMaterials();
	void RebuildColorInstances(int32 ColorIndex);
	void RebuildAllInstances();

	UPROPERTY()
	TArray<uint8> Cells;

	UPROPERTY()
	TArray<uint8> PendingCells;

	/** Snapshot of Cells taken when a preview begins, so Cancel can undo an Apply. */
	UPROPERTY()
	TArray<uint8> CellsBackup;

	UPROPERTY()
	TArray<UMaterialInstanceDynamic*> CubeMIDs;

	UPROPERTY()
	TObjectPtr<USceneComponent> RootComp = nullptr;

	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> GroundMesh = nullptr;

	UPROPERTY()
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> CubeInstancers;

	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> OverlayMesh = nullptr;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> OverlayMID = nullptr;

	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> GhostMesh = nullptr;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> GhostMID = nullptr;

	UPROPERTY()
	TObjectPtr<UTexture2D> OverlayTexture = nullptr;

	/** Preview granularity chosen at import (32/64/128/256). */
	int32 PreviewGranularity = 256;

	/** Preview overlay opacity. Higher is more opaque (more accurate color). */
	float OverlayOpacity = 1.0f;
	bool bHasOverlay = false;
	bool bOverlayVisible = true;
	bool bInitialized = false;
};
