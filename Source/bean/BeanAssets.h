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
	/** Small fake-3D cube icon for the palette bar. */
	static UTexture2D* CreateCubeIconTexture(const FLinearColor& Color, int32 IconSize = 64);
	/** Granularity x Granularity overlay texture (N = 32/64/128/256); HasCell[i] false = transparent. Colors are linear-space palette colors. */
	static UTexture2D* CreateOverlayTexture(const TArray<FLinearColor>& CellColors, const TArray<bool>& HasCell, int32 Granularity);
};
