#pragma once

#include "CoreMinimal.h"

class UMaterial;
class UTexture2D;

/**
 * Runtime asset factory.
 *
 * All materials/textures used by the game are generated in code so the project
 * needs no hand-authored assets. Materials are created at runtime (they work in
 * the editor; before packaging run the "BeanBakeAssets" console command once to
 * save them as /Game/Bean assets, which the runtime code then loads instead).
 */
class BEAN_API FBeanAssets
{
public:
	// ---- Materials (load baked asset when present, else create at runtime) ----
	static UMaterial* GetCubeMaterial();
	static UMaterial* GetGroundMaterial();
	static UMaterial* GetOverlayMaterial();
	static UMaterial* GetGhostMaterial();

	/** Drop all cached runtime materials (so they are rebuilt / re-resolved). */
	static void ResetCache();

	/** Editor only: create and save the four materials under /Game/Bean. */
	static bool BakeAssets();

	// ---- Textures (always created at runtime; safe for packaged builds) ----
	static UTexture2D* CreateGridTexture();
	static UTexture2D* CreateCrosshairTexture();
	/** Small fake-3D cube icon for the palette bar. If OutFrontFaceCenter is set it receives
	 *  the normalized (0..1) centre of the cube's front face so a label can be anchored there. */
	static UTexture2D* CreateCubeIconTexture(const FLinearColor& Color, FVector2D* OutFrontFaceCenter = nullptr, int32 IconSize = 64);
	/** Full-grid hint-layer texture showing the Granularity x Granularity downscaled image in
	 *  the centre: one block per ground cell, painted with its palette color, with a grid line
	 *  along every block boundary. CellLabels[i] (1-based palette number, 0 = none) is drawn
	 *  small in the bottom-right corner of each block (size/stroke fixed, no outline) in the
	 *  opposite shade of the block fill — near-black on light blocks, near-white on dark ones,
	 *  near-opaque so it stays readable at any hint-layer opacity. HasCell[i] false leaves the
	 *  block transparent (its grid lines are still drawn). */
	static UTexture2D* CreateOverlayTexture(const TArray<FLinearColor>& CellColors, const TArray<bool>& HasCell,
		const TArray<int32>& CellLabels, int32 Granularity, const TArray<FLinearColor>& PaletteColors);
};
