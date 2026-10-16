#pragma once

#include "CoreMinimal.h"

class UMaterial;
class UTexture2D;
struct FImage;

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
	/**
	 * Ground grid texture for a canvas of CellsX x CellsY cells, generated once per cell count and
	 * then shared between grid actors of the same shape.
	 *
	 * The texture has to follow the canvas: one ground square = one cell (16 texels, 0.25m). A
	 * single 256-cell texture stretched over a 32-cell canvas drew 8 squares per cell, so the ground
	 * no longer matched the beads. Caching keeps the cost off the hot path (the 256-cell texture is
	 * 64MB and ~200ms of pixel work; a 32-cell one is 512x512 = 1MB), and only the most recently
	 * used shapes are kept, because the cache is what would otherwise grow without bound.
	 */
	static UTexture2D* CreateGridTexture(int32 CellsX = 256, int32 CellsY = 256);
	static UTexture2D* CreateCrosshairTexture();
	/** Small fake-3D cube icon for the palette bar. If OutFrontFaceCenter is set it receives
	 *  the normalized (0..1) centre of the cube's front face so a label can be anchored there. */
	static UTexture2D* CreateCubeIconTexture(const FLinearColor& Color, FVector2D* OutFrontFaceCenter = nullptr, int32 IconSize = 64);
	/** Canvas-sized hint-layer texture showing the imported image downscaled into the pattern box:
	 *  one block per ground cell, painted with its palette color, with a grid line along every
	 *  block boundary. CellColors/HasCell/CellLabels are Width*Height entries covering the WHOLE
	 *  canvas (the pattern may sit anywhere in it). CellLabels[i] (1-based palette number, 0 =
	 *  none) is drawn small in the bottom-right corner of each block (size/stroke fixed, no
	 *  outline) in the opposite shade of the block fill — near-black on light blocks, near-white
	 *  on dark ones, near-opaque so it stays readable at any hint-layer opacity. HasCell[i] false
	 *  leaves the block transparent (its grid lines are still drawn). The returned texture always
	 *  has a 1:1 cell-to-texel-block layout so the overlay plane can be scaled to any canvas. */
	static UTexture2D* CreateOverlayTexture(const TArray<FLinearColor>& CellColors, const TArray<bool>& HasCell,
		const TArray<int32>& CellLabels, int32 Width, int32 Height, const TArray<FLinearColor>& PaletteColors);

	/** Render the bead work as a plain picture for export: the Cells box [MinX,MaxX] x [MinY,MaxY]
	 *  (row-major, Width wide), each cell PixelsPerCell x PixelsPerCell pixels, on a warm
	 *  off-white board. EmptyValue cells (and out-of-range palette indices) keep the board colour,
	 *  neighbouring beads are separated by a proportional gap, and image row 0 is the lowest grid
	 *  Y — the same orientation the imported hint layer is drawn with. Returns false on bad input. */
	static bool CreateCellsImage(const TArray<uint8>& Cells, int32 Width, int32 Height, uint8 EmptyValue,
		const TArray<FLinearColor>& PaletteColors, int32 MinX, int32 MinY, int32 MaxX, int32 MaxY,
		int32 PixelsPerCell, FImage& OutImage);

	/** Wrap a picture built by CreateCellsImage in a texture so it can be shown on screen (the
	 *  export preview). Hard pixel edges are kept so the preview looks exactly like the saved file.
	 *  Returns nullptr when the image is empty. */
	static UTexture2D* CreateImageTexture(const FImage& Image);
};
